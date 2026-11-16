#include "HeadlessTest.h"
#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <UI/Qt/DerpBirdAutomation.h>
#include <UI/Qt/DerpBirdDevToolsSession.h>
#include <QCoreApplication>
#include <QLocalSocket>

TEST_CASE(bidi_attach_evaluate_input_screenshot_and_shared_devtools)
{
    Test::HeadlessTestEnvironment environment { "TestDerpBirdAutomation"sv, 60 };
    int argc = 1; char name[] = "automation-test"; char* argv[] = { name, nullptr };
    QCoreApplication qt(argc, argv);
    auto app = TRY_OR_FAIL(environment.create_application());
    auto pump = Core::Timer::create_repeating(AK::Duration::from_milliseconds(5), [] { QCoreApplication::processEvents(); });
    pump->start();
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    bool loaded = false;
    view->on_load_finish = [&](auto const&) { loaded = true; };
    Core::EventLoop::current().spin_until([&] { return loaded; });
    loaded = false;
    view->load(URL::Parser::basic_parse("data:text/html,<!doctype html><title>Agent fixture</title><input id=probe>"sv).release_value());
    Core::EventLoop::current().spin_until([&] { return loaded; });

    // Attach after a page already exists, while a human DevTools session owns its
    // callbacks. BiDi must not replace those callbacks or close this page on detach.
    Ladybird::DerpBirdDevToolsSession devtools(*view, nullptr, [](auto) {});
    auto path = QString::fromUtf8(environment.directory().characters()) + "/automation.sock";
    qputenv("DERPBIRD_AUTOMATION_SOCKET", path.toUtf8());
    auto& automation = Ladybird::DerpBirdAutomation::the();
    TRY_OR_FAIL(automation.enable());
    QLocalSocket first, second;
    first.connectToServer(path); second.connectToServer(path);
    Core::EventLoop::current().spin_until([&] { return first.state() == QLocalSocket::ConnectedState && second.state() == QLocalSocket::ConnectedState; });
    u64 request_id = 0;
    auto request = [&](QLocalSocket& socket, String method, JsonObject params) {
        auto id = ++request_id;
        JsonObject packet; packet.set("id"sv, id); packet.set("method"sv, AK::move(method)); packet.set("params"sv, AK::move(params));
        auto serialized = packet.serialized(); socket.write(qstring_from_ak_string(serialized).toUtf8()); socket.write("\n");
        Optional<JsonObject> response;
        Core::EventLoop::current().spin_until([&] {
            while (socket.canReadLine()) {
                auto line = socket.readLine();
                auto value = MUST(JsonValue::from_string(StringView(line.constData(), line.size())));
                if (value.as_object().get_integer<u64>("id"sv) == id) response = value.as_object();
            }
            return response.has_value();
        });
        return response.release_value();
    };
    auto tree = request(first, "browsingContext.getTree"_string, {});
    EXPECT_EQ(tree.get_string("type"sv), "success"sv);
    auto contexts = tree.get_object("result"sv)->get_array("contexts"sv).value();
    EXPECT_EQ(contexts.size(), 1u);
    auto context = contexts[0].as_object().get_string("context"sv).value();
    auto evaluate = [&](QLocalSocket& socket, String expression) {
        JsonObject target; target.set("context"sv, context);
        JsonObject params; params.set("target"sv, AK::move(target)); params.set("expression"sv, AK::move(expression)); params.set("awaitPromise"sv, true);
        return request(socket, "script.evaluate"_string, AK::move(params));
    };
    auto result = evaluate(first, "Promise.resolve(42)"_string);
    EXPECT_EQ(result.get_object("result"sv)->get_object("result"sv)->get_integer<int>("value"sv), 42);
    auto exception = evaluate(second, "throw new Error('automation-test')"_string);
    EXPECT_EQ(exception.get_object("result"sv)->get_string("type"sv), "exception"sv);
    auto setup = evaluate(first, "document.body.innerHTML='<input id=probe autofocus>';document.querySelector('input').focus();true"_string);
    EXPECT_EQ(setup.get_string("type"sv), "success"sv);
    JsonObject input_params;
    input_params.set("context"sv, context);
    input_params.set("actions"sv, MUST(JsonValue::from_string(R"JSON([{"type":"key","id":"keyboard","actions":[{"type":"keyDown","value":"x"},{"type":"keyUp","value":"x"}]}])JSON"sv)));
    auto input = request(first, "input.performActions"_string, AK::move(input_params));
    EXPECT_EQ(input.get_string("type"sv), "success"sv);
    auto typed = evaluate(second, "document.querySelector('input').value"_string);
    EXPECT_EQ(typed.get_object("result"sv)->get_object("result"sv)->get_string("value"sv), "x"sv);
    JsonObject screenshot_params; screenshot_params.set("context"sv, context);
    auto screenshot = request(first, "browsingContext.captureScreenshot"_string, AK::move(screenshot_params));
    EXPECT_EQ(screenshot.get_string("type"sv), "success"sv);
    EXPECT(screenshot.get_object("result"sv)->get_string("data"sv)->bytes_as_string_view().starts_with("iVBOR"sv));
    Optional<JsonValue> console;
    devtools.request(MUST(JsonValue::from_string(R"({"type":"devtools.evaluate","expression":"6*7"})"sv)), [&](auto response) { VERIFY(!response.is_error()); console = response.release_value(); });
    Core::EventLoop::current().spin_until([&] { return console.has_value(); });
    EXPECT_EQ(console->as_string(), "42"sv);
    automation.disable();
    EXPECT(!automation.enabled());
    EXPECT(view->has_traversable());
    qunsetenv("DERPBIRD_AUTOMATION_SOCKET");
}
