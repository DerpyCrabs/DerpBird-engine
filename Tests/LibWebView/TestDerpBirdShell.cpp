#include "HeadlessTest.h"
#include <AK/JsonObject.h>
#include <AK/JsonArray.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibHTTP/Proxy.h>
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>

// Exercises the shipped bundle and real document-scoped WebUI transport. The
// tab service is deliberately a fixture; Qt window behavior is a separate test.
TEST_CASE(derpbird_bundle_mounts_and_round_trips_through_webui)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdShell"sv };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), Core::Environment::has("DERPBIRD_PROXY_NARROW"sv) ? Web::DevicePixelSize { 1000, 700 } : Web::DevicePixelSize { 2000, 1100 });
    if (auto scale = Core::Environment::get("DERPBIRD_PROXY_SCALE"sv); scale.has_value())
        view->set_device_pixel_ratio_for_testing(view->page(), scale->to_number<double>().value());
    view->on_console_message = [](WebView::ConsoleOutput message) {
        if (auto* error = message.output.get_pointer<WebView::ConsoleError>())
            warnln("Shell JavaScript error: {}: {}", error->name, error->message);
    };
    view->start_sending_console_output(false, true);
    size_t loads = 0;
    bool crashed = false;
    size_t snapshots = 0;
    size_t viewports = 0;
    size_t creations = 0;
    view->on_load_finish = [&](auto const& url) { ++loads; warnln("Shell load {}: {}", loads, url); };
    view->on_web_content_crashed = [&](auto) { crashed = true; };
    Core::EventLoop::current().spin_until([&] { return loads > 0 || crashed; });
    VERIFY(!crashed);
    auto proxy_state = MUST(JsonValue::from_string(R"({"revision":0,"active":"direct","options":{"schemaVersion":2}})"sv)).as_object();
    size_t proxy_writes = 0;
    auto style_snapshot = MUST(JsonValue::from_string(R"({"revision":0,"items":[{"id":"style1","name":"Readable pages","enabled":true,"source":"body { color: #ddd; }","sections":[{"css":"body { color: #ddd; }","conditions":[]}]}]})"sv)).as_object();
    auto script_snapshot = MUST(JsonValue::from_string(R"({"revision":0,"items":[{"id":"script1","name":"Study timer for this website","enabled":true,"fileAccessGranted":false,"source":"// ==UserScript==\n// @name Study timer for this website\n// @match https://example.test/*\n// ==/UserScript==\nconsole.log('ready');","metadata":{"runAt":"document-idle","grantNone":false,"noFrames":false,"matches":["https://example.test/*"],"includes":[],"excludeMatches":[],"excludes":[],"grants":[],"requires":[],"connects":[],"resources":[]}}]})"sv)).as_object();
    size_t source_toggles = 0;

    auto handle_request = [&](JsonValue const& request) -> ErrorOr<JsonValue> {
        auto const& data = request.as_object();
        if (data.get_string("type"sv) == "snapshot"sv) {
            ++snapshots;
            warnln("Shell snapshot {}", snapshots);
            return JsonValue::from_string(R"json({"version":1,"revision":1,"tabs":[{"id":"test","url":"https://example.test/","title":"Example","loading":false,"canGoBack":false,"canGoForward":false,"pinned":false,"private":false,"audible":false,"muted":false}],"activeTabId":"test","preferences":{"layout":"vertical","compact":false}})json"sv);
        }
        auto type = data.get_string("type"sv).value_or(String {});
        if (type == "import.status"sv) return JsonValue::from_string(R"({"welcome":false,"profiles":[]})"sv);
        if (type.is_one_of("styles.folders"sv, "scripts.folders"sv)) return JsonValue::from_string(R"({"directories":[],"files":[]})"sv);
        if (type == "styles.list"sv) return JsonValue(style_snapshot);
        if (type == "scripts.list"sv) return JsonValue(script_snapshot);
        if (type.is_one_of("styles.replace"sv, "scripts.replace"sv)) {
            auto& state = type == "styles.replace"sv ? style_snapshot : script_snapshot;
            state.set("items"sv, *data.get_array("items"sv));
            state.set("revision"sv, state.get_u32("revision"sv).value() + 1);
            return JsonValue(state);
        }
        if (type.is_one_of("sources.page"sv, "sources.toggle"sv)) {
            if (type == "sources.toggle"sv) {
                ++source_toggles;
                auto& state = data.get_string("kind"sv) == "styles"sv ? style_snapshot : script_snapshot;
                auto items = *state.get_array("items"sv);
                items.values()[0].as_object().set("enabled"sv, *data.get_bool("enabled"sv));
                state.set("items"sv, move(items));
                state.set("revision"sv, state.get_u32("revision"sv).value() + 1);
            }
            JsonObject result;
            result.set("tabId"sv, "test"sv); result.set("url"sv, "https://example.test/"sv);
            result.set("styleRevision"sv, *style_snapshot.get_u32("revision"sv));
            result.set("scriptRevision"sv, *script_snapshot.get_u32("revision"sv));
            result.set("styles"sv, *style_snapshot.get_array("items"sv));
            result.set("scripts"sv, *script_snapshot.get_array("items"sv));
            return JsonValue(move(result));
        }
        if (data.get_string("type"sv) == "proxy.get"sv) return JsonValue(proxy_state);
        if (data.get_string("type"sv) == "proxy.replace"sv) {
            HTTP::ProxyConfiguration configuration;
            TRY(configuration.set_omega(data.get_object("options"sv).value(), data.get_string("active"sv).value()));
            proxy_state = data;
            proxy_state.set("revision"sv, ++proxy_writes);
            return JsonValue(proxy_state);
        }
        auto command = data.get_object("command"sv);
        if (!command.has_value())
            return Error::from_string_literal("Expected command");
        if (command->get_string("type"sv) == "shell.viewport"sv) {
            auto rect = command->get_object("rect"sv);
            VERIFY(rect.has_value());
            VERIFY(rect->get_integer<int>("width"sv).value_or(0) > 0);
            VERIFY(rect->get_integer<int>("height"sv).value_or(0) > 0);
            ++viewports;
            warnln("Shell viewport {}", viewports);
        }
        if (command->get_string("type"sv) == "tab.create"sv)
            ++creations;
        return JsonValue {};
    };
    Vector<WebView::ViewImplementation::DerpBirdCompletion> held;
    view->on_derpbird_request = [&](JsonValue const& request, auto completion) {
        if (request.as_object().get_string("type"sv) == "hold"sv) {
            held.append(move(completion));
            return;
        }
        auto result = handle_request(request);
        // Real deferred completion: the request handler returns before sending
        // a response, as required by the worker-backed credential service.
        Core::deferred_invoke([completion = move(completion), result = move(result)]() mutable {
            completion(move(result));
        });
    };
    auto url = URL::Parser::basic_parse("about:derpbird"sv).release_value();
    warnln("Loading shell document");
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return viewports > 0 || crashed; });
    VERIFY(!crashed);
    EXPECT_EQ(snapshots, 1u);
    view->run_javascript("(document.activeElement ?? document).dispatchEvent(new KeyboardEvent('keydown', {key:'t',ctrlKey:true,bubbles:true}))"_string);
    Core::EventLoop::current().spin_until([&] { return creations > 0 || crashed; });
    VERIFY(!crashed);
    EXPECT_EQ(creations, 1u);

    // Exercise the actual compiled proxy editor in Ladybird, not a DOM emulator.
    String proxy_result;
    view->on_console_message = [&](WebView::ConsoleOutput message) {
        if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string()) {
            auto text = log->arguments[0].as_string();
            if (text.bytes_as_string_view().starts_with("PROXY:SHOT:"sv)) {
                if (auto directory = Core::Environment::get("DERPBIRD_PROXY_SCREENSHOT"sv); directory.has_value()) {
                    auto destination = ByteString::formatted("{}/{}.png", *directory, text.bytes_as_string_view().substring_view(11));
                    view->take_screenshot(WebView::ViewImplementation::ScreenshotType::Full)->when_resolved([&, destination](auto const& path) {
                        MUST(Core::System::rename(path.string(), destination));
                        warnln("Proxy screenshot: {}", destination);
                        view->run_javascript("window.proxyScreenshotDone=true"_string);
                    }).when_rejected([&](auto&) { proxy_result = "PROXY:FAIL:screenshot"_string; });
                } else view->run_javascript("window.proxyScreenshotDone=true"_string);
                return;
            }
            if (text.bytes_as_string_view().starts_with("PROXY:"sv)) proxy_result = move(text);
        }
    };
    view->start_sending_console_output(true, true);
    view->run_javascript(R"JS((async()=>{
        const wait=async f=>{for(let i=0;i<200;i++){if(f())return;await new Promise(r=>setTimeout(r,20));}throw new Error('UI timeout: '+String(f))};
        const shot=async name=>{window.proxyScreenshotDone=false;console.log('PROXY:SHOT:'+name);await wait(()=>window.proxyScreenshotDone)};
        const button=t=>Array.from(document.querySelectorAll('.proxy-manager button')).find(b=>b.textContent===t);
        try {
            document.querySelector('[aria-label="Proxy switcher"]').click();
            await wait(()=>document.querySelectorAll('.proxy-profile-option:not(:disabled)').length===2);
            const anchor=document.querySelector('[aria-label="Proxy switcher"]');
            const bounds=anchor.getBoundingClientRect();
            if(bounds.width<24 || anchor.scrollWidth>anchor.clientWidth || anchor.textContent.trim())throw new Error('Toolbar icon is clipped');
            const popup=document.querySelector('.proxy-popup').getBoundingClientRect();
            if(popup.width>310 || popup.height>400 || popup.top<bounds.bottom || popup.right>innerWidth)throw new Error('Popup geometry');
            await shot('01-empty-menu');
            document.querySelectorAll('.proxy-profile-option')[1].click();
            await wait(()=>!document.querySelector('.proxy-popup'));
            document.querySelector('[aria-label="Proxy switcher"]').click();await wait(()=>document.querySelector('.proxy-profile-option[aria-pressed="true"]:not(:disabled)')?.textContent.includes('System proxy'));
            document.querySelector('[aria-label="Proxy switcher"]').click();await wait(()=>!document.querySelector('.proxy-popup'));
            document.querySelector('[aria-label="Proxy switcher"]').click();await wait(()=>document.querySelector('.proxy-popup-settings:not(:disabled)'));
            document.querySelector('.proxy-popup-settings').click();
            await wait(()=>document.querySelector('.proxy-empty'));
            const empty=document.querySelector('.proxy-manager').getBoundingClientRect();
            if(empty.width>600 || empty.height>500 || getComputedStyle(document.querySelector('.proxy-manager')).outlineStyle!=='none')throw new Error('Empty settings geometry');
            if(document.querySelector('[type=file]').getBoundingClientRect().height)throw new Error('Raw file input is visible');
            await shot('02-empty-settings');
            button('New profile').click();
            await wait(()=>document.querySelector('[aria-label="New proxy profile name"]'));
            const name=document.querySelector('[aria-label="New proxy profile name"]');name.value='Work';name.dispatchEvent(new Event('input',{bubbles:true}));
            await wait(()=>!button('Create profile').disabled);
            button('Create profile').click();
            await wait(()=>document.querySelector('[aria-label="Fallback proxy host"]'));
            if(document.querySelector('[aria-label="Fallback proxy host"]').value!=='127.0.0.1')throw new Error('New proxy defaults');
            button('Discard changes').click();
            await wait(()=>document.querySelector('.proxy-empty'));

            const backup={schemaVersion:2,'-startupProfileName':'direct','-confirmDeletion':false,'+proxy':{name:'proxy',profileType:'FixedProfile',fallbackProxy:{scheme:'http',host:'127.0.0.1',port:8080}},'+auto':{name:'auto',profileType:'SwitchProfile',defaultProfileName:'direct',rules:Array.from({length:846},(_,i)=>({condition:{conditionType:'HostWildcardCondition',pattern:`*.site${i}.test`},profileName:'proxy'}))}};
            const transfer=new DataTransfer();transfer.items.add(new File([JSON.stringify(backup)],'ZeroOmegaOptions.bak',{type:'application/json'}));
            const importer=document.querySelector('[aria-label="Import ZeroOmega backup"]');importer.files=transfer.files;importer.dispatchEvent(new Event('change',{bubbles:true}));
            await wait(()=>document.querySelector('[role="status"]')?.textContent.includes('846 rules'));
            button('Apply settings').click();
            await wait(()=>document.querySelector('[role="status"]')?.textContent.includes('applied'));
            const originalClick=HTMLAnchorElement.prototype.click;
            let exported;
            HTMLAnchorElement.prototype.click=function(){fetch(this.href).then(r=>r.json()).then(value=>exported=value)};
            button('Export backup').click();
            await wait(()=>exported);
            HTMLAnchorElement.prototype.click=originalClick;
            if(JSON.stringify(exported)!==JSON.stringify(backup))throw new Error('Backup round trip changed fields');
            await shot('03-proxy-server');
            const auth=Array.from(document.querySelectorAll('.proxy-detail')).find(e=>e.querySelector('summary')?.textContent==='Authentication');auth.open=true;
            const username=document.querySelector('[aria-label="All connections username"]');username.value='fixture';username.dispatchEvent(new Event('input',{bubbles:true}));
            const password=document.querySelector('[aria-label="All connections password"]');password.value='test-only';password.dispatchEvent(new Event('input',{bubbles:true}));
            button('Apply settings').click();
            await wait(()=>document.querySelector('[role="status"]')?.textContent.includes('applied'));

            document.querySelectorAll('.proxy-sidebar-profile')[1].click();
            await wait(()=>document.querySelectorAll('.proxy-rule').length===40);
            button('Next').click();
            await wait(()=>document.querySelector('[aria-label="Rule 41 pattern"]'));
            const search=document.querySelector('[aria-label="Search proxy rules"]');search.value='site845';search.dispatchEvent(new Event('input',{bubbles:true}));
            await wait(()=>document.querySelectorAll('.proxy-rule').length===1);
            const input=document.querySelector('[aria-label="Rule 846 pattern"]');input.value='*.edited.test';input.dispatchEvent(new Event('input',{bubbles:true}));
            button('Apply settings').click();
            await wait(()=>document.querySelector('[role="status"]')?.textContent.includes('applied'));
            const active=document.querySelector('[aria-label="Active proxy profile"]');active.value='auto';active.dispatchEvent(new Event('change',{bubbles:true}));
            await wait(()=>document.querySelector('[aria-label="Active proxy profile"]').value==='auto' && !document.querySelector('[aria-label="Active proxy profile"]').disabled);
            search.value='';search.dispatchEvent(new Event('input',{bubbles:true}));
            await wait(()=>document.querySelectorAll('.proxy-rule').length===40);
            const dialog=document.querySelector('.proxy-manager').getBoundingClientRect();
            if(dialog.width>1050 || dialog.height>750 || dialog.left<0 || dialog.right>innerWidth)throw new Error('Editor geometry');
            if(document.querySelector('.proxy-rule').scrollWidth>document.querySelector('.proxy-rule').clientWidth)throw new Error('Rule overflow');
            const list=document.querySelector('.proxy-rules').getBoundingClientRect();
            if(list.height<(innerHeight>800?220:80))throw new Error('Rule list leaves unused vertical space');
            const fallback=document.querySelector('[aria-label="Default proxy profile"]').getBoundingClientRect();
            if(fallback.bottom>document.querySelector('.proxy-settings-footer').getBoundingClientRect().top)throw new Error('Default profile is hidden');
            await shot('04-rule-editor');
            document.querySelector('[aria-label="Close proxy settings"]').click();
            document.querySelector('[aria-label="Proxy switcher"]').click();
            await wait(()=>document.querySelectorAll('.proxy-profile-option').length===4);
            await wait(()=>!document.querySelector('.proxy-profile-option[aria-pressed="true"]').disabled);
            (document.activeElement ?? document).dispatchEvent(new KeyboardEvent('keydown',{key:'End',bubbles:true}));
            if(document.activeElement!==document.querySelector('.proxy-popup-settings'))throw new Error('Menu End key');
            (document.activeElement ?? document).dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true}));
            if(document.activeElement!==document.querySelector('.proxy-profile-option[aria-pressed="true"]'))throw new Error('Menu arrow key');
            await shot('05-profile-menu');
            (document.activeElement ?? document).dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));
            await wait(()=>!document.querySelector('.proxy-popup'));
            const sourceButton = text => Array.from(document.querySelectorAll('.source-manager button')).find(b=>b.textContent===text);
            document.querySelector('button[aria-label="Styles on this page"]').click();
            await wait(()=>document.querySelector('.page-source-row input:not(:disabled)'));
            if(document.querySelector('.page-sources-popup header button'))throw new Error('Redundant popup close button');
            for (const dismiss of ['Escape', 'outside', 'toggle']) {
                if(dismiss==='Escape')(document.activeElement ?? document).dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));
                else if(dismiss==='outside')document.querySelector('.address').dispatchEvent(new PointerEvent('pointerdown',{bubbles:true}));
                else document.querySelector('button[aria-label="Styles on this page"]').click();
                await wait(()=>!document.querySelector('.page-sources-popup'));
                document.querySelector('button[aria-label="Styles on this page"]').click();
                await wait(()=>document.querySelector('.page-source-row input:not(:disabled)'));
            }
            await shot('06-page-styles');
            document.querySelector('.page-source-row input').click();
            await wait(()=>document.querySelector('.page-source-row input')?.checked===false && !document.querySelector('.page-source-row input').disabled);
            Array.from(document.querySelectorAll('.page-sources-popup button')).find(b=>b.textContent==='Manage styles').click();
            await wait(()=>document.querySelector('[aria-label="Style name"]:not(:disabled)'));
            const styleName=document.querySelector('[aria-label="Style name"]');styleName.value='Readable website';styleName.dispatchEvent(new Event('input',{bubbles:true}));
            document.querySelector('[aria-label="Close styles"]').click();
            await wait(()=>document.querySelector('.source-close-confirm'));
            sourceButton('Keep editing').click();
            if(document.querySelector('[aria-label="Style name"]').value!=='Readable website')throw new Error('Close discarded a style draft');
            sourceButton('Discard changes').click();
            await wait(()=>sourceButton('Save')?.disabled);
            const manager=document.querySelector('.source-manager').getBoundingClientRect();
            const code=document.querySelector('[aria-label="CSS style"]').getBoundingClientRect();
            if(manager.width>1101 || code.height<150 || code.right>manager.right || code.bottom>manager.bottom)throw new Error('Style editor layout');
            await shot('07-style-editor');
            document.querySelector('[aria-label="Close styles"]').click();
            await wait(()=>!document.querySelector('.source-manager'));
            document.querySelector('button[aria-label="Scripts on this page"]').click();
            await wait(()=>document.querySelector('.page-source-row input:not(:disabled)'));
            await shot('08-page-scripts');
            document.querySelector('.page-source-row input').click();
            await wait(()=>document.querySelector('.page-sources-reload') && !document.querySelector('.page-source-row input').disabled);
            Array.from(document.querySelectorAll('.page-sources-popup button')).find(b=>b.textContent==='Manage scripts').click();
            await wait(()=>document.querySelector('[aria-label="Script source"]:not(:disabled)'));
            await shot('09-script-editor');
            document.querySelector('[aria-label="Close scripts"]').click();
            await wait(()=>!document.querySelector('.source-manager'));
            console.log('PROXY:PASS');
        } catch(error) {console.log('PROXY:FAIL:'+error.message)}
    })())JS"_string);
    Core::EventLoop::current().spin_until([&] { return !proxy_result.is_empty() || crashed; });
    VERIFY(!crashed);
    EXPECT_EQ(proxy_result, "PROXY:PASS"sv);
    if (proxy_result != "PROXY:PASS"sv) return;
    EXPECT_EQ(proxy_writes, 5u);
    EXPECT_EQ(source_toggles, 2u);
    EXPECT_EQ(proxy_state.get_string("active"sv).value(), "auto"sv);
    EXPECT_EQ(proxy_state.get_object("options"sv)->get_object("+proxy"sv)->get_object("auth"sv)->get_object("all"sv)->get_string("username"sv).value(), "fixture"sv);
    auto const& saved_rules = *proxy_state.get_object("options"sv)->get_object("+auto"sv)->get_array("rules"sv);
    EXPECT_EQ(saved_rules.at(845).as_object().get_object("condition"sv)->get_string("pattern"sv).value(), "*.edited.test"sv);

    // A fresh document must reacquire the channel and publish its geometry.
    auto old_viewports = viewports;
    warnln("Loading shell document");
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return (snapshots > 1 && viewports > old_viewports) || crashed; });
    VERIFY(!crashed);
    EXPECT_EQ(snapshots, 2u);

    Vector<String> replies;
    view->on_console_message = [&](WebView::ConsoleOutput message) {
        if (auto* log = message.output.get_pointer<WebView::ConsoleLog>(); log && log->arguments.size() == 1 && log->arguments[0].is_string()) {
            auto text = log->arguments[0].as_string();
            if (text.bytes_as_string_view().starts_with("RPC:"sv)) replies.append(move(text));
        }
    };
    view->start_sending_console_output(true, true);
    auto watch = "document.addEventListener('WebUIMessage',e=>{if(e.detail.name==='derpbird.response'&&e.detail.data.requestId>=10000)console.log('RPC:'+e.detail.data.requestId+':'+e.detail.data.ok)})"_string;
    view->run_javascript(watch);
    // Fill native admission, attempt duplicate IDs, and observe an explicit
    // overflow response rather than allocating a 129th pending operation.
    view->run_javascript("for(let i=0;i<129;i++)ladybird.sendMessage('derpbird.request',{requestId:10000+i,payload:{type:'hold'}});ladybird.sendMessage('derpbird.request',{requestId:10000,payload:{type:'hold'}})"_string);
    Core::EventLoop::current().spin_until([&] { return !replies.is_empty() || crashed; });
    VERIFY(!crashed);
    EXPECT_EQ(held.size(), 128u);
    EXPECT_EQ(replies[0], "RPC:10128:false"sv);
    held[0](JsonValue {});
    held[0](JsonValue {}); // A backend bug must not send a second response.
    Core::EventLoop::current().spin_until([&] { return replies.size() > 1 || crashed; });
    VERIFY(!crashed);
    EXPECT_EQ(replies[1], "RPC:10000:true"sv);
    auto prior_snapshots = snapshots;
    old_viewports = viewports;
    view->load(url);
    Core::EventLoop::current().spin_until([&] { return (snapshots > prior_snapshots && viewports > old_viewports) || crashed; });
    VERIFY(!crashed);
    view->run_javascript(watch);
    for (size_t i = 1; i < held.size(); ++i) held[i](JsonValue {});
    held.clear();
    // A new-document reply is the ordering barrier for old-document replies.
    view->run_javascript("ladybird.sendMessage('derpbird.request',{requestId:20000,payload:{type:'unknown'}})"_string);
    Core::EventLoop::current().spin_until([&] { return replies.size() > 2 || crashed; });
    VERIFY(!crashed);
    EXPECT_EQ(replies.size(), 3u);
    EXPECT_EQ(replies[2], "RPC:20000:false"sv);

    // A normal content view cannot acquire the browser-control channel merely
    // by navigating to the shell URL. Keep the authorized view alive as well:
    // its capability must not authorize another view implicitly.
    warnln("Testing unprivileged content view");
    auto content_theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto content = WebView::HeadlessWebView::create(move(content_theme), { 800, 600 });
    bool content_loaded_shell = false;
    content->on_load_finish = [&](auto const& loaded_url) { content_loaded_shell = loaded_url == url; };
    content->on_web_content_crashed = [&](auto) { crashed = true; };
    content->load(url);
    Core::EventLoop::current().spin_until([&] { return content_loaded_shell || crashed; });
    VERIFY(!crashed);
    EXPECT(!content->page().web_ui());
    EXPECT(view->page().web_ui());
    EXPECT_EQ(snapshots, 3u);
    EXPECT_EQ(creations, 1u);
}
