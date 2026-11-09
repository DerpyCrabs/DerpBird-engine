#include "HeadlessTest.h"
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

// Task-local loopback fixture. Every path serves the same probe; URL matching
// must distinguish /match from /other without relying on their HTML contents.
class StyleHTTPServer {
public:
    StyleHTTPServer()
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
                auto body = "<!doctype html><div id=probe style='color:rgb(1,2,3)'>HTTP child</div><script>addEventListener('message',e=>{if(e.source===parent&&e.data==='read-style')parent.postMessage({color:getComputedStyle(document.getElementById('probe')).color},'*')})</script>"sv;
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

// Exercise the production application registry and IPC, rather than the
// document-local Internals hook used by the LibWeb style fixtures.
TEST_CASE(derpbird_styles_reach_existing_and_future_renderer_processes)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdStyles"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    StyleHTTPServer server;
    auto path = ByteString::formatted("{}/probe.html", environment.directory());
    auto file = TRY_OR_FAIL(Core::File::open(path, Core::File::OpenMode::Write));
    auto html = ByteString::formatted("<!doctype html><div id=probe style='color:rgb(1,2,3)'>Probe</div><iframe id=matching src='{}/match'></iframe><iframe id=other src='{}/other'></iframe>", server.endpoint(), server.endpoint());
    TRY_OR_FAIL(file->write_until_depleted(html.bytes()));
    file->close();
    auto url = URL::create_with_file_scheme(path).release_value();
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    bool crashed = false;
    auto load = [&](WebView::HeadlessWebView& view) {
        bool loaded = false;
        view.on_web_content_crashed = [&](auto) { crashed = true; };
        view.on_load_finish = [&](auto const& loaded_url) { loaded = loaded_url == url; };
        view.load(url);
        Core::EventLoop::current().spin_until([&] { return loaded || crashed; });
        VERIFY(!crashed);
        view.on_load_finish = nullptr;
    };
    auto check_script = [&](WebView::HeadlessWebView& view, String script, StringView expected) {
        Optional<String> color;
        view.on_console_message = [&](WebView::ConsoleOutput message) {
            if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string())
                color = log->arguments[0].as_string();
        };
        view.start_sending_console_output(true, true);
        view.run_javascript(move(script));
        Core::EventLoop::current().spin_until([&] { return color.has_value() || crashed; });
        VERIFY(!crashed);
        EXPECT_EQ(color->bytes_as_string_view(), expected);
        view.on_console_message = nullptr;
    };
    auto check_color = [&](WebView::HeadlessWebView& view, StringView expected) {
        check_script(view, "console.log(getComputedStyle(document.getElementById('probe')).color)"_string, expected);
    };
    auto check_frame = [&](WebView::HeadlessWebView& view, StringView id, StringView expected) {
        auto script = MUST(String::formatted(R"JS((()=>{{const frame=document.getElementById('{}'); const receive=e=>{{if(e.source!==frame.contentWindow||typeof e.data?.color!=='string')return;removeEventListener('message',receive);console.log(e.data.color)}};addEventListener('message',receive);frame.contentWindow.postMessage('read-style','*')}})())JS", id));
        check_script(view, move(script), expected);
    };
    auto first = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto second = WebView::HeadlessWebView::create(theme, { 800, 600 });
    load(*first);
    load(*second);
    EXPECT(&first->page().client() != &second->page().client());
    check_color(*first, "rgb(1, 2, 3)"sv);
    check_color(*second, "rgb(1, 2, 3)"sv);
    JsonObject condition;
    condition.set("type"sv, "url"sv);
    condition.set("value"sv, url.serialize());
    JsonArray conditions;
    TRY_OR_FAIL(conditions.append(move(condition)));
    JsonObject section;
    section.set("conditions"sv, move(conditions));
    section.set("css"sv, "#probe { color: rgb(4,5,6) !important }"sv);
    JsonArray sections;
    TRY_OR_FAIL(sections.append(move(section)));
    JsonObject child_condition;
    child_condition.set("type"sv, "url"sv);
    child_condition.set("value"sv, MUST(String::formatted("{}/match", server.endpoint())));
    JsonArray child_conditions;
    TRY_OR_FAIL(child_conditions.append(move(child_condition)));
    JsonObject child_section;
    child_section.set("conditions"sv, move(child_conditions));
    child_section.set("css"sv, "#probe { color: rgb(7,8,9) !important }"sv);
    TRY_OR_FAIL(sections.append(move(child_section)));
    check_frame(*first, "matching"sv, "rgb(1, 2, 3)"sv);
    TRY_OR_FAIL(app->set_derpbird_user_style_sections(JsonValue { move(sections) }));
    check_color(*first, "rgb(4, 5, 6)"sv);
    check_color(*second, "rgb(4, 5, 6)"sv);
    check_frame(*first, "matching"sv, "rgb(7, 8, 9)"sv);
    check_frame(*first, "other"sv, "rgb(1, 2, 3)"sv);
    check_frame(*second, "matching"sv, "rgb(7, 8, 9)"sv);
    auto later = WebView::HeadlessWebView::create(theme, { 800, 600 });
    load(*later);
    EXPECT(&first->page().client() != &later->page().client());
    check_color(*later, "rgb(4, 5, 6)"sv);
    check_frame(*later, "matching"sv, "rgb(7, 8, 9)"sv);
    check_frame(*later, "other"sv, "rgb(1, 2, 3)"sv);
    TRY_OR_FAIL(app->set_derpbird_user_style_sections(JsonValue { JsonArray {} }));
    check_color(*first, "rgb(1, 2, 3)"sv);
    check_color(*second, "rgb(1, 2, 3)"sv);
    check_color(*later, "rgb(1, 2, 3)"sv);
    check_frame(*first, "matching"sv, "rgb(1, 2, 3)"sv);
    check_frame(*later, "matching"sv, "rgb(1, 2, 3)"sv);
    // Installed themes share the site author cascade. Earlier site layer
    // registration wins when the theme repeats those names. Empty layer
    // registration must survive section splitting and style replacement.
    check_script(*first, R"JS((()=>{
        const style=document.createElement('style');style.id='author-layers';
        style.textContent='@layer second, first; @layer first { #layer-author {color:rgb(255,0,0)} } @layer second { #layer-author {color:rgb(0,0,255)} }';
        document.head.append(style);
        for(const id of ['layer-user','layer-author']){const node=document.createElement('div');node.id=id;document.body.append(node);}
        console.log('ready');
    })())JS"_string, "ready"sv);
    auto set_layer_sections = [&](StringView registration, StringView rules) {
        JsonArray layer_sections;
        for (auto css : { registration, rules }) {
            JsonObject item;
            item.set("conditions"sv, JsonArray {});
            item.set("css"sv, css);
            MUST(layer_sections.append(move(item)));
        }
        MUST(app->set_derpbird_user_style_sections(JsonValue { move(layer_sections) }));
    };
    auto check_layers = [&](StringView expected) {
        check_script(*first, "console.log(getComputedStyle(document.getElementById('layer-user')).color+'|'+getComputedStyle(document.getElementById('layer-author')).color)"_string, expected);
    };
    set_layer_sections("@layer first {}"sv, "@layer second {#layer-user{color:rgb(0,0,255)}} @layer first {#layer-user{color:rgb(0,255,0)}}"sv);
    check_layers("rgb(0, 255, 0)|rgb(255, 0, 0)"sv);
    set_layer_sections("@layer second, first;"sv, "@layer second {#layer-user{color:rgb(0,0,255)}} @layer first {#layer-user{color:rgb(0,255,0)}}"sv);
    check_layers("rgb(0, 255, 0)|rgb(255, 0, 0)"sv);
    set_layer_sections("@layer first {}"sv, "@layer second {#layer-user{color:rgb(0,0,255)!important}} @layer first {#layer-user{color:rgb(0,255,0)!important}}"sv);
    check_layers("rgb(0, 0, 255)|rgb(255, 0, 0)"sv);
    set_layer_sections("@layer outer { @layer first {} }"sv, "@layer outer.second {#layer-user{color:rgb(0,0,255)}} @layer outer.first {#layer-user{color:rgb(0,255,0)}}"sv);
    check_layers("rgb(0, 0, 255)|rgb(255, 0, 0)"sv);
    MUST(app->set_derpbird_user_style_sections(JsonValue { JsonArray {} }));
    check_script(*first, "console.log(getComputedStyle(document.getElementById('layer-author')).color)"_string, "rgb(255, 0, 0)"sv);
    // One anonymous layer and selector span multiple URL-selected fragments.
    // Important declarations distinguish identity preservation from reopening
    // anonymous layers (which would incorrectly keep the first red rule).
    auto structured = MUST(JsonValue::from_string(R"JSON([
      {"sourceId":"one","conditions":[],"wrappers":[{"id":1,"open":"@layer {"},{"id":2,"open":"#layer-user {"}],"css":"color:rgb(255,0,0)!important;"},
      {"sourceId":"one","conditions":[{"type":"domain","value":"excluded.invalid"}],"wrappers":[{"id":1,"open":"@layer {"},{"id":2,"open":"#layer-user {"}],"css":"color:rgb(0,255,0)!important;"},
      {"sourceId":"one","conditions":[],"wrappers":[{"id":1,"open":"@layer {"},{"id":2,"open":"#layer-user {"}],"css":"color:rgb(0,0,255)!important;"},
      {"sourceId":"two","conditions":[],"wrappers":[{"id":1,"open":"@layer {"},{"id":2,"open":"#layer-user {"}],"css":"color:rgb(255,0,255)!important;"}
    ])JSON"sv));
    MUST(app->set_derpbird_user_style_sections(move(structured)));
    check_layers("rgb(0, 0, 255)|rgb(255, 0, 0)"sv);
    MUST(app->set_derpbird_user_style_sections(JsonValue { JsonArray {} }));

}
