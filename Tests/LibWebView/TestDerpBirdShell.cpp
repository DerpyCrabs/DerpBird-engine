#include "HeadlessTest.h"
#include <AK/JsonObject.h>
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
    auto view = WebView::HeadlessWebView::create(move(theme), { 1200, 800 });
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
    auto handle_request = [&](JsonValue const& request) -> ErrorOr<JsonValue> {
        auto const& data = request.as_object();
        if (data.get_string("type"sv) == "snapshot"sv) {
            ++snapshots;
            warnln("Shell snapshot {}", snapshots);
            return JsonValue::from_string(R"json({"version":1,"revision":1,"tabs":[],"activeTabId":null,"preferences":{"layout":"vertical","compact":false}})json"sv);
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
    view->run_javascript("document.dispatchEvent(new KeyboardEvent('keydown', {key:'t',ctrlKey:true,bubbles:true}))"_string);
    Core::EventLoop::current().spin_until([&] { return creations > 0 || crashed; });
    VERIFY(!crashed);
    EXPECT_EQ(creations, 1u);

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
