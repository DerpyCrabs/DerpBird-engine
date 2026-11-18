#pragma once

#include <AK/JsonArray.h>
#include <UI/Qt/DerpBirdAutomationBackend.h>
#if defined(LADYBIRD_HUB_IN_OWN_PROCESS)
#    include <UI/Qt/HubConnection.h>
#endif
#include <LibWebView/ViewImplementation.h>
#include <UI/Qt/StringUtils.h>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <memory>

namespace Ladybird {

// Private, owner-only IPC. BiDi owns page semantics; this transport only correlates
// requests and forwards events. MCP/CLI adapters live outside the browser process.
class DerpBirdAutomation final : public QObject {
public:
    using Completion = Function<void(ErrorOr<JsonValue>)>;
    using WindowRequest = Function<void(JsonValue const&, Completion)>;
    struct Window {
        QPointer<QObject> owner;
        Function<JsonValue()> state;
        WindowRequest request;
        Function<void()> changed;
    };

    static DerpBirdAutomation& the()
    {
        static QPointer<DerpBirdAutomation> server;
        if (!server) server = new DerpBirdAutomation;
        return *server;
    }

    ~DerpBirdAutomation() override
    {
        // QLocalServer destroys its sockets after the request and window maps are gone.
        for (auto* socket : m_clients.keys())
            QObject::disconnect(socket, nullptr, this, nullptr);
    }

    void add_window(QString id, Window window) { m_windows.insert(id, std::make_shared<Window>(AK::move(window))); }
    void remove_window(QString const& id) { m_windows.remove(id); }
    bool enabled() const { return m_server.isListening(); }
    QString endpoint() const { return m_server.fullServerName(); }
    int client_count() const { return m_clients.size(); }

    ErrorOr<void> enable()
    {
        if (enabled()) return {};
        if (WebView::WebDriverBiDi::the())
            return Error::from_string_literal("This browser already has a WebDriver session");
        auto path = qEnvironmentVariable("DERPBIRD_AUTOMATION_SOCKET");
        if (path.isEmpty())
            path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QString("/derpbird-automation-%1.sock").arg(QCoreApplication::applicationPid());
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        if (!m_server.listen(path)) { auto message = ak_string_from_qstring(m_server.errorString()); return Error::from_string_view(message.bytes_as_string_view()); }
#if defined(LADYBIRD_HUB_IN_OWN_PROCESS)
        auto started = HubConnection::the().request_derpbird_sync(0, "automation.enable"_string);
        if (started.is_error()) { m_server.close(); return started.release_error(); }
#else
        m_backend = std::make_unique<DerpBirdAutomationBackend>(this, [this](JsonValue packet) { publish_event(AK::move(packet)); });
#endif
        notify_windows();
        return {};
    }

    void publish_event(JsonValue packet)
    {
        auto bytes = qstring_from_ak_string(packet.serialized()).toUtf8();
        for (auto* socket : m_clients.keys()) send(*socket, bytes);
    }

    void disable()
    {
        m_server.close();
        for (auto* socket : m_clients.keys()) socket->abort();
        m_pending.clear();
#if defined(LADYBIRD_HUB_IN_OWN_PROCESS)
        if (auto* hub = HubConnection::the_if_connected()) hub->request_derpbird(0, "automation.disable"_string, {}, [](auto) {});
#else
        m_backend.reset();
#endif
        notify_windows();
    }

private:
    DerpBirdAutomation() : QObject(QCoreApplication::instance()), m_server(this)
    {
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (auto* socket = m_server.nextPendingConnection()) {
                if (m_clients.size() >= 16) { socket->abort(); socket->deleteLater(); continue; }
                socket->setReadBufferSize(max_packet + 1);
                m_clients.insert(socket, {});
                notify_windows();
                connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                    m_clients.remove(socket);
                    notify_windows();
                    for (auto it = m_pending.begin(); it != m_pending.end();) {
                        if (it->socket == socket) it = m_pending.erase(it); else ++it;
                    }
                    socket->deleteLater();
                });
                connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                    auto& buffer = m_clients[socket];
                    buffer += socket->readAll();
                    if (buffer.size() > max_packet) { socket->abort(); return; }
                    while (true) {
                        auto end = buffer.indexOf('\n');
                        if (end < 0) break;
                        auto line = buffer.left(end); buffer.remove(0, end + 1);
                        receive(*socket, line);
                        if (!m_clients.contains(socket)) return;
                    }
                });
            }
        });
    }

    void notify_windows()
    {
        for (auto const& window : m_windows) if (window->owner && window->changed) window->changed();
    }

    void send(QLocalSocket& socket, QByteArray bytes)
    {
        if (socket.bytesToWrite() + bytes.size() > max_packet * 4) { socket.abort(); return; }
        socket.write(bytes); socket.write("\n");
    }

    void finish(u64 id, JsonValue result, QString error = {}, QString message = {})
    {
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return;
        auto pending = *it; m_pending.erase(it);
        if (!pending.socket) return;
        JsonObject packet;
        packet.set("id"sv, pending.id);
        packet.set("type"sv, error.isEmpty() ? "success"sv : "error"sv);
        if (error.isEmpty()) packet.set("result"sv, AK::move(result));
        else { packet.set("error"sv, ak_string_from_qstring(error)); packet.set("message"sv, ak_string_from_qstring(message)); }
        auto serialized = packet.serialized();
        send(*pending.socket, qstring_from_ak_string(serialized).toUtf8());
    }

    void receive(QLocalSocket& socket, QByteArray const& bytes)
    {
        auto value = JsonValue::from_string(StringView(bytes.constData(), bytes.size()));
        if (value.is_error() || !value.value().is_object()) { socket.abort(); return; }
        auto const& packet = value.value().as_object();
        auto original_id = packet.get_integer<u64>("id"sv);
        auto method = packet.get_string("method"sv);
        auto params = packet.get_object("params"sv);
        if (!original_id.has_value() || !method.has_value() || !params.has_value() || m_pending.size() >= 256) { socket.abort(); return; }
        for (auto const& pending : m_pending) {
            if (pending.socket == &socket && pending.id == *original_id) { socket.abort(); return; }
        }
        auto id = ++m_next_id;
        m_pending.insert(id, { &socket, *original_id });
        QTimer::singleShot(60000, this, [this, id] { finish(id, {}, "timeout", "Automation command timed out"); });
        if (*method == "derpbird.windows"sv) {
            JsonArray windows;
            for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
                if (!it.value()->owner) continue;
                auto state = it.value()->state();
                state.as_object().set("windowId"sv, ak_string_from_qstring(it.key()));
                MUST(windows.append(AK::move(state)));
            }
            JsonObject result; result.set("windows"sv, AK::move(windows)); finish(id, AK::move(result)); return;
        }
        if (*method == "derpbird.request"sv) {
            auto window_id = params->get_string("windowId"sv);
            auto payload = params->get("payload"sv);
            auto window = window_id.has_value() ? m_windows.value(qstring_from_ak_string(*window_id)) : nullptr;
            if (!window || !window->owner || !payload.has_value()) { finish(id, {}, "no such window", "Select an available DerpBird window"); return; }
            window->request(*payload, [this, id](auto result) {
                if (result.is_error()) finish(id, {}, "unknown error", qstring_from_ak_string(MUST(String::formatted("{}", result.error()))));
                else finish(id, result.release_value());
            });
            return;
        }
        auto complete = [self = QPointer(this), id](JsonValue packet) {
            if (!self) return;
            auto const& object = packet.as_object();
            if (object.get_string("type"sv) == "error"sv)
                self->finish(id, {}, qstring_from_ak_string(object.get_string("error"sv).value()), qstring_from_ak_string(object.get_string("message"sv).value()));
            else self->finish(id, object.get("result"sv).value_or(JsonValue {}));
        };
#if defined(LADYBIRD_HUB_IN_OWN_PROCESS)
        JsonObject command; command.set("method"sv, *method); command.set("params"sv, JsonValue { *params });
        HubConnection::the().request_derpbird(0, "automation.command"_string, AK::move(command), [self = QPointer(this), id, complete = AK::move(complete)](auto result) {
            if (!self) return;
            if (result.is_error()) self->finish(id, {}, "unknown error", qstring_from_ak_string(MUST(String::formatted("{}", result.error()))));
            else complete(result.release_value());
        });
#else
        m_backend->request(*method, JsonValue { *params }, AK::move(complete));
#endif
    }

    static constexpr qsizetype max_packet = 16 * 1024 * 1024;
    struct Pending { QPointer<QLocalSocket> socket; u64 id; };
    QLocalServer m_server;
    QHash<QLocalSocket*, QByteArray> m_clients;
    QHash<u64, Pending> m_pending;
    QHash<QString, std::shared_ptr<Window>> m_windows;
    std::unique_ptr<DerpBirdAutomationBackend> m_backend;
    u64 m_next_id { 0 };
};

}
