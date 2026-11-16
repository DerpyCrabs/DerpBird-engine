#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/OwnPtr.h>
#include <AK/WeakPtr.h>
#include <LibWebView/Forward.h>
#include <LibWebView/WebUI.h>
#include <QHash>
#include <QObject>
#include <QPointer>

namespace Ladybird {

class DerpBirdValueHost;
class DerpBirdDevToolsSession;
class DerpBirdAutomationBackend;

}
namespace Hub {

class ConnectionFromUI;
class View;

// DerpBird's trusted browser services live with canonical documents in the Hub.
// Only native UI operations cross back to the UI process.
class DerpBird final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;
    using Completion = Function<void(ErrorOr<JsonValue>)>;
    explicit DerpBird(ConnectionFromUI&);
    ~DerpBird() override;
    static DerpBird* the();
    void command(u64 request_id, u64 view_id, String command, JsonValue payload);
    void shell_response(u64 request_id, JsonValue result, Optional<String> error);
    void ui_response(u64 request_id, JsonValue result, Optional<String> error);
    void install_view_callbacks(View&);
    void release_devtools(u64 view_id);
    JsonValue view_state(View const&) const;
    void shutdown(Function<void()>);

private:
    String shell_channel(View const&) const;
    struct Channel {
        WeakPtr<WebView::WebUI> channel;
        String token;
    };
    mutable HashMap<u64, Channel> m_channels;
    mutable u64 m_next_channel { 1 };
    void request_ui(u64 view_id, String operation, JsonValue payload, Completion);
    void finish(u64 request_id, ErrorOr<JsonValue>);
    void dispatch(u64 request_id, u64 view_id, String const&, JsonValue const&);
    void credential_command(u64 request_id, View&, String const&, JsonValue const&);
    ConnectionFromUI& m_connection;
    OwnPtr<Ladybird::DerpBirdValueHost> m_values;
    QPointer<Ladybird::DerpBirdAutomationBackend> m_automation;
    QHash<u64, QPointer<Ladybird::DerpBirdDevToolsSession>> m_devtools;
    HashMap<u64, Completion> m_ui_pending;
    HashMap<u64, Completion> m_shell_pending;
    HashMap<u64, QPointer<QObject>> m_tasks;
    QHash<QString, QPointer<QObject>> m_tabs;
    u64 m_next_ui_request { 1 };
    u64 m_next_shell_request { 1 };
};

}
