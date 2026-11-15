#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibWebView/WebUI/DerpBirdFilterIncludes.h>
#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/MemoryStream.h>
#include <AK/JsonValue.h>
#include <stdlib.h>
#include <LibCore/EventLoop.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Timer.h>
#include <LibWebView/WebContentClient.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/UDPServer.h>
#include <LibDNS/Message.h>
#include <LibWebView/Settings.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentPage.h>

namespace {

// Task-local HTTP server records requests that actually reach the network.
class BlockingHTTPServer {
public:
    BlockingHTTPServer()
        : m_server(MUST(Core::TCPServer::try_create()))
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
                StringView request { connection.request.bytes() };
                auto header_end = request.find("\r\n\r\n"sv);
                if (!header_end.has_value())
                    return;
                size_t content_length = 0;
                for (auto line : request.substring_view(0, *header_end).split_view("\r\n"sv)) {
                    if (line.starts_with("Content-Length:"sv, CaseSensitivity::CaseInsensitive))
                        content_length = line.substring_view(15).trim_whitespace().to_number<size_t>().value();
                }
                if (request.length() < *header_end + 4 + content_length)
                    return;
                connection.socket->on_ready_to_read = nullptr;
                requests.append(ByteString(request.split_view("\r\n"sv)[0]));
                request_headers.append(ByteString(request.substring_view(0, *header_end)));
                bodies.append(ByteString(request.substring_view(*header_end + 4, content_length)));
                auto body = "<!doctype html><title>Blocking fixture</title><script>window.derpAtStart=globalThis.derpProbe;globalThis.derpProbe=false;window.derpAfterWrite=globalThis.derpProbe;</script>"sv;
                if (request.starts_with("GET /isolated"sv))
                    body = "<!doctype html><script>window.pageQueryCalls=0;window.pageAttrCalls=0;document.querySelectorAll=()=>{pageQueryCalls++;return[]};Element.prototype.setAttribute=()=>{pageAttrCalls++};</script><div id=isolated-target data-probe=0 class=admarker></div>"sv;
                if (request.starts_with("GET /procedural"sv))
                    body = "<!doctype html><div id=ad class=card style='display:block!important'>Sponsored</div><div id=remove>Remove me</div><div id=attr class=advert data-ad=1>Keep</div>"sv;
                if (request.starts_with("GET /csp-page"sv) || request.starts_with("GET /csp-frame"sv))
                    body = "<!doctype html><script>document.documentElement.dataset.inline='ran';</script><img src='/csp-image'>"sv;
                if (request.starts_with("GET /html-filter"sv))
                    body = "<!doctype html><script>window.advertisementRan=true;</script><script>window.ordinaryRan=true;</script><div class=sponsor>Remove</div><p id=retained>Keep</p>"sv;
                auto site_policy = request.starts_with("GET /html-filter-trusted-types"sv)
                    ? "Content-Security-Policy: require-trusted-types-for 'script'; trusted-types 'none'\r\n"sv
                    : request.starts_with("GET /csp-page/site-policy"sv)
                    ? "Content-Security-Policy: img-src 'none'\r\n"sv : ""sv;
                if (request.starts_with("GET /csp-page/site-policy"sv))
                    body = "<!doctype html><script>document.documentElement.dataset.inline='ran';</script><img src='/csp-site-policy-image'>"sv;
                if (request.starts_with("GET /top-context-frame"sv))
                    body = "<!doctype html><script>const s=document.createElement('script');s.src='/top-context-script';s.onload=()=>parent.postMessage('top-context-allowed','*');s.onerror=()=>parent.postMessage('top-context-blocked','*');document.head.append(s);</script>"sv;
                if (request.starts_with("GET /top-context-script"sv)) body = "var dependencyLoaded = true;"sv;
                auto cache = (request.starts_with("GET /csp-page/cache"sv) || request.starts_with("GET /canonical-cache"sv)) ? "Cache-Control: max-age=600\r\n"sv : ""sv;
                auto response_modifiers = request.starts_with("GET /header-page"sv)
                    ? "Location: /header-target\r\nRefresh: 3600; url=/header-target\r\nReport-To: {\"group\":\"test\"}\r\nSet-Cookie: derpResponse=stored; Path=/\r\nContent-Security-Policy: script-src 'none'\r\n"sv : ""sv;
                auto response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\n{}{}{}Connection: close\r\n\r\n{}", body.length(), site_policy, cache, response_modifiers, body);
                auto send = [&connection, response = move(response)] {
                    MUST(connection.socket->set_blocking(true));
                    MUST(connection.socket->write_until_depleted(response.bytes()));
                    connection.socket->close();
                };
                if (request.starts_with("GET /csp-page/delayed"sv) || request.starts_with("GET /header-page/delayed"sv))
                    delayed_response = move(send);
                else
                    send();
            };
        };
    }
    Function<void()> delayed_response;
    Vector<ByteString> requests;
    Vector<ByteString> request_headers;
    Vector<ByteString> bodies;
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

TEST_CASE(derpbird_blocking_method_prevents_network_requests)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdBlocking"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    BlockingHTTPServer server;
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    bool loaded = false, crashed = false, expired = false;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [&] { expired = true; });
    deadline->start();
    auto url = URL::Parser::basic_parse(server.endpoint()).release_value();
    view->on_web_content_crashed = [&](auto) { crashed = true; };
    view->on_load_finish = [&](auto const& address) { loaded = address == url; };
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return loaded || crashed || expired; });
    VERIFY(!crashed && !expired);
    auto patterns = "/method-post$method=post\n/method-get$method=get\n/method-head$method=head\n@@/method-post/allowed$method=post\n"_string;
    auto buffer = TRY_OR_FAIL(Core::AnonymousBuffer::create_with_size(patterns.bytes().size()));
    memcpy(buffer.data<u8>(), patterns.bytes().data(), patterns.bytes().size());
    view->page().client().async_set_content_blockers(buffer);
    view->page().async_set_content_blocking_policy(true, {});
    auto check = [&](String script, StringView expected) {
        Optional<String> actual;
        view->on_console_message = [&](WebView::ConsoleOutput message) {
            if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string())
                actual = log->arguments[0].as_string();
        };
        view->start_sending_console_output(true, true);
        view->run_javascript(move(script));
        Core::EventLoop::current().spin_until([&] { return actual.has_value() || crashed || expired; });
        VERIFY(!crashed && !expired);
        if (!expected.is_empty())
            EXPECT_EQ(actual->bytes_as_string_view(), expected);
        view->on_console_message = nullptr;
        return actual.release_value();
    };
    check(R"JS(
(async()=>{
 const results=[];
 for(const [path,method] of [
  ['/method-post','POST'],['/method-post','GET'],
  ['/method-get','GET'],['/method-get','POST'],
  ['/method-head','HEAD'],['/method-head','GET'],
  ['/method-post/allowed','POST']]) {
   try { await fetch(path,{method}); results.push('allowed'); }
   catch { results.push('blocked'); }
 }
 console.log(results.join(','));
})();
)JS"_string, "blocked,allowed,blocked,allowed,blocked,allowed,allowed"sv);
    EXPECT(server.requests.contains_slow("GET /method-post HTTP/1.1"sv));
    EXPECT(server.requests.contains_slow("POST /method-get HTTP/1.1"sv));
    EXPECT(server.requests.contains_slow("GET /method-head HTTP/1.1"sv));
    EXPECT(server.requests.contains_slow("POST /method-post/allowed HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("POST /method-post HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("GET /method-get HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("HEAD /method-head HTTP/1.1"sv));
    view->page().async_set_content_blocking_policy(false, {});
    check("fetch('/method-post',{method:'POST'}).then(()=>console.log('allowed'),()=>console.log('blocked'))"_string, "allowed"sv);
    EXPECT(server.requests.contains_slow("POST /method-post HTTP/1.1"sv));
    auto rewrite_patterns = "/rewrite$removeparam=tracking\n/rewrite-blocked\n"_string;
    auto rewrite_buffer = TRY_OR_FAIL(Core::AnonymousBuffer::create_with_size(rewrite_patterns.bytes().size()));
    memcpy(rewrite_buffer.data<u8>(), rewrite_patterns.bytes().data(), rewrite_patterns.bytes().size());
    view->page().client().async_set_content_blockers(rewrite_buffer);
    view->page().async_set_content_blocking_policy(true, {});
    check(R"JS(
(async()=>{
 const get = await fetch('/rewrite?keep=%26&tracking=secret');
 const post = await fetch('/rewrite?tracking=secret',{method:'POST',body:'body-preserved=abc%26xyz'});
 let denied = false;
 try { await fetch('/rewrite?tracking=secret&mode=error',{redirect:'error'}); } catch { denied = true; }
 let blocked = false;
 try { await fetch('/rewrite-blocked?tracking=secret'); } catch { blocked = true; }
 console.log([new URL(get.url).search, get.redirected, new URL(post.url).search, post.redirected, denied, blocked].join('|'));
})();
)JS"_string, "?keep=%26|true||true|true|true"sv);
    EXPECT(server.requests.contains_slow("GET /rewrite?keep=%26 HTTP/1.1"sv));
    auto post_index = server.requests.find_first_index(ByteString("POST /rewrite HTTP/1.1"sv));
    VERIFY(post_index.has_value());
    EXPECT_EQ(server.bodies[*post_index], "body-preserved=abc%26xyz"sv);
    for (auto const& request : server.requests) {
        EXPECT(!request.contains("tracking=secret"sv));
        EXPECT(!request.contains("/rewrite-blocked"sv));
        EXPECT(!request.contains("mode=error"sv));
    }
    view->page().async_set_content_blocking_policy(false, {});
    check("fetch('/rewrite?tracking=disabled').then(r=>console.log(new URL(r.url).search))"_string, "?tracking=disabled"sv);
    EXPECT(server.requests.contains_slow("GET /rewrite?tracking=disabled HTTP/1.1"sv));

    auto exception_patterns = "/exceptions$removeparam\n@@/exceptions$removeparam=keep\n@@/exceptions/allowed$removeparam\n/exceptions/blocked\n@@/exceptions/blocked$removeparam\n"_string;
    auto exception_buffer = TRY_OR_FAIL(Core::AnonymousBuffer::create_with_size(exception_patterns.bytes().size()));
    memcpy(exception_buffer.data<u8>(), exception_patterns.bytes().data(), exception_patterns.bytes().size());
    view->page().client().async_set_content_blockers(exception_buffer);
    view->page().async_set_content_blocking_policy(true, {});
    check(R"JS(
(async()=>{
 const cleaned = await fetch('/exceptions?keep=&tracking=&bare&keep=yes');
 const exempt = await fetch('/exceptions/allowed?tracking=&bare');
 let blocked = false;
 try { await fetch('/exceptions/blocked?tracking=yes'); } catch { blocked = true; }
 console.log([new URL(cleaned.url).search, new URL(exempt.url).search, exempt.redirected, blocked].join('|'));
})();
)JS"_string, "?keep=&keep=yes|?tracking=&bare|false|true"sv);
    EXPECT(server.requests.contains_slow("GET /exceptions?keep=&keep=yes HTTP/1.1"sv));
    EXPECT(server.requests.contains_slow("GET /exceptions/allowed?tracking=&bare HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("GET /exceptions?keep=&tracking=&bare&keep=yes HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("GET /exceptions/blocked?tracking=yes HTTP/1.1"sv));

    auto inverted_patterns = "/inverted$removeparam=~keep\n@@/inverted$removeparam=protected\n"_string;
    auto inverted_buffer = TRY_OR_FAIL(Core::AnonymousBuffer::create_with_size(inverted_patterns.bytes().size()));
    memcpy(inverted_buffer.data<u8>(), inverted_patterns.bytes().data(), inverted_patterns.bytes().size());
    view->page().client().async_set_content_blockers(inverted_buffer);
    check("fetch('/inverted?%6Beep=yes&tracking=secret&protected=&bare').then(r=>console.log(new URL(r.url).search))"_string,
        "?%6Beep=yes&protected="sv);
    EXPECT(server.requests.contains_slow("GET /inverted?%6Beep=yes&protected= HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("GET /inverted?%6Beep=yes&tracking=secret&protected=&bare HTTP/1.1"sv));

    auto regex_patterns = "/regex$removeparam=/^utm_[a-z]+=.*$/\n@@/regex$removeparam=/^utm_keep=/\n/regex$removeparam=/^event=one,two$/i\n"_string;
    auto regex_buffer = TRY_OR_FAIL(Core::AnonymousBuffer::create_with_size(regex_patterns.bytes().size()));
    memcpy(regex_buffer.data<u8>(), regex_patterns.bytes().data(), regex_patterns.bytes().size());
    view->page().client().async_set_content_blockers(regex_buffer);
    check("fetch('/regex?%75tm_source=secret&utm_keep=yes&event=ONE%2Ctwo&keep=ok').then(r=>console.log(new URL(r.url).search))"_string,
        "?utm_keep=yes&keep=ok"sv);
    EXPECT(server.requests.contains_slow("GET /regex?utm_keep=yes&keep=ok HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("GET /regex?%75tm_source=secret&utm_keep=yes&event=ONE%2Ctwo&keep=ok HTTP/1.1"sv));

    auto resource_patterns = "/redirect-json$redirect=noopjson\n/redirect-script$redirect=noop.js\n/redirect-missing$redirect=missing-resource\n/redirect-rule$redirect-rule=noop.json\n@@/redirect-json/allowed\n"_string;
    auto resource_buffer = TRY_OR_FAIL(Core::AnonymousBuffer::create_with_size(resource_patterns.bytes().size()));
    memcpy(resource_buffer.data<u8>(), resource_patterns.bytes().data(), resource_patterns.bytes().size());
    view->page().client().async_set_content_blockers(resource_buffer);
    check(R"JS(
(async()=>{
 const response = await fetch('/redirect-json');
 const text = await response.text();
 const head = await fetch('/redirect-json',{method:'HEAD'});
 const headText = await head.text();
 const original = await fetch('/redirect-rule');
 const excepted = await fetch('/redirect-json/allowed');
 let missing = false;
 try { await fetch('/redirect-missing'); } catch { missing = true; }
 const script = await new Promise(resolve=>{
   const element=document.createElement('script');
   element.src='/redirect-script';
   element.onload=()=>resolve('loaded');element.onerror=()=>resolve('failed');
   document.head.append(element);
 });
 const crossURL = new URL('/redirect-json',location.href);
 crossURL.hostname='localhost';
 const cross = await fetch(crossURL);
 console.log([response.status,response.headers.get('Content-Type'),text,headText.length,
   (await original.text()).includes('Blocking fixture'),
   (await excepted.text()).includes('Blocking fixture'),missing,script,await cross.text()].join('|'));
})();
)JS"_string, "200|application/json|{}|0|true|true|true|loaded|{}"sv);
    EXPECT(server.requests.contains_slow("GET /redirect-rule HTTP/1.1"sv));
    EXPECT(server.requests.contains_slow("GET /redirect-json/allowed HTTP/1.1"sv));
    for (auto const& request : server.requests) {
        EXPECT(request != "GET /redirect-json HTTP/1.1"sv);
        EXPECT(request != "HEAD /redirect-json HTTP/1.1"sv);
        EXPECT(request != "GET /redirect-script HTTP/1.1"sv);
        EXPECT(request != "GET /redirect-missing HTTP/1.1"sv);
    }
    view->page().async_set_content_blocking_policy(false, {});
    check("fetch('/redirect-json').then(r=>r.text()).then(t=>console.log(t.includes('Blocking fixture')?'original':'wrong'))"_string,
        "original"sv);
    EXPECT(server.requests.contains_slow("GET /redirect-json HTTP/1.1"sv));

    auto set_scriptlets = [&](StringView patterns) {
        auto scriptlet_buffer = MUST(Core::AnonymousBuffer::create_with_size(patterns.length()));
        memcpy(scriptlet_buffer.data<u8>(), patterns.characters_without_null_termination(), patterns.length());
        view->page().client().async_set_content_blockers(scriptlet_buffer);
    };
    auto navigate = [&](StringView path) {
        auto target = URL::Parser::basic_parse(MUST(String::formatted("{}{}", server.endpoint(), path))).release_value();
        bool finished = false;
        view->on_load_finish = [&](auto const& address) { finished = address == target; };
        view->load(target);
        Core::EventLoop::current().spin_until([&] { return finished || crashed || expired; });
        VERIFY(!crashed && !expired);
        view->on_load_finish = nullptr;
    };
    set_scriptlets("/redirect-fallback$redirect=noop.json:1\n/redirect-fallback$redirect=unknown-resource:100\n/redirect-specific$redirect=noop.json:100\n@@/redirect-specific$redirect-rule=noop.json\n/redirect-generic$redirect=noop.json\n@@/redirect-generic$redirect-rule\n/redirect-none$redirect=noop.json:1\n/redirect-none$redirect-rule=none:100\n/redirect-priority$redirect=noop.txt:-10\n/redirect-priority$redirect=noop.json:10\n/redirect-ordinary$redirect=noop.json\n@@/redirect-ordinary\n"sv);
    view->page().async_set_content_blocking_policy(true, {});
    check(R"JS(
        (async()=>{
            const results=[];
            for(const path of ['/redirect-fallback','/redirect-specific','/redirect-generic','/redirect-none','/redirect-priority','/redirect-ordinary']){
                try{const text=await(await fetch(path)).text();results.push(text.trim()==='{}'?'json':text.includes('Blocking fixture')?'original':'other');}
                catch{results.push('blocked');}
            }
            console.log(results.join('|'));
        })();
    )JS"_string, "json|blocked|blocked|blocked|json|original"sv);
    for(auto const& request:server.requests){
        EXPECT(request!="GET /redirect-fallback HTTP/1.1"sv);
        EXPECT(request!="GET /redirect-specific HTTP/1.1"sv);
        EXPECT(request!="GET /redirect-generic HTTP/1.1"sv);
        EXPECT(request!="GET /redirect-none HTTP/1.1"sv);
        EXPECT(request!="GET /redirect-priority HTTP/1.1"sv);
    }
    EXPECT(server.requests.contains_slow("GET /redirect-ordinary HTTP/1.1"sv));

    set_scriptlets("/clickframe$frame,redirect=click2load.html\n"sv);
    view->page().async_set_content_blocking_policy(true, {});
    check(R"JS(
        (()=>{
            document.body.innerHTML='<iframe id="clickframe" style="width:400px;height:220px;border:0" src="/clickframe"></iframe>';
            let attempts=0;
            const poll=()=>{const doc=document.getElementById('clickframe').contentDocument;if(doc?.getElementById('derpbird-click-to-load'))console.log('click-placeholder');else if(++attempts<200)setTimeout(poll,20);else console.log('click-placeholder-failed');};poll();
        })();
    )JS"_string, "click-placeholder"sv);
    EXPECT(!server.requests.contains_slow("GET /clickframe HTTP/1.1"sv));
    check(R"JS(
        document.getElementById('clickframe').contentDocument.getElementById('derpbird-click-to-load').click();
        setTimeout(()=>console.log(document.getElementById('clickframe').contentDocument.getElementById('derpbird-click-to-load')?'synthetic-denied':'synthetic-loaded'),50);
    )JS"_string, "synthetic-denied"sv);
    EXPECT(!server.requests.contains_slow("GET /clickframe HTTP/1.1"sv));
    auto click_position_json = check(R"JS(
        (()=>{const f=document.getElementById('clickframe');const outer=f.getBoundingClientRect();const b=f.contentDocument.getElementById('derpbird-click-to-load').getBoundingClientRect();console.log(JSON.stringify([Math.round(outer.x+b.x+b.width/2),Math.round(outer.y+b.y+b.height/2)]));})();
    )JS"_string, {});
    auto click_position = MUST(JsonValue::from_string(click_position_json)).as_array();
    Web::DevicePixelPoint click_point { click_position.at(0).as_integer<i32>(), click_position.at(1).as_integer<i32>() };
    for(auto type : { Web::MouseEvent::Type::MouseDown, Web::MouseEvent::Type::MouseUp }) {
        view->enqueue_input_event(Web::MouseEvent {
            .type=type, .position=click_point, .screen_position=click_point,
            .button=Web::UIEvents::MouseButton::Primary,
            .buttons=type==Web::MouseEvent::Type::MouseDown?Web::UIEvents::MouseButton::Primary:Web::UIEvents::MouseButton::None,
            .click_count=1, .browser_data=nullptr,
        });
    }
    check(R"JS(
        (()=>{let attempts=0;const poll=()=>{if(document.getElementById('clickframe').contentDocument?.title==='Blocking fixture')console.log('click-loaded');else if(++attempts<200)setTimeout(poll,20);else console.log('click-load-failed');};poll();})();
    )JS"_string, "click-loaded"sv);
    EXPECT(server.requests.contains_slow("GET /clickframe HTTP/1.1"sv));
    check(R"JS(
        (()=>{document.getElementById('clickframe').contentWindow.location.reload();let attempts=0;const poll=()=>{if(document.getElementById('clickframe').contentDocument?.getElementById('derpbird-click-to-load'))console.log('click-permit-consumed');else if(++attempts<200)setTimeout(poll,20);else console.log('click-permit-reused');};poll();})();
    )JS"_string, "click-permit-consumed"sv);
    size_t click_requests=0;for(auto const& request:server.requests)if(request=="GET /clickframe HTTP/1.1"sv)++click_requests;
    EXPECT_EQ(click_requests,1u);

    set_scriptlets("127.0.0.1##+js(set, derpProbe, true)\n127.0.0.1##+js(trusted-set-constant, derpTrustedValue, true)\n127.0.0.1##+js(no-fetch-if, /scriptlet-tracker)\n127.0.0.1##+js(json-prune, ad)\n"sv);
    view->page().async_set_content_blocking_policy(true, {});
    navigate("/scriptlet-first"sv);
    check("console.log([derpAtStart,derpAfterWrite,typeof derpTrustedValue,typeof scriptletGlobals].join('|'))"_string,
        "true|true|undefined|undefined"sv);
    check("fetch('/scriptlet-tracker').then(r=>r.text()).then(t=>console.log(t.length===0?'intercepted':'network'))"_string,
        "intercepted"sv);
    EXPECT(!server.requests.contains_slow("GET /scriptlet-tracker HTTP/1.1"sv));
    check(R"JS(console.log(JSON.stringify(JSON.parse('{"ad":true,"keep":1}'))))JS"_string,
        "{\"keep\":1}"sv);
    set_scriptlets("127.0.0.1##+js(set, derpProbe, true)\n127.0.0.1#@#+js(set, derpProbe, true)\n"sv);
    navigate("/scriptlet-excepted"sv);
    check("console.log([typeof derpAtStart,derpAfterWrite].join('|'))"_string, "undefined|false"sv);
    set_scriptlets("127.0.0.1##+js(set, derpProbe, true)\n"sv);
    view->page().async_set_content_blocking_policy(false, {});
    navigate("/scriptlet-disabled"sv);
    check("console.log([typeof derpAtStart,derpAfterWrite].join('|'))"_string, "undefined|false"sv);
    view->page().async_set_content_blocking_policy(true, Vector<String> { "127.0.0.1"_string });
    navigate("/scriptlet-site-disabled"sv);
    check("console.log([typeof derpAtStart,derpAfterWrite].join('|'))"_string, "undefined|false"sv);

    set_scriptlets("127.0.0.1##+js(set-attr, #isolated-target, data-probe, 1)\n127.0.0.1##+js(remove-class, admarker, #isolated-target, stay)\n"sv);
    view->page().async_set_content_blocking_policy(true, {});
    navigate("/isolated-first"sv);
    check(R"JS(
        let isolatedAttempts=0;
        const pollIsolated=()=>{
            const node=document.getElementById('isolated-target');
            const result=[node.getAttribute('data-probe'),node.className,pageQueryCalls,pageAttrCalls].join('|');
            if(result==='1||0|0' || ++isolatedAttempts>=100)console.log(result);
            else setTimeout(pollIsolated,20);
        };
        pollIsolated();
    )JS"_string, "1||0|0"sv);
    check(R"JS(
        document.getElementById('isolated-target').remove();
        document.body.insertAdjacentHTML('beforeend','<div id="isolated-target" data-probe="0" class="admarker"></div>');
        let attempts=0;
        const poll=()=>{const n=document.getElementById('isolated-target');if(n.getAttribute('data-probe')==='1' && n.className==='')console.log('dynamic-isolated');else if(++attempts<100)setTimeout(poll,20);else console.log('dynamic-failed');};
        poll();
    )JS"_string, "dynamic-isolated"sv);
    set_scriptlets("127.0.0.1##+js(set-attr, #isolated-target, data-probe, 1)\n127.0.0.1#@#+js(set-attr, #isolated-target, data-probe, 1)\n"sv);
    navigate("/isolated-excepted"sv);
    check("console.log(document.getElementById('isolated-target').getAttribute('data-probe'))"_string, "0"sv);
    set_scriptlets("127.0.0.1##+js(set-attr, #isolated-target, data-probe, 1)\n"sv);
    view->page().async_set_content_blocking_policy(false, {});
    navigate("/isolated-disabled"sv);
    check("console.log(document.getElementById('isolated-target').getAttribute('data-probe'))"_string, "0"sv);
    view->page().async_set_content_blocking_policy(true, Vector<String> { "127.0.0.1"_string });
    navigate("/isolated-site-disabled"sv);
    check("console.log(document.getElementById('isolated-target').getAttribute('data-probe'))"_string, "0"sv);

    set_scriptlets("127.0.0.1##.card:has-text(Sponsored)\n127.0.0.1###remove:remove()\n127.0.0.1###attr:remove-attr(data-ad)\n127.0.0.1###attr:remove-class(advert)\n"sv);
    view->page().async_set_content_blocking_policy(true, {});
    navigate("/procedural-first"sv);
    check(R"JS(
        let attempts=0;
        const poll=()=>{const ad=document.getElementById('ad'), attr=document.getElementById('attr');
            if(getComputedStyle(ad).display==='none' && !document.getElementById('remove') && !attr.hasAttribute('data-ad') && attr.className==='')console.log('procedural-applied');
            else if(++attempts<100)setTimeout(poll,20);else console.log('procedural-failed');};
        poll();
    )JS"_string, "procedural-applied"sv);
    check(R"JS(
        document.getElementById('ad').textContent='Editorial';
        document.body.insertAdjacentHTML('beforeend','<div id="dynamic" class="card">Sponsored</div>');
        let changedAttempts=0;
        const pollChanged=()=>{
            if(getComputedStyle(document.getElementById('ad')).display==='block' && getComputedStyle(document.getElementById('dynamic')).display==='none')console.log('procedural-updated');
            else if(++changedAttempts<100)setTimeout(pollChanged,20);else console.log('procedural-update-failed');};
        pollChanged();
    )JS"_string, "procedural-updated"sv);
    set_scriptlets("127.0.0.1##.card:has-text(Sponsored)\n127.0.0.1#@#.card:has-text(Sponsored)\n"sv);
    navigate("/procedural-excepted"sv);
    check("setTimeout(()=>console.log(getComputedStyle(document.getElementById('ad')).display),40)"_string, "block"sv);
    set_scriptlets("127.0.0.1##.card:has-text(Sponsored)\n127.0.0.1###remove:remove()\n"sv);
    view->page().async_set_content_blocking_policy(false, {});
    navigate("/procedural-disabled"sv);
    check("setTimeout(()=>console.log([getComputedStyle(document.getElementById('ad')).display,!!document.getElementById('remove'),typeof __derpbirdAddCSS].join('|')),40)"_string, "block|true|undefined"sv);

    set_scriptlets("127.0.0.1###ad:matches-media((min-width: 0px)):watch-attr(data-status):matches-attr(data-status=ad)\n127.0.0.1###attr:matches-media(not all)\n"sv);
    view->page().async_set_content_blocking_policy(true, {});
    navigate("/procedural-media-watch"sv);
    check(R"JS(
        (()=>{
        document.getElementById('ad').setAttribute('data-status','ad');
        let attempts=0;
        const poll=()=>{
            if(getComputedStyle(document.getElementById('ad')).display==='none' && getComputedStyle(document.getElementById('attr')).display!=='none')console.log('media-watch-applied');
            else if(++attempts<100)setTimeout(poll,20);else console.log('media-watch-failed');
        };poll();
        })();
    )JS"_string, "media-watch-applied"sv);
    check(R"JS(
        (()=>{
        document.getElementById('ad').setAttribute('data-status','editorial');
        let attempts=0;
        const poll=()=>{if(getComputedStyle(document.getElementById('ad')).display==='block')console.log('watch-restored');else if(++attempts<100)setTimeout(poll,20);else console.log('watch-failed');};poll();
        })();
    )JS"_string, "watch-restored"sv);
    set_scriptlets("127.0.0.1###ad:others()\n"sv);
    navigate("/procedural-others"sv);
    check(R"JS(
        (()=>{
        let attempts=0;
        const poll=()=>{
            if(getComputedStyle(document.getElementById('ad')).display==='block' && getComputedStyle(document.getElementById('remove')).display==='none' && getComputedStyle(document.getElementById('attr')).display==='none')console.log('others-applied');
            else if(++attempts<100)setTimeout(poll,20);else console.log('others-failed');
        };poll();
        })();
    )JS"_string, "others-applied"sv);

    set_scriptlets("127.0.0.1##section:has(div:if(> span:has-text(Sponsored)))\n127.0.0.1##article:if-not(span:has-text(Editorial))\n"sv);
    navigate("/procedural-nested"sv);
    check(R"JS(
        (()=>{
            document.body.innerHTML='<section id="nested-ad"><div><span>Sponsored</span></div></section><section id="nested-keep"><div><span>Article</span></div></section><article id="negative-ad"><span>Sponsored</span></article><article id="negative-keep"><span>Editorial</span></article>';
            let attempts=0;
            const hidden=id=>getComputedStyle(document.getElementById(id)).display==='none';
            const poll=()=>{if(hidden('nested-ad')&&!hidden('nested-keep')&&hidden('negative-ad')&&!hidden('negative-keep'))console.log('nested-applied');else if(++attempts<100)setTimeout(poll,20);else console.log('nested-failed');};poll();
        })();
    )JS"_string, "nested-applied"sv);
    check(R"JS(
        (()=>{
            document.querySelector('#nested-ad span').textContent='Article';
            document.querySelector('#negative-ad span').textContent='Editorial';
            let attempts=0;
            const poll=()=>{if(getComputedStyle(document.getElementById('nested-ad')).display!=='none'&&getComputedStyle(document.getElementById('negative-ad')).display!=='none')console.log('nested-restored');else if(++attempts<100)setTimeout(poll,20);else console.log('nested-restore-failed');};poll();
        })();
    )JS"_string, "nested-restored"sv);

    set_scriptlets("127.0.0.1##article:not(:has-text(Editorial))\n127.0.0.1##section:has(> span:has-text(Sponsored), > b:has-text(Promoted))\n127.0.0.1###union-one:has-text(Ad), #union-two:has-text(Ad)\n"sv);
    navigate("/procedural-union-negation"sv);
    check(R"JS(
        (()=>{
            document.body.innerHTML='<article id="not-ad">Sponsored</article><article id="not-keep">Editorial</article><section id="union-has"><b>Promoted</b></section><section id="union-keep"><span>News</span></section><div id="union-one">Ad</div><div id="union-two">Ad</div>';
            let attempts=0;
            const hidden=id=>getComputedStyle(document.getElementById(id)).display==='none';
            const poll=()=>{if(hidden('not-ad')&&!hidden('not-keep')&&hidden('union-has')&&!hidden('union-keep')&&hidden('union-one')&&hidden('union-two'))console.log('union-not-applied');else if(++attempts<100)setTimeout(poll,20);else console.log('union-not-failed');};poll();
        })();
    )JS"_string, "union-not-applied"sv);
    check(R"JS(
        (()=>{
            document.getElementById('not-ad').textContent='Editorial';
            let attempts=0;
            const poll=()=>{if(getComputedStyle(document.getElementById('not-ad')).display!=='none')console.log('not-restored');else if(++attempts<100)setTimeout(poll,20);else console.log('not-restore-failed');};poll();
        })();
    )JS"_string, "not-restored"sv);

    set_scriptlets("127.0.0.1##article:is(:has-text(Sponsored), :has-text(Promoted))\n127.0.0.1##aside:where(:has-text(Sponsored)).eligible\n127.0.0.1##.subject:has-text(Sponsored).eligible > b\n"sv);
    navigate("/procedural-is-where-subject"sv);
    check(R"JS(
        (()=>{
            document.body.innerHTML='<article id="is-ad">Promoted</article><article id="is-keep">Editorial</article><aside id="where-ad" class="eligible">Sponsored</aside><aside id="where-keep">Sponsored<b class="eligible"></b></aside><div class="subject eligible">Sponsored<b id="suffix-ad"></b></div><div class="subject">Sponsored<b id="suffix-keep" class="eligible"></b></div>';
            let attempts=0;
            const hidden=id=>getComputedStyle(document.getElementById(id)).display==='none';
            const poll=()=>{if(hidden('is-ad')&&!hidden('is-keep')&&hidden('where-ad')&&!hidden('where-keep')&&hidden('suffix-ad')&&!hidden('suffix-keep'))console.log('is-where-subject-applied');else if(++attempts<100)setTimeout(poll,20);else console.log('is-where-subject-failed');};poll();
        })();
    )JS"_string, "is-where-subject-applied"sv);

    set_scriptlets("/csp-page$csp=script-src 'none'\n/csp-frame$csp=script-src 'none'\n@@/csp-page/exact$csp=script-src 'none'\n@@/csp-page/all$csp\n@@/csp-page/site-policy-excepted$csp\n"sv);
    view->page().async_set_content_blocking_policy(true, {});
    view->set_request_logging(true);
    view->clear_blocking_log();
    navigate("/csp-page/blocked"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "blocked"sv);
    bool csp_rule_logged = false;
    for (auto const& row : view->blocking_log())
        csp_rule_logged |= row.rule.contains("/csp-page$csp=script-src 'none'"sv);
    EXPECT(csp_rule_logged);
    view->set_request_logging(false);
    navigate("/csp-page/exact"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "ran"sv);
    navigate("/csp-page/all"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "ran"sv);
    server.requests.clear();
    navigate("/csp-page/site-policy"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "blocked"sv);
    EXPECT(!server.requests.contains_slow("GET /csp-site-policy-image HTTP/1.1"sv));
    navigate("/csp-page/site-policy-excepted"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "ran"sv);
    EXPECT(!server.requests.contains_slow("GET /csp-site-policy-image HTTP/1.1"sv));
    navigate("/csp-parent"sv);
    check(R"JS(new Promise(resolve=>{const f=document.createElement('iframe');f.onload=()=>resolve(f.contentDocument.documentElement.dataset.inline||'blocked');f.src='/csp-frame';document.body.append(f)}).then(console.log))JS"_string, "blocked"sv);
    view->page().async_set_content_blocking_policy(false, {});
    navigate("/csp-page/disabled"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "ran"sv);
    view->page().async_set_content_blocking_policy(true, Vector<String> { "127.0.0.1"_string });
    navigate("/csp-page/site-disabled"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "ran"sv);
    view->page().async_set_content_blocking_policy(true, {});
    navigate("/csp-page/cache"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "blocked"sv);
    EXPECT(server.requests.contains_slow("GET /csp-page/cache HTTP/1.1"sv));
    server.requests.clear();
    view->page().async_set_content_blocking_policy(false, {});
    navigate("/csp-page/cache"sv);
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "ran"sv);
    EXPECT(!server.requests.contains_slow("GET /csp-page/cache HTTP/1.1"sv));

    view->page().async_set_content_blocking_policy(true, {});
    auto delayed_url = URL::Parser::basic_parse(MUST(String::formatted("{}/csp-page/delayed", server.endpoint()))).release_value();
    bool delayed_loaded = false;
    view->on_load_finish = [&](auto const& address) { delayed_loaded = address == delayed_url; };
    view->load(delayed_url);
    Core::EventLoop::current().spin_until([&] { return server.delayed_response || crashed || expired; });
    VERIFY(!crashed && !expired);
    view->page().async_set_content_blocking_policy(false, {});
    // The console reply establishes IPC ordering before releasing HTTP headers.
    check("console.log('policy-switch-delivered')"_string, "policy-switch-delivered"sv);
    auto release_response = move(server.delayed_response);
    release_response();
    Core::EventLoop::current().spin_until([&] { return delayed_loaded || crashed || expired; });
    VERIFY(!crashed && !expired);
    view->on_load_finish = nullptr;
    check("console.log(document.documentElement.dataset.inline||'blocked')"_string, "ran"sv);

    set_scriptlets("/permissions-page$permissions=camera=()\n/parameter-probe$xhr,removeparam=drop"sv);
    view->page().async_set_content_blocking_policy(true, {});
    view->set_request_logging(true);
    view->clear_blocking_log();
    navigate("/permissions-page"sv);
    check("fetch('/parameter-probe?drop=1&keep=2').then(()=>console.log('rewritten'))"_string, "rewritten"sv);
    bool permission_rule_logged = false;
    bool parameter_rule_logged = false;
    for (auto const& row : view->blocking_log()) {
        permission_rule_logged |= row.rule.contains("/permissions-page$permissions=camera=()"sv);
        parameter_rule_logged |= row.rule.contains("/parameter-probe$xhr,removeparam=drop"sv);
    }
    EXPECT(permission_rule_logged);
    EXPECT(parameter_rule_logged);
    EXPECT(server.requests.contains_slow("GET /parameter-probe?keep=2 HTTP/1.1"sv));
    view->set_request_logging(false);

    // Header removal must run before redirect/refresh and cookie storage, without
    // stripping security headers or suppressing existing outbound cookies.
    set_scriptlets("127.0.0.1##^responseheader(location)\n127.0.0.1##^responseheader(refresh)\n127.0.0.1##^responseheader(report-to)\n127.0.0.1##^responseheader(set-cookie)\n127.0.0.1##^responseheader(content-security-policy)\n"sv);
    view->page().async_set_content_blocking_policy(true, {});
    check("document.cookie='derpExisting=sent; Path=/';document.cookie='derpResponse=; Max-Age=0; Path=/';console.log('ready')"_string,"ready"sv);
    Vector<HTTP::Header> observed;
    view->on_network_response_headers_received = [&](auto, auto, auto const&, auto const& headers, auto) { observed = headers; };
    auto has_header = [&](StringView name) { for (auto const& header : observed) if (header.name.equals_ignoring_ascii_case(name)) return true; return false; };
    view->set_request_logging(true);
    view->clear_blocking_log();
    navigate("/header-page/filtered"sv);
    for (auto name : { "location"sv, "refresh"sv, "report-to"sv, "set-cookie"sv }) {
        auto expected = MUST(String::formatted("127.0.0.1##^responseheader({})", name));
        bool logged = false;
        for (auto const& row : view->blocking_log()) logged |= row.rule.contains(expected);
        EXPECT(logged);
    }
    view->set_request_logging(false);
    for (auto name : { "location"sv, "refresh"sv, "report-to"sv, "set-cookie"sv }) EXPECT(!has_header(name));
    EXPECT(has_header("content-security-policy"sv));
    EXPECT(server.request_headers.last().contains("derpExisting=sent"sv));
    check("console.log(document.cookie.includes('derpResponse=')?'stored':'absent')"_string, "absent"sv);
    view->page().async_set_content_blocking_policy(false, {});
    navigate("/header-page/disabled"sv);
    for (auto name : { "location"sv, "refresh"sv, "report-to"sv, "set-cookie"sv }) EXPECT(has_header(name));
    check("console.log(document.cookie.includes('derpResponse=stored')?'stored':'absent')"_string, "stored"sv);
    view->page().async_set_content_blocking_policy(true, {});
    check("document.cookie='derpResponse=; Max-Age=0; Path=/';console.log('ready')"_string,"ready"sv);
    set_scriptlets("127.0.0.1##^responseheader(set-cookie)\n127.0.0.1#@#^responseheader(set-cookie)\n"sv);
    navigate("/header-page/exception"sv);
    EXPECT(has_header("set-cookie"sv));
    check("console.log(document.cookie.includes('derpResponse=stored')?'stored':'absent')"_string,"stored"sv);
    set_scriptlets("127.0.0.1##^responseheader(set-cookie)\n"sv);
    check("document.cookie='derpResponse=; Max-Age=0; Path=/';fetch('/header-page/fetch').then(()=>console.log(document.cookie.includes('derpResponse=stored')?'stored':'absent'))"_string,"stored"sv);
    check("document.cookie='derpResponse=; Max-Age=0; Path=/';const f=document.createElement('iframe');f.onload=()=>console.log(document.cookie.includes('derpResponse=')?'stored':'absent');f.src='/header-page/frame';document.body.appendChild(f)"_string,"absent"sv);
    // Changes while HTTP headers are held must affect both storage and delivery.
    for (int scenario = 0; scenario < 4; ++scenario) {
        bool expect_stored = scenario == 0 || scenario == 2;
        set_scriptlets(scenario == 3 ? "! empty\n"sv : "127.0.0.1##^responseheader(set-cookie)\n"sv);
        view->page().async_set_content_blocking_policy(scenario != 1, {});
        check("document.cookie='derpResponse=; Max-Age=0; Path=/';console.log('cleared')"_string,"cleared"sv);
        auto target = URL::Parser::basic_parse(MUST(String::formatted("{}/header-page/delayed-{}", server.endpoint(), scenario))).release_value();
        bool complete = false;
        view->on_load_finish = [&](auto const& address) { complete = address == target; };
        view->load(target);
        Core::EventLoop::current().spin_until([&] { return server.delayed_response || crashed || expired; });
        VERIFY(!crashed && !expired);
        if (scenario < 2) view->page().async_set_content_blocking_policy(scenario == 1, {});
        else set_scriptlets(scenario == 2 ? "! empty\n"sv : "127.0.0.1##^responseheader(set-cookie)\n"sv);
        check("console.log('response-policy-updated')"_string,"response-policy-updated"sv);
        auto send = move(server.delayed_response);
        send();
        Core::EventLoop::current().spin_until([&] { return complete || crashed || expired; });
        VERIFY(!crashed && !expired);
        view->on_load_finish = nullptr;
        EXPECT_EQ(has_header("set-cookie"sv), expect_stored);
        check("console.log(document.cookie.includes('derpResponse=stored')?'stored':'absent')"_string, expect_stored ? "stored"sv : "absent"sv);
    }
    view->on_network_response_headers_received = nullptr;

    // Conditional response blocking rejects fetch before body delivery or cookie
    // storage. Header rules must not block the request before it reaches the server.
    set_scriptlets("/header-page/conditional$xhr,header=set-cookie\n"sv);
    check("document.cookie='derpResponse=; Max-Age=0; Path=/';fetch('/header-page/conditional').then(()=>console.log('allowed'),()=>console.log('blocked'))"_string,"blocked"sv);
    EXPECT(server.requests.contains_slow("GET /header-page/conditional HTTP/1.1"sv));
    check("console.log(document.cookie.includes('derpResponse=')?'stored':'absent')"_string,"absent"sv);
    set_scriptlets("/header-page/conditional$xhr,header=set-cookie\n@@/header-page/conditional$xhr,header\n"sv);
    check("fetch('/header-page/conditional-exception').then(()=>console.log(document.cookie.includes('derpResponse=stored')?'stored':'absent'))"_string,"stored"sv);
    // Populate a cache entry, then add a rule: the cached body must also be denied.
    set_scriptlets("! empty\n"sv);
    check("fetch('/csp-page/cache-header-condition').then(r=>r.text()).then(()=>console.log('cached'))"_string,"cached"sv);
    server.requests.clear();
    set_scriptlets("/csp-page/cache-header-condition$xhr,header=content-type\n"sv);
    check("fetch('/csp-page/cache-header-condition').then(()=>console.log('allowed'),()=>console.log('blocked'))"_string,"blocked"sv);
    EXPECT(!server.requests.contains_slow("GET /csp-page/cache-header-condition HTTP/1.1"sv));

    EXPECT(!view->blocking_log().is_empty());
    EXPECT(view->blocking_log().last().kind.contains("Response header"sv));
    EXPECT_EQ(view->blocking_log().last().rule, "/csp-page/cache-header-condition$xhr,header=content-type"sv);
    set_scriptlets("/journal-blocked$xhr\n"sv);
    check("Promise.all(Array.from({length:140},(_,i)=>fetch('/journal-blocked?i='+i).catch(()=>{}))).then(()=>console.log('journal-full'))"_string,"journal-full"sv);
    EXPECT_EQ(view->blocking_log().size(), 128u);
    EXPECT(view->blocking_log().last().url.contains("journal-blocked"sv));
    EXPECT_EQ(view->blocking_log().last().rule, "/journal-blocked$xhr"sv);
    EXPECT(view->blocking_log().first().sequence < view->blocking_log().last().sequence);
    view->clear_blocking_log();
    EXPECT(view->blocking_log().is_empty());
    view->set_request_logging(true);
    check("fetch('/journal-allowed').then(()=>console.log('journal-request'))"_string,"journal-request"sv);
    EXPECT_EQ(view->blocking_log().size(), 1u);
    EXPECT(view->blocking_log().last().kind.starts_with_bytes("Request ·"sv));
    view->set_request_logging(false);
    check("fetch('/journal-hidden').then(()=>console.log('journal-hidden'))"_string,"journal-hidden"sv);
    EXPECT_EQ(view->blocking_log().size(), 1u);

    set_scriptlets("127.0.0.1##^script:has-text(advertisementRan)\n127.0.0.1##^div.sponsor\n"sv);
    navigate("/html-filter"sv);
    check("console.log([!!window.advertisementRan,!!window.ordinaryRan,!!document.querySelector('.sponsor'),!!document.querySelector('#retained'),document.compatMode].join(','))"_string, "false,true,false,true,CSS1Compat"sv);
    navigate("/html-filter-trusted-types"sv);
    check("console.log([!!document.querySelector('#retained'),!!document.querySelector('.sponsor'),!!window.advertisementRan,window.ordinaryRan].join(','))"_string, "true,false,false,true"sv);
    check("try{new DOMParser().parseFromString('<p>page injection</p>','text/html');console.log('allowed')}catch{console.log('blocked')}"_string, "blocked"sv);
    set_scriptlets("127.0.0.1##^script:has-text(advertisementRan)\n127.0.0.1#@#^script:has-text(advertisementRan)\n"sv);
    navigate("/html-filter-exception"sv);
    check("console.log(String(!!window.advertisementRan))"_string, "true"sv);

    if (getenv("DERPBIRD_BLOCKER_PERF")) {
        set_scriptlets("127.0.0.1##.perf-card:has-text(Sponsored)\n"sv);
        for (size_t sample = 0; sample < 3; ++sample) {
            for (bool enabled : { false, true }) {
                view->page().async_set_content_blocking_policy(enabled, {});
                navigate("/procedural-performance"sv);
                auto script = MUST(String::formatted("globalThis.perfEnabled={};\n{}", enabled ? "true"sv : "false"sv, R"JS(
(async()=>{
 const count=2000, gaps=[];
 let previous=performance.now();
 const heartbeat=setInterval(()=>{const now=performance.now();gaps.push(now-previous);previous=now},5);
 const started=performance.now();
 const fragment=document.createDocumentFragment();
 for(let i=0;i<count;i++){const node=document.createElement('div');node.className='perf-card';node.textContent='Sponsored '+i;fragment.appendChild(node)}
 document.body.appendChild(fragment);
 const inserted=performance.now();
 const nodes=Array.from(document.querySelectorAll('.perf-card'));
 const expected=globalThis.perfEnabled;
 let ready=false;
 for(let attempt=0;attempt<200;attempt++){
   await new Promise(resolve=>setTimeout(resolve,5));
   if((getComputedStyle(nodes[nodes.length-1]).display==='none')===expected){ready=true;break}
 }
 const settled=performance.now();
 await new Promise(resolve=>setTimeout(resolve,100));
 clearInterval(heartbeat);
 const matched=nodes.filter(node=>(getComputedStyle(node).display==='none')===expected).length;
 gaps.sort((a,b)=>a-b);
 console.log(JSON.stringify({count,enabled:expected,ready,matched,insertMs:inserted-started,
   settleMs:settled-started,heartbeatMaxMs:Math.max(...gaps),heartbeatP95Ms:gaps[Math.floor(gaps.length*.95)]}));
})()
)JS"sv));
                auto result = check(move(script), {});
                outln("DERPBIRD_PROCEDURAL_PERF sample={} {}", sample, result);
                auto metrics = MUST(JsonValue::from_string(result)).as_object();
                EXPECT(metrics.get_bool("ready"sv).value_or(false));
                EXPECT_EQ(metrics.get_u64("matched"sv).value_or(0), 2000u);
            }
        }
    }

    // A third-party iframe must inherit the top site's dynamic firewall, while
    // its own hostname remains the static domain= matching context.
    navigate("/top-context-parent"sv);
    view->page().async_set_content_blocking_policy(true, {});
    // Configure the profile service so newly created cross-origin renderer
    // processes receive the same rules and enabled policy as the parent.
    auto& global_settings = WebView::Application::settings();
    auto replace_filters = [&](String filters) {
        global_settings.set_custom_content_blocker_filters(move(filters));
        // Publication queues the renderer IPC before this barrier completes.
        // Do not race the background list reader with the request under test.
        Core::EventLoop::current().spin_until([&] {
            return !app->content_blocker_reload_in_progress() || crashed || expired;
        });
        VERIFY(!crashed && !expired);
    };
    global_settings.set_content_blocker_enabled(true);
    replace_filters("!#derpbird-dynamic 127.0.0.1 * 3p-script block\n"_string);
    auto frame_probe = R"JS(
        new Promise(resolve=>{
          const frame=document.createElement('iframe');
          const listener=event=>{if(event.source!==frame.contentWindow)return;window.removeEventListener('message',listener);frame.remove();resolve(event.data)};
          window.addEventListener('message',listener);
          const url=new URL('/top-context-frame',location.href);url.hostname='localhost';frame.src=url.href;document.body.append(frame);
        }).then(value=>console.log(value));
    )JS"_string;
    check(frame_probe, "top-context-blocked"sv);
    EXPECT(!server.requests.contains_slow("GET /top-context-script HTTP/1.1"sv));
    replace_filters("! No dynamic rules"_string);
    check(frame_probe, "top-context-allowed"sv);
    EXPECT(server.requests.contains_slow("GET /top-context-script HTTP/1.1"sv));
    check("fetch('/canonical-cache-top').then(r=>r.text()).then(()=>console.log('cached'))"_string, "cached"sv);
    replace_filters("/canonical-cache-top$xhr"_string);
    check("fetch('/canonical-cache-top').then(()=>console.log('escaped'),()=>console.log('blocked-cache'))"_string, "blocked-cache"sv);
    replace_filters("/transport-cookie-probe$xhr,requestheader=cookie:/(^|; )transport_probe=1(;|$)/"_string);
    check("document.cookie='transport_probe=1;path=/';console.log(String(document.cookie.includes('transport_probe=1')))"_string, "true"sv);
    check("fetch('/transport-cookie-probe').then(()=>console.log('escaped'),()=>console.log('blocked-cookie'))"_string, "blocked-cookie"sv);
    EXPECT(!server.requests.contains_slow("GET /transport-cookie-probe HTTP/1.1"sv));
    check("fetch('/transport-cookie-probe', {credentials:'omit'}).then(()=>console.log('no-cookie-allowed'),()=>console.log('failed'))"_string, "no-cookie-allowed"sv);
    EXPECT(server.requests.contains_slow("GET /transport-cookie-probe HTTP/1.1"sv));
    replace_filters("/transport-cookie-exception$xhr,requestheader=x-probe:yes\n@@/transport-cookie-exception$xhr,requestheader=cookie:/(^|; )transport_probe=1(;|$)/"_string);
    check("fetch('/transport-cookie-exception',{headers:{'X-Probe':'yes'}}).then(()=>console.log('cookie-exception'),()=>console.log('premature-block'))"_string, "cookie-exception"sv);
    EXPECT(server.requests.contains_slow("GET /transport-cookie-exception HTTP/1.1"sv));
    replace_filters("! populate cache"_string);
    check("fetch('/canonical-cache-cookie-policy').then(r=>r.text()).then(()=>console.log('cookie-cache-ready'))"_string, "cookie-cache-ready"sv);
    auto cached_request_count = server.requests.size();
    replace_filters("/canonical-cache-cookie-policy$xhr,requestheader=cookie:/(^|; )transport_probe=1(;|$)/"_string);
    check("fetch('/canonical-cache-cookie-policy').then(()=>console.log('escaped-cache'),()=>console.log('cookie-cache-blocked'))"_string, "cookie-cache-blocked"sv);
    EXPECT_EQ(server.requests.size(), cached_request_count);
    check("fetch('/canonical-cache-cookie-policy',{credentials:'omit'}).then(r=>r.text()).then(()=>console.log('cookie-cache-omit'))"_string, "cookie-cache-omit"sv);
    EXPECT_EQ(server.requests.size(), cached_request_count);
    check("document.cookie='transport_probe=;Max-Age=0;path=/';console.log('cleared')"_string, "cleared"sv);


}

TEST_CASE(derpbird_canonical_policy_covers_network_cache_and_websocket)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdCanonical"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    BlockingHTTPServer server;
    auto dns = Core::UDPServer::construct();
    VERIFY(dns->bind(IPv4Address { 127, 0, 0, 1 }, 0));
    size_t dns_queries = 0;
    dns->on_ready_to_receive = [&] {
        using namespace DNS::Messages;
        sockaddr_in from {};
        auto bytes = MUST(dns->receive(4096, from));
        ++dns_queries;
        FixedMemoryStream stream { bytes.bytes() };
        auto query = MUST(Message::from_raw(stream));
        Message response;
        response.header.id = query.header.id;
        response.header.options.set_is_question(false);
        response.header.question_count = query.questions.size();
        response.questions = move(query.questions);
        auto owner = response.questions.first().name;
        auto target = DomainName::from_string("tracker.example"sv);
        response.answers.append({ owner, ResourceType::CNAME, Class::IN, 60, Records::CNAME { target }, {} });
        if (response.questions.first().type == ResourceType::A)
            response.answers.append({ target, ResourceType::A, Class::IN, 60, Records::A { IPv4Address { 127, 0, 0, 1 } }, {} });
        response.header.answer_count = response.answers.size();
        ByteBuffer output;
        MUST(response.to_raw(output));
        MUST(dns->send(output.bytes(), from));
    };
    WebView::Application::settings().set_dns_settings(WebView::DNSOverUDP { "127.0.0.1"sv, *dns->local_port(), false });
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    bool loaded = false, crashed = false, expired = false;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [&] { expired = true; });
    deadline->start();
    auto url = URL::Parser::basic_parse(server.endpoint()).release_value();
    url.set_host("visible.example"_string);
    view->on_web_content_crashed = [&](auto) { crashed = true; };
    view->on_load_finish = [&](auto const& address) { loaded = address == url; };
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return loaded || crashed || expired; });
    VERIFY(!crashed && !expired);
    auto filters = [&](StringView patterns) {
        auto buffer = MUST(Core::AnonymousBuffer::create_with_size(patterns.length()));
        memcpy(buffer.data<u8>(), patterns.characters_without_null_termination(), patterns.length());
        view->page().client().async_set_content_blockers(buffer);
        view->page().async_set_content_blocking_policy(true, {});
    };
    auto check = [&](String script, StringView expected) {
        Optional<String> actual;
        view->on_console_message = [&](WebView::ConsoleOutput message) {
            if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string())
                actual = log->arguments[0].as_string();
        };
        view->start_sending_console_output(true, true);
        view->run_javascript(move(script));
        Core::EventLoop::current().spin_until([&] { return actual.has_value() || crashed || expired; });
        VERIFY(!crashed && !expired);
        EXPECT_EQ(actual->bytes_as_string_view(), expected);
        view->on_console_message = nullptr;
    };
    filters("! empty filters\n"sv);
    check("fetch('/canonical-cache').then(r=>r.text()).then(()=>console.log('loaded'),()=>console.log('loaded-failed'))"_string, "loaded"sv);
    auto requests_after_cache = server.requests.size();
    auto queries_after_cache = dns_queries;
    check("fetch('/canonical-cache').then(r=>r.text()).then(()=>console.log('cached'),()=>console.log('cached-failed'))"_string, "cached"sv);
    EXPECT_EQ(server.requests.size(), requests_after_cache);
    filters("||tracker.example^\n@@/except$cname\n/direct-ws$websocket\n"sv);
    check("fetch('/canonical-cache').then(()=>console.log('escaped'),()=>console.log('blocked-cache'))"_string, "blocked-cache"sv);
    EXPECT_EQ(server.requests.size(), requests_after_cache);
    EXPECT_EQ(dns_queries, queries_after_cache);
    check("fetch('/blocked-canonical-post',{method:'POST',body:'must-not-be-sent',cache:'no-store'}).then(()=>console.log('escaped'),()=>console.log('blocked-post'))"_string, "blocked-post"sv);
    EXPECT_EQ(server.requests.size(), requests_after_cache);
    check("fetch('/except').then(r=>r.text()).then(()=>console.log('except'),()=>console.log('except-failed'))"_string, "except"sv);
    EXPECT(server.requests.contains_slow("GET /except HTTP/1.1"sv));
    for (auto path : { "blocked-ws"sv, "except-ws"sv, "direct-ws"sv }) {
        check(MUST(String::formatted("{{const ws=new WebSocket('ws://'+location.host+'/{}');ws.onerror=()=>console.log('closed')}}", path)), "closed"sv);
    }
    EXPECT(!server.requests.contains_slow("GET /blocked-ws HTTP/1.1"sv));
    EXPECT(server.requests.contains_slow("GET /except-ws HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("GET /direct-ws HTTP/1.1"sv));
    view->page().async_set_content_blocking_policy(false, {});
    auto before_disabled = server.requests.size();
    check("fetch('/canonical-cache').then(r=>r.text()).then(()=>console.log('disabled-cache'),()=>console.log('disabled-cache-failed'))"_string, "disabled-cache"sv);
    EXPECT_EQ(server.requests.size(), before_disabled);
    filters("||tracker.example^$redirect=noop.json\n"sv);
    auto before_substitution = server.requests.size();
    check("fetch('/canonical-substitute').then(r=>r.text()).then(t=>console.log(t),()=>console.log('substitute-failed'))"_string, "{}"sv);
    check("fetch('/canonical-cache').then(r=>r.text()).then(t=>console.log(t),()=>console.log('cached-substitute-failed'))"_string, "{}"sv);
    EXPECT_EQ(server.requests.size(), before_substitution);
    filters("||tracker.example^$removeparam=drop\n"sv);
    check("fetch('/canonical-rewrite?drop=secret&keep=yes',{method:'POST',body:'preserved'}).then(r=>console.log(new URL(r.url).host===location.host?'rewritten':'wrong-host'),()=>console.log('rewrite-failed'))"_string, "rewritten"sv);
    EXPECT(server.requests.contains_slow("POST /canonical-rewrite?keep=yes HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("POST /canonical-rewrite?drop=secret&keep=yes HTTP/1.1"sv));
    EXPECT_EQ(server.bodies.last(), "preserved"sv);
    filters(R"JSON({"derpbirdFilterBundle":1,"lists":[{"name":"My filters","trusted":true,"text":"/jump$urlskip=?to"}]})JSON"sv);
    check("fetch('/jump?to='+encodeURIComponent(location.origin+'/skipped'),{method:'POST',body:'skip-body'}).then(r=>console.log(new URL(r.url).pathname),()=>console.log('skip-failed'))"_string, "/skipped"sv);
    EXPECT(server.requests.contains_slow("POST /skipped HTTP/1.1"sv));
    for (auto const& request : server.requests)
        EXPECT(!request.starts_with("POST /jump"sv));
    EXPECT_EQ(server.bodies.last(), "skip-body"sv);
    deadline->stop();
}

TEST_CASE(derpbird_filter_include_graph_is_scoped_and_atomic)
{
    auto parse = [](StringView value) { return URL::Parser::basic_parse(value).release_value(); };
    auto root = parse("https://lists.example/filters/main.txt"sv);
    auto graph = adopt_ref(*new WebView::DerpBirdFilterIncludes(root));
    MUST(graph->accept(root, root, "before\n!#if false\n!#include forbidden.txt\n!#endif\n!#include child.txt\nafter\n"sv));
    auto next = graph->next();
    VERIFY(next.has_value());
    EXPECT_EQ(next->serialize(), "https://lists.example/filters/child.txt"sv);
    EXPECT(graph->finish().is_error());
    MUST(graph->accept(*next, *next, "child-before\n!#include nested/leaf.txt\nchild-after"sv));
    auto leaf = graph->next();
    VERIFY(leaf.has_value());
    MUST(graph->accept(*leaf, *leaf, "leaf"sv));
    EXPECT(!graph->next().has_value());
    auto output = MUST(graph->finish());
    EXPECT(output.contains("before"sv));
    EXPECT(output.contains("child-before\nleaf"sv));
    EXPECT(output.contains("child-after"sv));
    EXPECT(!output.contains("forbidden"sv));
    for (auto path : { "../outside.txt"sv, "https://evil.example/list"sv, "/outside.txt"sv, "%2e%2e/outside.txt"sv }) {
        auto invalid = adopt_ref(*new WebView::DerpBirdFilterIncludes(root));
        auto directive = MUST(String::formatted("!#include {}", path));
        EXPECT(invalid->accept(root, root, directive.bytes_as_string_view()).is_error());
    }
    EXPECT(!WebView::DerpBirdFilterIncludes::allowed(root, parse("https://evil.example/filters/child.txt"sv)));
    auto cycle = adopt_ref(*new WebView::DerpBirdFilterIncludes(root));
    MUST(cycle->accept(root, root, "!#include main.txt"sv));
    EXPECT(!cycle->next().has_value());
    EXPECT(cycle->finish().is_error());
}

TEST_CASE(derpbird_popup_rules_cover_named_windows_links_and_forms)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdPopupFiltering"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    BlockingHTTPServer server;
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    bool loaded = false, crashed = false, expired = false;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [&] { expired = true; });
    deadline->start();
    auto url = URL::Parser::basic_parse(server.endpoint()).release_value();
    view->on_web_content_crashed = [&](auto) { crashed = true; };
    view->on_load_finish = [&](auto const& address) { loaded = address == url; };
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return loaded || crashed || expired; });
    VERIFY(!crashed && !expired);
    auto patterns = "/popup-denied$popup\n/popup-post$popup,method=post\n"_string;
    auto buffer = TRY_OR_FAIL(Core::AnonymousBuffer::create_with_size(patterns.bytes().size()));
    memcpy(buffer.data<u8>(), patterns.bytes().data(), patterns.bytes().size());
    view->page().client().async_set_content_blockers(buffer);
    view->page().async_set_content_blocking_policy(true, {});
    view->set_request_logging(true);
    size_t created = 0;
    view->on_new_web_view = [&](auto, auto, auto&) { ++created; return String {}; };
    Optional<String> result;
    view->on_console_message = [&](WebView::ConsoleOutput message) {
        if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string())
            result = log->arguments[0].as_string();
    };
    view->start_sending_console_output(true, true);
    view->run_javascript(R"JS(
(async()=>{
 const first=window.open('/popup-denied','new-name');
 const second=window.open('/popup-denied','_BLANK');
 const link=document.createElement('a'); link.href='/popup-denied'; link.target='link-name'; document.body.append(link); link.click();
 const form=document.createElement('form'); form.action='/popup-post'; form.method='post'; form.target='form-name'; document.body.append(form); form.submit();
 const frame=document.createElement('iframe'); frame.name='existing-target';
 await new Promise(resolve=>{frame.onload=resolve;document.body.append(frame)});
 await new Promise(resolve=>{frame.onload=resolve; window.open('/popup-denied?existing','existing-target')});
 console.log(first===null && second===null ? 'popup-checks-done' : 'unexpected-window');
})();
)JS"_string);
    Core::EventLoop::current().spin_until([&] { return result.has_value() || crashed || expired; });
    VERIFY(!crashed && !expired);
    EXPECT_EQ(*result, "popup-checks-done"sv);
    EXPECT_EQ(created, 0u);
    size_t blocked = 0;
    for (auto const& row : view->blocking_log())
        if (row.kind == "Popup"sv) ++blocked;
    EXPECT_EQ(blocked, 4u);
    EXPECT(!server.requests.contains_slow("GET /popup-denied HTTP/1.1"sv));
    EXPECT(!server.requests.contains_slow("POST /popup-post HTTP/1.1"sv));
    EXPECT(server.requests.contains_slow("GET /popup-denied?existing HTTP/1.1"sv));
}

TEST_CASE(response_body_waits_for_header_callback_even_during_nested_event_loop)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdBlocking"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    BlockingHTTPServer server;
    auto client = TRY_OR_FAIL(WebView::Application::request_server_client());
    auto headers = HTTP::HeaderList::create();
    auto request = client->start_request("GET"sv, URL::Parser::basic_parse(server.endpoint()).release_value(), {},
        *headers, {}, HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
    VERIFY(request);
    bool header_finished = false, early_body = false, finished = false, expired = false;
    size_t bytes = 0;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(10), [&] { expired = true; });
    deadline->start();
    request->set_unbuffered_request_callbacks(
        [&](auto, auto, auto const&, auto) {
            bool elapsed = false;
            auto nested = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(50), [&] { elapsed = true; });
            nested->start();
            Core::EventLoop::current().spin_until([&] { return elapsed || expired; });
            header_finished = true;
        },
        [&](Requests::ResponseData data) { early_body |= !header_finished; bytes += data.bytes().size(); },
        [&](Core::ImmutableBytes) { early_body |= !header_finished; },
        [&](u64 size, auto const&, Optional<Requests::NetworkError> error) {
            EXPECT(header_finished);
            EXPECT(!error.has_value());
            EXPECT_EQ(bytes, size);
            finished = true;
        });
    Core::EventLoop::current().spin_until([&] { return finished || expired; });
    EXPECT(!expired);
    EXPECT(!early_body);
    EXPECT(bytes > 0);
    if (expired) request->stop();
}
