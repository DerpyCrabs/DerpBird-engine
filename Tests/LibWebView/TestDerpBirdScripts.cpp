#include "HeadlessTest.h"
#include <LibWebView/WebContentTestClient.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
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

// Task-local loopback fixture. Every path serves the same probe; URL matching
// must distinguish /match from /other without relying on their HTML contents.
class ScriptHTTPServer {
public:
    explicit ScriptHTTPServer(Function<ByteString(StringView)> handler = {})
        : m_server(MUST(Core::TCPServer::try_create()))
        , m_handler(move(handler))
    {
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_connections.append(make<Connection>(move(socket)));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [this, &connection] {
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
                auto body = R"HTML(<!doctype html><script>
                    window.fixturePageValue = 97;
                    document.documentElement.setAttribute('data-script-page-start', window.derpBirdPageStart === 1 && typeof derpBirdLocal === 'undefined' ? 'before' : 'missing');
                    document.dispatchEvent(new Event('page-first-script'));
                    addEventListener('DOMContentLoaded',()=>document.documentElement.setAttribute('data-page-dcl','yes'));
                </script><body><div id=probe>Script lifecycle fixture</div></body>)HTML"sv;
                if (m_handler) warnln("Request fixture: {}", StringView { connection.request.bytes() }.substring_view(0, StringView { connection.request.bytes() }.find("\r\n"sv).value_or(0)));
                auto selected_body = m_handler ? m_handler(StringView { connection.request.bytes() }) : ByteString { body };
                auto response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", selected_body.length(), selected_body);
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
    Function<ByteString(StringView)> m_handler;
};

}

TEST_CASE(derpbird_scripts_reach_renderer_and_run_at_document_phases)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdScripts"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    int closed_private_values = 0;
    {
        auto session = WebView::BrowsingSession::create(WebView::IsPrivate::Yes);
        session->on_derpbird_values_close = [&] { ++closed_private_values; };
        EXPECT_EQ(closed_private_values, 0);
    }
    EXPECT_EQ(closed_private_values, 1);
    size_t host_registry_updates = 0;
    app->on_derpbird_scripts_changed = [&](auto const& snapshot) {
        EXPECT(snapshot.is_array());
        ++host_registry_updates;
    };
    ScriptHTTPServer server;
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    bool crashed = false;
    bool expired = false;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [&] { expired = true; });
    auto load = [&](WebView::HeadlessWebView& view, StringView path) {
        bool loaded = false;
        expired = false;
        view.on_web_content_crashed = [&](auto) { crashed = true; };
        view.on_load_finish = [&](auto const&) { loaded = true; };
        auto url = URL::Parser::basic_parse(MUST(String::formatted("{}{}", server.endpoint(), path)));
        VERIFY(url.has_value());
        deadline->start();
        view.load(*url);
        Core::EventLoop::current().spin_until([&] { return loaded || crashed || expired; });
        deadline->stop();
        VERIFY(!crashed && !expired);
        view.on_load_finish = nullptr;
    };
    auto check = [&](WebView::HeadlessWebView& view, bool installed) {
        Optional<String> result;
        expired = false;
        view.on_console_message = [&](WebView::ConsoleOutput message) {
            if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string()) {
                auto text = log->arguments[0].as_string();
                if (text.bytes_as_string_view().starts_with("DERPBIRD:"sv))
                    result = move(text);
            }
        };
        view.start_sending_console_output(true, true);
        auto script = installed
            ? R"JS((()=>{const poll=()=>{const d=document.documentElement;if(!d.hasAttribute('data-script-idle')){setTimeout(poll,5);return} console.log('DERPBIRD:'+['start','body','end','idle','page-start','page-end','isolated'].map(x=>d.getAttribute('data-script-'+x)).concat(getComputedStyle(d).getPropertyValue('--derpbird-gm-style').trim(),document.getElementById('derpbird-gm-async-style')?.getAttribute('data-style-promise'),typeof GM_addStyle,d.getAttribute('data-unsafe-window'),window.derpbirdUnsafeExport).join('|'))};poll()})())JS"sv
            : R"JS(console.log('DERPBIRD:'+['start','body','end','idle'].every(x=>!document.documentElement.hasAttribute('data-script-'+x))))JS"sv;
        deadline->start();
        view.run_javascript(MUST(String::from_utf8(script)));
        Core::EventLoop::current().spin_until([&] { return result.has_value() || crashed || expired; });
        deadline->stop();
        VERIFY(!crashed && !expired);
        EXPECT_EQ(*result, installed ? "DERPBIRD:before|present|before|after|before|97|isolated|active|resolved|undefined|bridged|42"sv : "DERPBIRD:true"sv);
        view.on_console_message = nullptr;
    };
    JsonArray records;
    auto add = [&](StringView id, StringView phase, StringView source, bool grant_none = false) {
        auto metadata = MUST(JsonValue::from_string(R"JSON({"runAt":"document-start","grantNone":false,"noFrames":false,"matches":[],"includes":[],"excludeMatches":[],"excludes":[],"grants":[],"requires":[],"connects":[],"resources":[]})JSON"sv)).as_object();
        JsonArray patterns;
        MUST(patterns.append(MUST(String::formatted("{}/match*", server.endpoint()))));
        metadata.set("matches"sv, move(patterns));
        metadata.set("runAt"sv, phase);
        metadata.set("grantNone"sv, grant_none);
        JsonObject record;
        record.set("id"sv, id);
        record.set("name"sv, id);
        record.set("source"sv, source);
        record.set("enabled"sv, true);
        record.set("fileAccessGranted"sv, false);
        metadata.set("entries"sv, MUST(JsonValue::from_string(R"JSON([{"key":"sandbox","value":"DOM"}])JSON"sv)));
        record.set("metadata"sv, move(metadata));
        MUST(records.append(move(record)));
    };
    add("start"sv, "document-start"sv, R"JS(const early=!document.body; document.addEventListener('page-first-script',()=>document.documentElement.setAttribute('data-script-start',early?'before':'late')); document.addEventListener('revocation-probe',()=>document.documentElement.setAttribute('data-revocation',String(Number(document.documentElement.getAttribute('data-revocation')||0)+1))))JS"sv);
    add("body"sv, "document-body"sv, R"JS(document.documentElement.setAttribute('data-script-body',document.body?'present':'missing'))JS"sv);
    add("end"sv, "document-end"sv, R"JS(document.documentElement.setAttribute('data-script-end',document.documentElement.hasAttribute('data-page-dcl')?'late':'before'))JS"sv);
    add("idle"sv, "document-idle"sv, R"JS(document.documentElement.setAttribute('data-script-idle',document.documentElement.hasAttribute('data-page-dcl')?'after':'early'))JS"sv);
    add("page-start"sv, "document-start"sv, "if(GM_info.script.name !== 'page-start' || GM.info !== GM_info) throw new Error('bad page info'); var derpBirdLocal = 12; window.derpBirdPageStart = (window.derpBirdPageStart || 0) + 1; return; throw new Error('unreachable');"sv, true);
    add("page-end"sv, "document-end"sv, "document.documentElement.setAttribute('data-script-page-end', String(fixturePageValue));"sv, true);
    add("isolated"sv, "document-end"sv, "document.documentElement.setAttribute('data-script-isolated', GM_info.script.name === 'isolated' && GM.info === GM_info && typeof fixturePageValue === 'undefined' && typeof derpBirdPageStart === 'undefined' ? 'isolated' : 'leaked');"sv);
    add("unsafe-window"sv, "document-end"sv, "document.documentElement.setAttribute('data-unsafe-window', unsafeWindow.fixturePageValue === 97 && unsafeWindow.window === unsafeWindow && typeof fixturePageValue === 'undefined' ? 'bridged' : 'failed'); unsafeWindow.derpbirdUnsafeExport = 42;"sv);
    {
        auto record = records.at(records.size() - 1).as_object();
        auto metadata = record.get_object("metadata"sv).value();
        JsonArray grants;
        grants.must_append("unsafeWindow"sv);
        metadata.set("grants"sv, move(grants));
        record.set("metadata"sv, move(metadata));
        records.set(records.size() - 1, move(record));
    }
    add("style"sv, "document-start"sv, R"JS(
        if (typeof GM_addStyle !== 'undefined') throw new Error('ungranted synchronous alias');
        const result = GM.addStyle(':root { --derpbird-gm-style: active; }');
        if (!(result instanceof Promise)) throw new Error('missing style promise');
        result.then(style => {
            if (!(style instanceof HTMLStyleElement)) throw new Error('missing style result');
            style.id = 'derpbird-gm-async-style';
            style.setAttribute('data-style-promise', 'resolved');
        });
    )JS"sv);
    auto style_metadata = records.values().last().as_object().get_object("metadata"sv).value();
    JsonArray style_grants;
    MUST(style_grants.append("GM.addStyle"sv));
    style_metadata.set("grants"sv, move(style_grants));
    records.values().last().as_object().set("metadata"sv, move(style_metadata));
    for (auto index : { 0u, 4u }) {
        auto& record = records.values()[index].as_object();
        auto metadata = record.get_object("metadata"sv).value();
        metadata.set("requires"sv, MUST(JsonValue::from_string(R"JSON(["https://example.com/first.js","https://example.com/second.js"])JSON"sv)));
        record.set("metadata"sv, move(metadata));
        record.set("requiredSources"sv, MUST(JsonValue::from_string(R"JSON([
            {"url":"https://example.com/first.js","source":"const requiredSequence = [1]; // comment"},
            {"url":"https://example.com/second.js","source":"requiredSequence.push(2);"}
        ])JSON"sv)));
        record.set("source"sv, MUST(String::formatted("if (requiredSequence.join(',') !== '1,2') throw new Error('dependency order');\n{}", record.get_string("source"sv).value())));
    }
    add("budget-expensive"sv, "document-end"sv, "document.documentElement.setAttribute('data-budget-expensive', 'ran');"sv);
    auto expensive_metadata = records.values().last().as_object().get_object("metadata"sv).value();
    expensive_metadata.set("excludes"sv, MUST(JsonValue::from_string(R"JSON(["/(a+)+b$/"])JSON"sv)));
    records.values().last().as_object().set("metadata"sv, move(expensive_metadata));
    add("budget-after"sv, "document-end"sv, "document.documentElement.setAttribute('data-budget-after', 'ran');"sv);
    auto first = WebView::HeadlessWebView::create(theme, { 800, 600 });
    load(*first, "/match"sv);
    check(*first, false);
    TRY_OR_FAIL(app->set_derpbird_user_scripts(JsonValue { move(records) }));
    load(*first, "/match?reload"sv);
    check(*first, true);
    load(*first, "/match?aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv);
    check(*first, true); // An indeterminate later exclusion must not discard earlier scripts.
    Optional<String> budget_result;
    first->on_console_message = [&](WebView::ConsoleOutput message) {
        if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string()) {
            auto text = log->arguments[0].as_string();
            if (text.bytes_as_string_view().starts_with("BUDGET:"sv)) budget_result = move(text);
        }
    };
    expired = false;
    deadline->start();
    first->run_javascript("console.log('BUDGET:'+document.documentElement.hasAttribute('data-budget-expensive')+'|'+document.documentElement.getAttribute('data-budget-after'))"_string);
    Core::EventLoop::current().spin_until([&] { return budget_result.has_value() || crashed || expired; });
    deadline->stop();
    VERIFY(!crashed && !expired);
    EXPECT_EQ(*budget_result, "BUDGET:false|ran"sv);
    first->on_console_message = nullptr;

    auto* test_client = first->page().client().test_connection();
    VERIFY(test_client);
    u64 probe_id = 0;
    using ValueProtocol = Web::DerpBirdValueProtocol;
    auto probe_payload = MUST(ValueProtocol::encode(ValueProtocol::Request {
        ValueProtocol::Method::Open, true, "start"_string, {}, {} }));
    auto probe_storage_channel = [&](bool unknown_document, bool expected_success, bool malformed = false) {
        Optional<bool> result;
        expired = false;
        auto expected_id = ++probe_id;
        test_client->on_derpbird_value_probe = [&](u64 received, bool success, ByteBuffer payload) {
            VERIFY(received == expected_id);
            result = success;
            if (success) EXPECT_EQ(StringView(payload.bytes()), StringView(probe_payload.bytes()));
        };
        deadline->start();
        ReadonlyBytes payload = probe_payload.bytes();
        if (malformed) payload = "opaque-value-fixture"sv.bytes();
        test_client->async_probe_derpbird_values(first->page().id(), expected_id, unknown_document, payload);
        Core::EventLoop::current().spin_until([&] { return result.has_value() || crashed || expired; });
        deadline->stop();
        VERIFY(!crashed && !expired && result.has_value());
        EXPECT_EQ(*result, expected_success);
        test_client->on_derpbird_value_probe = nullptr;
    };
    probe_storage_channel(false, false); // No registered host provider.
    size_t handled = 0;
    app->on_derpbird_value_request = [&](auto& page, auto& document, ByteBuffer payload, auto completion) {
        EXPECT_EQ(page.id(), first->page().id());
        EXPECT(document.creation_url().serialize().contains("/match"sv));
        ++handled;
        Core::deferred_invoke([payload = move(payload), completion = move(completion)]() mutable { completion(move(payload)); });
    };
    probe_storage_channel(false, true);
    EXPECT_EQ(handled, 1u);
    probe_storage_channel(false, false, true); // Invalid protocol never reaches the host provider.
    EXPECT_EQ(handled, 1u);
    probe_storage_channel(true, false); // Rejected before reaching provider.
    EXPECT_EQ(handled, 1u);
    auto probe_event_channel = [&](bool unknown_document) {
        using Protocol = Web::DerpBirdValueProtocol;
        auto doc = first->page().traversable().active_document().id();
        if (unknown_document) ++doc.namespace_id;
        auto payload = MUST(Protocol::encode(Protocol::Event { "unregistered-capability"_string, 1, 1, "u8:key"_string, false,
            {}, MUST(ByteBuffer::copy("value"sv.bytes())) }));
        bool returned = false, rejected = false;
        expired = false;
        deadline->start();
        MUST(first->page().client().send_derpbird_value_event(first->page().id(), doc, move(payload), [&](auto result) {
            EXPECT(result.is_error());
            // A valid active document reaches WebContent and replies asynchronously.
            // A fabricated document is rejected in the UI before any IPC send.
            EXPECT_EQ(returned, !unknown_document);
            rejected = true;
        }));
        returned = true;
        Core::EventLoop::current().spin_until([&] { return rejected || crashed || expired; });
        deadline->stop();
        VERIFY(rejected && !crashed && !expired);
    };
    probe_event_channel(false);
    probe_event_channel(true);
    app->on_derpbird_value_request = nullptr;
    load(*first, "/other"sv);
    check(*first, false);
    auto later = WebView::HeadlessWebView::create(theme, { 800, 600 });
    load(*later, "/match?new-process"sv);
    EXPECT(&first->page().client() != &later->page().client());
    check(*later, true);
    EXPECT_EQ(host_registry_updates, 1u);
    EXPECT(app->set_derpbird_user_scripts(JsonValue { "invalid"sv }).is_error());
    EXPECT_EQ(host_registry_updates, 1u);
    load(*first, "/match?retained"sv);
    check(*first, true);
    auto probe_callback = [&](WebView::HeadlessWebView& view) {
        Optional<String> result;
        expired = false;
        view.on_console_message = [&](WebView::ConsoleOutput message) {
            if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string()) {
                auto text = log->arguments[0].as_string();
                if (text.bytes_as_string_view().starts_with("REVOKE:"sv)) result = move(text);
            }
        };
        deadline->start();
        view.run_javascript("document.dispatchEvent(new Event('revocation-probe')); console.log('REVOKE:'+document.documentElement.getAttribute('data-revocation'))"_string);
        Core::EventLoop::current().spin_until([&] { return result.has_value() || crashed || expired; });
        deadline->stop();
        VERIFY(!crashed && !expired);
        EXPECT_EQ(*result, "REVOKE:1"sv);
        view.on_console_message = nullptr;
    };
    probe_callback(*first);
    TRY_OR_FAIL(app->set_derpbird_user_scripts(JsonValue { JsonArray {} }));
    EXPECT_EQ(host_registry_updates, 2u);
    app->on_derpbird_scripts_changed = nullptr;
    probe_callback(*first); // Same live document; the listener must no longer run.
    load(*first, "/match?removed"sv);
    check(*first, false);
}

TEST_CASE(web_request_policies_follow_cross_process_frames_and_revoke)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdWebRequest"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    ScriptHTTPServer server([](StringView request) -> ByteString {
        if (request.starts_with("GET /blocked"sv)) return "UNBLOCKED";
        if (request.starts_with("GET /redirect"sv)) return "ORIGINAL";
        if (request.starts_with("GET /replacement"sv)) return "REPLACED";
        if (request.starts_with("GET /frame"sv)) return R"HTML(<!doctype html><script>
            async function probe() {
                let blocked=false;
                try { await fetch('/blocked'); } catch { blocked=true; }
                const text=await (await fetch('/redirect')).text();
                parent.postMessage({blocked,text},'*');
            }
            addEventListener('message',event=>{if(event.data==='probe')probe()});
            probe();
        </script>)HTML";
        return R"HTML(<!doctype html><body><script>
            window.results=[];
            addEventListener('message',event=>{if(event.data && typeof event.data.blocked==='boolean')results.push(event.data)});
        </script></body>)HTML";
    });
    auto endpoint_bytes = server.endpoint();
    auto endpoint = MUST(String::from_utf8(endpoint_bytes.view()));
    auto remote = MUST(endpoint.replace("127.0.0.1"sv, "localhost"sv, ReplaceMode::All));
    auto metadata = MUST(JsonValue::from_string(R"JSON({"runAt":"document-start","grantNone":false,"noFrames":true,"matches":[],"includes":[],"excludeMatches":[],"excludes":[],"grants":["GM_webRequest"],"requires":[],"connects":[],"resources":[]})JSON"sv)).as_object();
    JsonArray matches;
    matches.must_append(MUST(String::formatted("{}/*", endpoint)));
    matches.must_append(MUST(String::formatted("{}/*", remote)));
    metadata.set("matches"sv, move(matches));
    auto source = MUST(String::formatted(R"JS(
        window.webRequestEvents=[];
        GM_webRequest([
            {{selector:'{}/blocked*',action:'cancel'}},
            {{selector:'{}/redirect*',action:{{redirect:'{}/replacement'}}}}
        ],(info,message,details)=>webRequestEvents.push({{info,message,url:details.url}}));
    )JS", remote, remote, remote));
    JsonObject record;
    record.set("id"sv, "cross-process-policy"sv);
    record.set("name"sv, "cross-process-policy"sv);
    record.set("source"sv, move(source));
    record.set("enabled"sv, true);
    record.set("fileAccessGranted"sv, false);
    record.set("metadata"sv, move(metadata));
    JsonArray records;
    records.must_append(move(record));
    TRY_OR_FAIL(app->set_derpbird_user_scripts(JsonValue { move(records) }));
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    bool loaded = false, crashed = false, expired = false;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [&] { expired = true; });
    view->on_web_content_crashed = [&](auto) { crashed = true; };
    view->on_load_finish = [&](auto const&) { loaded = true; };
    deadline->start();
    view->load(*URL::Parser::basic_parse(MUST(String::formatted("{}/top", endpoint))));
    Core::EventLoop::current().spin_until([&] {
        return (loaded && view->traversable().active_document().derpbird_web_requests.contains("cross-process-policy"sv)) || crashed || expired;
    });
    deadline->stop();
    VERIFY(loaded && !crashed && !expired);
    Optional<String> result;
    view->on_console_message = [&](WebView::ConsoleOutput message) {
        if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string()) {
            auto text = log->arguments[0].as_string();
            if (text.bytes_as_string_view().starts_with("REQUEST:"sv)) result = move(text);
        }
    };
    view->start_sending_console_output(true, true);
    auto await_probe = [&](String script) {
        result.clear();
        expired = false;
        deadline->start();
        view->run_javascript(move(script));
        Core::EventLoop::current().spin_until([&] { return result.has_value() || crashed || expired; });
        deadline->stop();
        VERIFY(result.has_value() && !crashed && !expired);
    };
    await_probe(MUST(String::formatted(R"JS(
        const frame=document.createElement('iframe'); frame.src='{}/frame'; document.body.appendChild(frame);
        const probeDeadline=performance.now()+5000;
        (function poll(){{if(!results.length || webRequestEvents.length<2){{if(performance.now()>probeDeadline){{console.log('REQUEST:timeout:'+JSON.stringify({{results,events:webRequestEvents}}));return}}setTimeout(poll,5);return}}
            console.log('REQUEST:'+results[0].blocked+':'+results[0].text+':'+(typeof GM_webRequest));}})();
    )JS", remote)));
    EXPECT_EQ(*result, "REQUEST:true:REPLACED:undefined"sv);
    bool separate_process = false;
    view->traversable().for_each_in_subtree([&](WebView::CanonicalNavigable& navigable) {
        auto const& host = navigable.active_document().host();
        if (host && &host->client() != &view->page().client()) separate_process = true;
        return IterationDecision::Continue;
    });
    EXPECT(separate_process);
    TRY_OR_FAIL(app->set_derpbird_user_scripts(JsonValue { JsonArray {} }));
    // The roundtrip into the frame runs after its ordered policy-revocation IPC.
    await_probe(R"JS(document.querySelector('iframe').contentWindow.postMessage('probe','*');
        (function poll(){if(results.length<2){setTimeout(poll,5);return}
            console.log('REQUEST:'+results[1].blocked+':'+results[1].text);})();
    )JS"_string);
    EXPECT_EQ(*result, "REQUEST:false:ORIGINAL"sv);
    view->on_load_finish = nullptr;
    view->on_console_message = nullptr;
    view->on_web_content_crashed = nullptr;
}
