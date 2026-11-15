#include "HeadlessTest.h"
#include <AK/JsonObject.h>
#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI.h>
#include <UI/Qt/DerpBirdDevToolsSession.h>
#include <QCoreApplication>

namespace {
class DevToolsHTTPServer {
public:
    DevToolsHTTPServer()
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
                if (StringView { connection.request.bytes() }.starts_with("GET /image.png"sv)) {
                    auto png = MUST(decode_base64("iVBORw0KGgoAAAANSUhEUgAAABAAAAAMCAIAAADkharWAAAAF0lEQVR4nGMMmHaCgRTARJLqUQ0jSAMA9d0BxosrUzEAAAAASUVORK5CYII="sv));
                    auto header = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", png.size());
                    MUST(connection.socket->set_blocking(true));
                    MUST(connection.socket->write_until_depleted(header.bytes()));
                    MUST(connection.socket->write_until_depleted(png.bytes())); connection.socket->close(); return;
                }
                bool json = StringView { connection.request.bytes() }.starts_with("GET /data"sv);
                auto body = json ? "{\"data\":{\"nested\":{\"needle\":\"native-search-result\"}}}"sv : "<!doctype html><title>Native tools</title><div id=probe style='color:rgb(1,2,3)'>Probe</div><img src='/image.png'><script>console.log('native-console-ready');fetch('/data').then(r=>r.json()).then(data=>window.result=data)</script>"sv;
                auto response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", json ? "application/json"sv : "text/html"sv, body.length(), body);
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
TEST_CASE(native_devtools_capture_and_shipped_ui)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdDevTools"sv, 90 };
    int argc=1; char name[]="devtools-test"; char* argv[]={name,nullptr}; QCoreApplication qt(argc,argv);
    auto app = TRY_OR_FAIL(environment.create_application());
    auto pump = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [] { QCoreApplication::processEvents(); });
    pump->start();
    DevToolsHTTPServer server;
    auto theme_path=LexicalPath::join(WebView::s_ladybird_resource_root,"themes"sv,"Default.ini"sv);
    auto theme=TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto target=WebView::HeadlessWebView::create(theme,{1000,700});
    size_t loads=0; target->on_load_finish=[&](auto const&){++loads;};
    Core::EventLoop::current().spin_until([&]{return loads>0;});
    warnln("Tools phase: target ready");
    auto ui=WebView::HeadlessWebView::create(theme,{1100,720});
    size_t ui_loads=0;ui->on_load_finish=[&](auto const& url){if(url.serialize()=="about:derpbird"sv)++ui_loads;};

    warnln("Tools phase: UI view ready");
    Ladybird::DerpBirdDevToolsSession session(*target,nullptr,[&](JsonValue packet){if(ui->page().web_ui())ui->page().web_ui()->async_send_message("derpbird.devtools"_string,move(packet));});
    auto request=[&](StringView json)->JsonValue{
        Optional<JsonValue> result;
        session.request(MUST(JsonValue::from_string(json)),[&](auto value){VERIFY(!value.is_error());result=value.release_value();});
        Core::EventLoop::current().spin_until([&]{return result.has_value();});return result.release_value();
    };
    ui->on_derpbird_request=[&](JsonValue const& value,auto done){session.request(value,move(done));};
    target->load(URL::Parser::basic_parse(server.endpoint()).release_value());
    Core::EventLoop::current().spin_until([&]{return loads>=2;});
    warnln("Tools phase: HTTP page loaded");
    JsonValue snapshot;
    Core::EventLoop::current().spin_until([&]{
        snapshot=request(R"({"type":"devtools.bootstrap"})"sv);
        auto rows=snapshot.as_object().get_array("requests"sv).value();
        for(auto const& row:rows.values()) if(row.as_object().get_string("url"sv)->bytes_as_string_view().ends_with("/data"sv)&&row.as_object().get_bool("complete"sv).value_or(false))return true;
        return false;
    });
    warnln("Tools phase: network complete");
    String response_id;
    for(auto const& row:snapshot.as_object().get_array("requests"sv)->values())if(row.as_object().get_string("url"sv)->bytes_as_string_view().ends_with("/data"sv))response_id=row.as_object().get_string("id"sv).value();
    auto response_command=MUST(String::formatted(R"({{"type":"devtools.response","id":"{}"}})",response_id));
    auto body=request(response_command.bytes_as_string_view());
    EXPECT(body.as_string().contains("native-search-result"sv));
    EXPECT_EQ(request(R"({"type":"devtools.evaluate","expression":"document.title"})"sv).as_string(),"Native tools"sv);
    Core::EventLoop::current().spin_until([&]{snapshot=request(R"({"type":"devtools.bootstrap"})"sv);return snapshot.as_object().get_array("sources"sv)->size()>0&&snapshot.as_object().get_object("dom"sv)->size() > 0;});
    auto source_id=snapshot.as_object().get_array("sources"sv)->at(0).as_object().get_string("id"sv).value();
    auto source_command=MUST(String::formatted(R"({{"type":"devtools.source","id":"{}"}})",source_id));
    EXPECT(request(source_command.bytes_as_string_view()).as_string().contains("native-console-ready"sv));
    warnln("Tools phase: native adapter passed");
    String result;
    ui->on_console_message=[&](WebView::ConsoleOutput output){
        if(auto* log=output.output.get_pointer<WebView::ConsoleLog>())for(auto const& argument:log->arguments)if(argument.is_string()&&argument.as_string().bytes_as_string_view().starts_with("TOOLS:"sv))result=argument.as_string();
        if(auto* error=output.output.get_pointer<WebView::ConsoleError>())warnln("Tools UI: {}",error->message);
    };
    ui->start_sending_console_output(true,true);
    ui->load(URL::Parser::basic_parse("about:derpbird"sv).release_value());
    Core::EventLoop::current().spin_until([&]{return ui_loads>=1;});
    ui->run_javascript(R"JS((async()=>{
      const wait=async f=>{for(let i=0;i<300;i++){if(f())return;await new Promise(r=>setTimeout(r,20));}throw Error('timeout '+f)};
      const tab=name=>Array.from(document.querySelectorAll('[role=tab]')).find(b=>b.textContent===name).click();
      await wait(()=>document.querySelector('.dt-dom-row'));
      const probe=Array.from(document.querySelectorAll('.dt-dom-row')).find(b=>b.textContent.includes('id="probe"'));if(!probe)throw Error('DOM missing probe');probe.click();
      await wait(()=>document.querySelector('.dt-rule').textContent.includes('rgb(1, 2, 3)'));
      tab('Network');await wait(()=>document.querySelector('[aria-label="GET data"]'));document.querySelector('[aria-label="GET data"]').click();
      await wait(()=>document.querySelector('.dt-preview'));
      const input=document.querySelector('[aria-label="Search response"]');input.value='native-search-result';input.dispatchEvent(new Event('input',{bubbles:true}));
      await wait(()=>document.querySelector('.current-match')?.textContent.includes('native-search-result'));
      await wait(()=>document.querySelector('[aria-label="GET image.png"]'));document.querySelector('[aria-label="GET image.png"]').click();
      await wait(()=>{const i=document.querySelector('.dt-image-preview img');return i?.complete&&i.naturalWidth===16&&i.naturalHeight===12});
      if(document.querySelector('[aria-label="Search response"]'))throw Error('Image has text search controls');
      if(document.querySelector('[aria-label="Refresh DOM"]'))throw Error('Manual DOM refresh remains');
      tab('Console');const expression=document.querySelector('[aria-label="Console expression"]');expression.value='6 * 7';expression.dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('.dt-console-input').dispatchEvent(new Event('submit',{bubbles:true,cancelable:true}));
      await wait(()=>Array.from(document.querySelectorAll('.dt-console-line.result')).some(row=>row.textContent.includes('42')));
      expression.value="document.querySelector('#probe').setAttribute('data-live', 'updated')";expression.dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('.dt-console-input').dispatchEvent(new Event('submit',{bubbles:true,cancelable:true}));
      tab('Network');await new Promise(r=>setTimeout(r,350));if(document.querySelector('[role=tab][aria-selected=true]').textContent!=='Network')throw Error('Background DOM update switched panels');
      tab('Elements');await wait(()=>Array.from(document.querySelectorAll('.dt-dom-row')).some(row=>row.textContent.includes('data-live="updated"')));
      tab('Sources');await wait(()=>document.querySelector('.dt-code').textContent.includes('native-console-ready'));
      console.log('TOOLS:PASS');
    })().catch(e=>console.log('TOOLS:FAIL:'+e.message+':'+document.querySelector('.dt-detail')?.outerHTML.slice(-2500))))JS"_string);
    Core::EventLoop::current().spin_until([&]{return !result.is_empty();});
    EXPECT_EQ(result,"TOOLS:PASS"sv);
    request(R"({"type":"devtools.clear"})"sv);
    EXPECT_EQ(request(R"({"type":"devtools.bootstrap"})"sv).as_object().get_array("requests"sv)->size(),0u);
}
