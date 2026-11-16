#pragma once
#include <AK/JsonObject.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebDriverBiDi.h>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace Ladybird {

// Lives alongside canonical traversables (in Hub in the Qt browser).
class DerpBirdAutomationBackend final : public QObject {
public:
    using Reply = Function<void(JsonValue)>;
    explicit DerpBirdAutomationBackend(QObject* parent, Function<void(JsonValue)> event)
        : QObject(parent)
        , m_bidi(WebView::WebDriverBiDi::Transport {
            .complete_command = [this](u64 id, Web::WebDriver::Response response) {
                JsonObject packet;
                if (response.is_error()) {
                    auto error = response.release_error();
                    packet.set("type"sv, "error"sv); packet.set("error"sv, AK::move(error.error)); packet.set("message"sv, AK::move(error.message));
                } else { packet.set("type"sv, "success"sv); packet.set("result"sv, response.release_value()); }
                complete(id, AK::move(packet));
            },
            .emit_event = [event = AK::move(event)](String name, JsonValue params, Vector<String>, Vector<String>) {
                JsonObject packet; packet.set("type"sv, "event"sv); packet.set("method"sv, AK::move(name)); packet.set("params"sv, AK::move(params));
                event(AK::move(packet));
            },
        })
    {
        m_bidi.set_enabled_events({ "log.entryAdded"_string, "network.beforeRequestSent"_string,
            "network.responseStarted"_string, "network.responseCompleted"_string, "network.fetchError"_string,
            "browsingContext.contextCreated"_string, "browsingContext.contextDestroyed"_string,
            "browsingContext.navigationStarted"_string, "browsingContext.load"_string }, {});
    }
    void request(String method, JsonValue params, Reply reply)
    {
        auto id = ++m_next_id;
        m_replies.set(id, AK::move(reply));
        QTimer::singleShot(55000, this, [this, id] {
            fail(id, "timeout"sv, "Automation command timed out"sv);
        });
        m_bidi.run_command(id, method, AK::move(params));
    }
private:
    void complete(u64 id, JsonValue value) { if (auto callback = m_replies.take(id); callback.has_value()) (*callback)(AK::move(value)); }
    void fail(u64 id, StringView error, StringView message) {
        JsonObject packet; packet.set("type"sv, "error"sv); packet.set("error"sv, error); packet.set("message"sv, message); complete(id, AK::move(packet));
    }
    HashMap<u64, Reply> m_replies;
    u64 m_next_id { 0 };
    WebView::WebDriverBiDi m_bidi;
};
}
