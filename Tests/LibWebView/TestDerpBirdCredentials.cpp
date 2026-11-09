#include "HeadlessTest.h"
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <AK/StringUtils.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentPage.h>

namespace {

// Task-local HTTP origin for native credential IPC. No external site or keyring.
class CredentialHTTPServer {
public:
    CredentialHTTPServer()
        : m_server(MUST(Core::TCPServer::try_create()))
    {
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_connections.append(make<Connection>(move(socket)));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [&connection] {
                u8 buffer[4096];
                auto received = connection.socket->read_some(buffer);
                if (received.is_error() || received.value().is_empty()) {
                    connection.socket->on_ready_to_read = nullptr;
                    return;
                }
                connection.request.append(received.value());
                if (!StringView { connection.request.bytes() }.contains("\r\n\r\n"sv))
                    return;
                connection.socket->on_ready_to_read = nullptr;
                auto body = "<!doctype html><form><input id=u autocomplete=username><input id=p type=password autocomplete=current-password></form>"sv;
                auto response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.length(), body);
                MUST(connection.socket->set_blocking(true));
                MUST(connection.socket->write_until_depleted(response.bytes()));
                connection.socket->close();
            };
        };
    }
    ByteString endpoint() const { return ByteString::formatted("http://127.0.0.1:{}", *m_server->local_port()); }

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;
        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer request {};
    };
    NonnullRefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
};

}

TEST_CASE(derpbird_credential_ipc_selects_fills_and_rejects_replay)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdCredentials"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    CredentialHTTPServer server;
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    bool loaded = false, crashed = false;
    auto url = URL::Parser::basic_parse(server.endpoint()).release_value();
    view->on_web_content_crashed = [&](auto) { crashed = true; };
    view->on_load_finish = [&](auto const& address) { loaded = address == url; };
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return loaded || crashed; });
    VERIFY(!crashed);
    auto check = [&](String script, StringView expected) {
        Optional<String> actual;
        view->on_console_message = [&](WebView::ConsoleOutput message) {
            if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string())
                actual = log->arguments[0].as_string();
        };
        view->start_sending_console_output(true, true);
        view->run_javascript(move(script));
        Core::EventLoop::current().spin_until([&] { return actual.has_value() || crashed; });
        VERIFY(!crashed);
        EXPECT_EQ(actual->bytes_as_string_view(), expected);
        view->on_console_message = nullptr;
    };
    check("document.getElementById('p').focus(); console.log(document.activeElement.id)"_string, "p"sv);
    auto document = view->page().traversable().active_document().id();
    u64 request = 0, received = 0;
    JsonValue reply;
    Optional<String> error;
    view->page().on_derpbird_credential_result = [&](auto id, u64 correlation, JsonValue value, Optional<String> failure) {
        EXPECT(id == document);
        received = correlation;
        reply = move(value);
        error = move(failure);
    };
    auto wait = [&] {
        Core::EventLoop::current().spin_until([&] { return received == request || crashed; });
        VERIFY(!crashed);
    };
    auto select = [&] {
        view->page().async_request_derpbird_credential_fields(document, ++request);
        wait();
        VERIFY(!error.has_value());
        VERIFY(reply.is_object());
        EXPECT_EQ(reply.as_object().get_string("origin"sv).value(), url.origin().serialize());
        return reply.as_object().get_string("ticket"sv).value().to_number<u64>().value();
    };
    auto fill = [&](u64 ticket, String const& origin) {
        view->page().async_fill_derpbird_credential_fields(document, ++request, ticket, origin, "ipc-user"_string, "ipc-fixture"_string);
        wait();
    };
    auto capture = [&](u64 ticket, String const& origin) {
        view->page().async_capture_derpbird_credential_fields(document, ++request, ticket, origin);
        wait();
    };
    auto ticket = select();
    fill(ticket, url.origin().serialize());
    EXPECT(!error.has_value());
    EXPECT(reply.is_null());
    check("console.log(document.getElementById('u').value+':'+document.getElementById('p').value)"_string, "ipc-user:ipc-fixture"sv);
    fill(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    ticket = select();
    capture(ticket, url.origin().serialize());
    VERIFY(!error.has_value() && reply.is_object());
    EXPECT_EQ(reply.as_object().get_string("username"sv).value(), "ipc-user"sv);
    EXPECT_EQ(reply.as_object().get_string("password"sv).value(), "ipc-fixture"sv);
    EXPECT_EQ(reply.as_object().get_string("origin"sv).value(), url.origin().serialize());
    capture(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    check("document.getElementById('p').autocomplete='new-password';document.getElementById('p').value='registration-fixture';console.log('registration')"_string, "registration"sv);
    ticket = select();
    capture(ticket, url.origin().serialize());
    VERIFY(!error.has_value() && reply.is_object());
    EXPECT_EQ(reply.as_object().get_string("password"sv).value(), "registration-fixture"sv);
    check("document.getElementById('p').autocomplete='current-password';console.log('login')"_string, "login"sv);
    ticket = select();
    capture(ticket, "https://other.example"_string);
    EXPECT(error.has_value());
    capture(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    ticket = select();
    check("document.getElementById('p').style.visibility='hidden';console.log('hidden-for-capture')"_string, "hidden-for-capture"sv);
    capture(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    check("document.getElementById('p').style.visibility='visible';document.getElementById('p').focus();console.log('visible-again')"_string, "visible-again"sv);
    auto old = select();
    auto newer = select();
    fill(old, url.origin().serialize());
    EXPECT(error.has_value());
    fill(newer, url.origin().serialize());
    EXPECT(!error.has_value());
    ticket = select();
    fill(ticket, "https://other.example"_string);
    EXPECT(error.has_value());
    fill(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    ticket = select();
    view->page().async_cancel_derpbird_credential_fields(document, ticket);
    fill(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    ticket = select();
    check("document.querySelector('form').style.opacity='0';console.log('hidden')"_string, "hidden"sv);
    fill(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    view->page().async_request_derpbird_credential_fields(document, ++request);
    wait();
    EXPECT(error.has_value());
    check("document.querySelector('form').style.opacity='1';document.getElementById('p').focus();console.log('visible')"_string, "visible"sv);
    ticket = select();
    check("document.getElementById('u').addEventListener('input',()=>document.getElementById('p').remove(),{once:true});console.log('armed')"_string, "armed"sv);
    fill(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    check("document.querySelector('form').insertAdjacentHTML('beforeend','<input id=p type=password autocomplete=current-password>');document.getElementById('p').focus();console.log('restored')"_string, "restored"sv);
    size_t submissions = 0;
    String submitted_username, submitted_password;
    view->on_derpbird_credential_submission = [&](auto& source, String username, String password) {
        EXPECT(source.id() == document);
        EXPECT_EQ(source.origin().serialize(), url.origin().serialize());
        ++submissions;
        submitted_username = move(username);
        submitted_password = move(password);
    };
    check("document.querySelector('form').addEventListener('submit',e=>{e.preventDefault();document.getElementById('p').value=''});document.getElementById('u').value='submitted-user';document.getElementById('p').value='submission-fixture';document.querySelector('form').requestSubmit();console.log('without-activation')"_string, "without-activation"sv);
    EXPECT_EQ(submissions, 0u);
    view->page().notify_about_user_activation(document);
    check("document.getElementById('p').value='submission-fixture';document.querySelector('form').requestSubmit();console.log('submitted')"_string, "submitted"sv);
    EXPECT_EQ(submissions, 1u);
    EXPECT_EQ(submitted_username, "submitted-user"sv);
    EXPECT_EQ(submitted_password, "submission-fixture"sv);
    check("console.log(document.getElementById('p').value===''?'cleared':'wrong')"_string, "cleared"sv);
    check("for(let i=0;i<20;i++){document.getElementById('p').value='repeat';document.querySelector('form').requestSubmit()}console.log('repeated')"_string, "repeated"sv);
    EXPECT_EQ(submissions, 1u); // One IPC per activation, even when script loops.
    view->page().notify_about_user_activation(document);
    check("document.getElementById('p').value='next-fixture';document.querySelector('form').requestSubmit();console.log('new-activation')"_string, "new-activation"sv);
    EXPECT_EQ(submissions, 2u);
    EXPECT_EQ(submitted_password, "next-fixture"sv);
    view->page().notify_about_user_activation(document);
    check("document.getElementById('p').value='invalid-fixture';document.querySelector('form').insertAdjacentHTML('beforeend','<input required>');document.querySelector('form').requestSubmit();console.log('invalid-form')"_string, "invalid-form"sv);
    EXPECT_EQ(submissions, 2u);
    // Synthetic DOM events cannot enter the native form-submission hook.
    check("document.querySelector('form').dispatchEvent(new Event('submit',{cancelable:true}));console.log('synthetic')"_string, "synthetic"sv);
    EXPECT_EQ(submissions, 2u);
    check("document.getElementById('p').focus();console.log('restore-password-focus')"_string, "restore-password-focus"sv);
    ticket = select();
    auto old_document = document;
    loaded = false;
    url = URL::Parser::basic_parse(MUST(String::formatted("{}/next", server.endpoint()))).release_value();
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return loaded || crashed; });
    VERIFY(!crashed);
    document = view->page().traversable().active_document().id();
    EXPECT(document != old_document);
    check("document.getElementById('p').focus();console.log('new-document')"_string, "new-document"sv);
    // The stale result must be dropped by the host document gate. The subsequent
    // current-document reply is also a barrier proving the old command was read.
    view->page().async_fill_derpbird_credential_fields(old_document, ++request, ticket, url.origin().serialize(), "old"_string, "must-not-fill"_string);
    view->page().async_capture_derpbird_credential_fields(old_document, ++request, ticket, url.origin().serialize());
    auto current_ticket = select();
    fill(ticket, url.origin().serialize());
    EXPECT(error.has_value());
    fill(current_ticket, url.origin().serialize());
    EXPECT(!error.has_value());
    view->page().did_submit_derpbird_credential(old_document, "stale"_string, "must-not-prompt"_string);
    EXPECT_EQ(submissions, 2u);
    view->on_derpbird_credential_submission = nullptr;
    view->page().on_derpbird_credential_result = nullptr;
}
