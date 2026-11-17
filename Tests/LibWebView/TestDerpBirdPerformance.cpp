#include "HeadlessTest.h"
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/Settings.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

// Performance-only entry point. The runner supplies a local fixture and one
// scenario per process; ordinary correctness runs do not run the matrix.
TEST_CASE(derpbird_page_performance)
{
    auto* configuration = getenv("DERPBIRD_PERF_CONFIG");
    if (!configuration) return;
    auto config = MUST(JsonValue::from_string(StringView { configuration, strlen(configuration) })).as_object();
    auto endpoint = StringView { getenv("DERPBIRD_PERF_ENDPOINT"), strlen(getenv("DERPBIRD_PERF_ENDPOINT")) };
    auto samples = config.get_u32("samples"sv).value();
    auto warmups = config.get_u32("warmups"sv).value();
    auto count = config.get_u32("count"sv).value();
    auto rules = config.get_u32("rules"sv).value_or(0);
    auto mode = config.get_string("mode"sv).value_or("off"_string);
    auto scenario = config.get_string("id"sv).value();
    auto timeout = getenv("DERPBIRD_PERF_TIMEOUT");
    Test::HeadlessTestEnvironment environment { "DerpBirdPerformance"sv, timeout ? static_cast<unsigned>(strtoul(timeout,nullptr,10)) : 180 };
    auto app = TRY_OR_FAIL(environment.create_application());
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    JsonArray styles;
    for (u32 i = 0; i < config.get_u32("styles"sv).value_or(0); ++i) {
        JsonObject section;
        section.set("conditions"sv, JsonArray {});
        section.set("css"sv, i == 0 ? "#probe { color: rgb(12,34,56) !important }"_string : MUST(String::formatted(".unused{} {{ color: red }}", i)));
        MUST(styles.append(move(section)));
    }
    MUST(app->set_derpbird_user_style_sections(JsonValue { move(styles) }));
    JsonArray scripts;
    for (u32 i = 0; i < config.get_u32("scripts"sv).value_or(0); ++i) {
        auto record = MUST(JsonValue::from_string(R"JSON({"id":"perf","name":"perf","enabled":true,"fileAccessGranted":false,"source":"document.addEventListener('DOMContentLoaded',()=>document.documentElement.setAttribute('data-user-scripts',String(Number(document.documentElement.getAttribute('data-user-scripts')||0)+1)))","metadata":{"runAt":"document-start","grantNone":false,"noFrames":false,"matches":["http://127.0.0.1/*"],"includes":[],"excludeMatches":[],"excludes":[],"grants":[],"requires":[],"connects":[],"resources":[],"entries":[{"key":"sandbox","value":"DOM"}]}})JSON"sv)).as_object();
        auto metadata=record.get_object("metadata"sv).value();
        JsonArray matches;MUST(matches.append(MUST(String::formatted("{}/*",endpoint))));
        metadata.set("matches"sv,move(matches));record.set("metadata"sv,move(metadata));
        record.set("id"sv, MUST(String::formatted("perf{}", i)));
        MUST(scripts.append(move(record)));
    }
    MUST(app->set_derpbird_user_scripts(JsonValue { move(scripts) }));
    StringBuilder filters;
    filters.append("! performance fixture\n"sv);
    if (mode == "procedural"sv) {
        filters.append("127.0.0.1##.card:has-text(Sponsored)\n"sv);
        for (u32 i=1; i<rules; ++i) filters.appendff("127.0.0.1##.card:has-text(NeverMatches{})\n", i);
    } else if (mode == "cosmetic"sv) {
        filters.append("127.0.0.1##.card\n"sv);
    } else if (mode == "network"sv) {
        filters.appendff("|{}/blocked/*$xhr\n",endpoint);
        for (u32 i=1; i<rules; ++i) filters.appendff("||tracker{}.invalid^\n", i);
    }
    auto& settings=WebView::Application::settings();
    settings.set_content_blocker_enabled(mode != "off"sv);
    settings.set_custom_content_blocker_filters(MUST(filters.to_string()));
    Core::EventLoop::current().spin_until([&] {return !app->content_blocker_reload_in_progress();});
    using Clock = std::chrono::steady_clock;
    auto ms = [](auto start) { return std::chrono::duration<double, std::milli>(Clock::now()-start).count(); };
    for (u32 iteration=0; iteration<samples+warmups; ++iteration) {
        bool crashed=false;
        auto started=Clock::now();
        auto view=WebView::HeadlessWebView::create(theme, {1280,720});
        bool initial=false;
        view->on_web_content_crashed=[&](auto) { crashed=true; };
        view->on_load_finish=[&](auto const&) { initial=true; };
        view->load(URL::Parser::basic_parse("about:blank"sv).release_value());
        Core::EventLoop::current().spin_until([&] {return initial||crashed;});
        VERIFY(!crashed);
        auto startup_ms=ms(started);
        view->page().async_set_content_blocking_policy(mode != "off"sv, {});
        auto token=MUST(String::formatted("{}-{}",scenario,iteration));
        config.set("token"sv,token);
        bool shell = config.get_string("kind"sv).value_or("page"_string) == "shell"sv;
        if (shell) {
            view->on_derpbird_request = [&](JsonValue const& request, auto completion) {
                auto type=request.as_object().get_string("type"sv).value_or(String {});
                JsonValue response;
                if(type=="snapshot"sv) response=MUST(JsonValue::from_string(R"JSON({"version":1,"revision":1,"tabs":[{"id":"test","url":"about:blank","title":"Test","loading":false,"canGoBack":false,"canGoForward":false,"pinned":false,"private":false,"audible":false,"muted":false}],"activeTabId":"test","preferences":{"layout":"vertical","compact":false}})JSON"sv));
                else if(type=="import.status"sv) response=MUST(JsonValue::from_string(R"JSON({"welcome":false,"profiles":[]})JSON"sv));
                else if(type.is_one_of("styles.folders"sv,"scripts.folders"sv)) response=MUST(JsonValue::from_string(R"JSON({"directories":[],"files":[]})JSON"sv));
                else if(type.is_one_of("styles.list"sv,"scripts.list"sv)) response=MUST(JsonValue::from_string(R"JSON({"revision":0,"items":[]})JSON"sv));
                Core::deferred_invoke([completion=move(completion),response=move(response)]() mutable {completion(move(response));});
            };
        }
        auto address=URL::Parser::basic_parse(MUST(String::formatted("{}/page?count={}&token={}",endpoint,count,token))).release_value();
        if (shell) address=URL::Parser::basic_parse("about:derpbird"sv).release_value();
        bool loaded=false;
        view->on_load_finish=[&](auto const& url) {loaded=url==address;};
        started=Clock::now();
        for(u32 cycle=0;cycle<config.get_u32("cycles"sv).value_or(1);++cycle) {
            loaded=false;view->load(address);
            Core::EventLoop::current().spin_until([&] {return loaded||crashed;});
            VERIFY(!crashed);
        }
        auto navigation_ms=ms(started);
        view->on_load_finish=nullptr;
        Vector<NonnullOwnPtr<WebView::HeadlessWebView>> tabs;
        for(u32 i=1;i<config.get_u32("tabs"sv).value_or(1);++i) {
            auto tab=WebView::HeadlessWebView::create(theme,{1280,720});
            bool ready=false;
            tab->on_web_content_crashed=[&](auto) {crashed=true;};
            tab->on_load_finish=[&](auto const& url) {ready=url==address;};
            tab->load(address);
            Core::EventLoop::current().spin_until([&] {return ready||crashed;});
            VERIFY(!crashed);tab->on_load_finish=nullptr;
            tab->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
            tabs.append(move(tab));
        }
        Optional<String> result;
        view->on_console_message=[&](WebView::ConsoleOutput message) {
            if(auto* log=message.output.get_pointer<WebView::ConsoleLog>();log&&log->arguments.size()==1&&log->arguments[0].is_string()) {
                auto text=log->arguments[0].as_string();
                if(text.bytes_as_string_view().starts_with("PERF_RESULT:"sv)) result=MUST(text.substring_from_byte_offset(12));
            }
        };
        view->start_sending_console_output(true,true);
        if(shell) {
            auto file=MUST(Core::File::open("tests/performance/shell-workloads.js"sv,Core::File::OpenMode::Read));
            auto source=MUST(file->read_until_eof());
            view->run_javascript(MUST(String::from_utf8(StringView { source.bytes() })));
        }
        outln("PERF_PHASE {}", MUST(String::formatted("{{\"sample\":{},\"warmup\":{},\"phase\":\"workload\"}}",iteration,iteration<warmups)));
        fflush(stdout);
        view->run_javascript(MUST(String::formatted("runPerf({}).then(r=>console.log('PERF_RESULT:'+JSON.stringify(r)),e=>console.log('PERF_RESULT:'+JSON.stringify({{ok:false,error:String(e)}})))",JsonValue(config).serialized())));
        Core::EventLoop::current().spin_until([&] {return result.has_value()||crashed;});
        VERIFY(!crashed);
        auto output=MUST(JsonValue::from_string(*result)).as_object();
        output.set("id"sv,scenario);output.set("sample"sv,iteration);output.set("warmup"sv,iteration<warmups);
        if(auto metrics=output.get_object("metrics"sv);metrics.has_value()) {
            if(config.get_bool("capture"sv).value_or(false)) {
                bool captured=false,failed=false;
                auto begin=Clock::now();
                view->take_screenshot(WebView::ViewImplementation::ScreenshotType::Visible)->when_resolved([&](auto const& path) {
                    auto destination=ByteString::formatted("{}/capture-{}.png",getenv("DERPBIRD_PERF_OUTPUT"),iteration);
                    MUST(Core::System::rename(path.string(),destination));captured=true;
                }).when_rejected([&](auto&) {failed=true;});
                Core::EventLoop::current().spin_until([&] {return captured||failed||crashed;});
                VERIFY(captured&&!failed&&!crashed);
                metrics->set("captureMs"sv,ms(begin));
            }
            metrics->set("viewReadyMs"sv,startup_ms);metrics->set("navigationMs"sv,navigation_ms);
            output.set("metrics"sv,*metrics);
        }
        outln("PERF_SAMPLE {}",JsonValue(output).serialized());fflush(stdout);
        EXPECT(output.get_bool("ok"sv).value_or(false));
        view->on_console_message=nullptr;
        for(auto& tab:tabs) tab->on_web_content_crashed=nullptr;
        view->on_web_content_crashed=nullptr;
    }
}
