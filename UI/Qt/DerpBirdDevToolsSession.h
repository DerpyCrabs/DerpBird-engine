#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibWebCommon/WebView/DOMNodeProperties.h>
#include <LibWebCommon/WebView/Debugger.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibWebView/ViewImplementation.h>

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QObject>
#include <QPointer>
#include <memory>

namespace Ladybird {

// One trusted tools document owns one inspected view. Nothing is exposed to page JS.
class DerpBirdDevToolsSession final : public QObject {
public:
    explicit DerpBirdDevToolsSession(WebView::ViewImplementation& target, QObject* parent, Function<void(JsonValue)> publish)
        : QObject(parent), m_target(target.make_weak_ptr()), m_publish(AK::move(publish))
    {
        m_clock.start();
        m_flush.setParent(this);
        m_flush.setSingleShot(true);
        m_flush.setInterval(50);
        connect(&m_flush, &QTimer::timeout, this, [this] { flush(); });
        m_dom_refresh.setSingleShot(true); m_dom_refresh.setInterval(100);
        connect(&m_dom_refresh, &QTimer::timeout, this, [this] { if(m_target) { m_target->inspect_dom_tree(); if(!m_node.isEmpty()) inspect(Web::UniqueNodeID { m_node.toLongLong() }, false); } });
        target.on_dom_mutation_received = [this](auto) { if(!m_dom_refresh.isActive()) m_dom_refresh.start(); };
        target.set_listen_for_dom_mutations(true);
        target.on_received_dom_tree = [this](JsonObject tree) {
            m_dom = QJsonDocument::fromJson(to_qstring(JsonValue(AK::move(tree)).serialized()).toUtf8()).object();
            m_dom_dirty = true; schedule();
        };
        target.on_received_dom_node_properties = [this](WebView::DOMNodeProperties properties) {
            if (properties.type != WebView::DOMNodeProperties::Type::ComputedStyle) return;
            m_style_loading = false;
            if (m_style_node != m_node) { inspect(Web::UniqueNodeID { m_node.toLongLong() }, false); return; }
            auto styles = QJsonDocument::fromJson(to_qstring(properties.properties.serialized()).toUtf8());
            m_styles = styles.object(); m_styles_dirty = true; schedule();
        };
        target.on_console_message = [this](WebView::ConsoleOutput output) {
            output.output.visit([this](WebView::ConsoleLog const& log) {
                QStringList parts;
                for (auto const& argument : log.arguments) parts.append(argument.is_string() ? to_qstring(argument.as_string()) : to_qstring(argument.serialized()));
                message(log.level == JS::ConsoleLogLevel::Error ? "error" : log.level == JS::ConsoleLogLevel::Warn ? "warning" : "info", parts.join(' '));
            }, [this](WebView::ConsoleError const& error) { message("error", to_qstring(error.name) + ": " + to_qstring(error.message)); },
                [this](WebView::ConsoleTrace const& trace) { message("info", to_qstring(trace.label)); }, [](WebView::ConsoleGroupEnd const&) {});
        };
        target.on_received_js_console_result = [this](WebView::DebuggerValue value) {
            m_evaluation_waiting = false;
            QString text;
            switch (value.type) {
            case WebView::DebuggerValueType::Undefined: text = "undefined"; break;
            case WebView::DebuggerValueType::Null: text = "null"; break;
            case WebView::DebuggerValueType::Boolean: text = value.boolean_value ? "true" : "false"; break;
            case WebView::DebuggerValueType::Number: text = QString::number(value.number_value, 'g', 16); break;
            case WebView::DebuggerValueType::Object: text = to_qstring(value.object_class) + " " + to_qstring(value.grip_fields.serialized()); break;
            default: text = to_qstring(value.text.to_utf8()); break;
            }
            if (auto done = std::exchange(m_evaluation, {})) (*done)(JsonValue(to_string(text)));
        };
        target.on_network_request_started = [this](u64 id, URL::URL const& url, ByteString const& method, auto const& headers, ByteBuffer body, Optional<String> initiator, String, bool navigation, auto) {
            if (m_requests.size() >= 10000) { if (!m_limit_reported) { m_limit_reported = true; message("warning", "Network capture reached 10,000 requests. Clear the log to continue."); } return; }
            auto key = QString::number(id);
            auto kind = initiator.has_value() ? to_qstring(*initiator) : QString {};
            QString type = navigation ? "Doc" : kind == "fetch" || kind == "xmlhttprequest" ? "Fetch/XHR" : "Other";
            auto payload = body.size() <= 1024 * 1024 ? QString::fromUtf8(reinterpret_cast<char const*>(body.data()), body.size()) : QString("Request payload exceeds 1 MiB capture limit");
            m_requests[key] = QJsonObject { {"id", key}, {"url", to_qstring(url.serialize())}, {"method", QString::fromUtf8(method.characters(), method.length())}, {"status", 0}, {"type", type}, {"bytes", 0}, {"start", m_clock.elapsed()}, {"duration", 0}, {"initiator", kind}, {"requestHeaders", header_object(headers)}, {"headers", QJsonObject {}}, {"payload", payload}, {"complete", false} };
            m_dirty.insert(key); schedule();
        };
        target.on_network_response_headers_received = [this](u64 id, u32 status, auto const&, auto const& headers, auto) {
            auto key = QString::number(id); if (!m_requests.contains(key)) return;
            auto values = header_object(headers);
            auto& request = m_requests[key]; request["status"] = static_cast<int>(status); request["headers"] = values;
            auto mime = values.value("content-type").toString();
            if (request.value("type") != "Fetch/XHR" && request.value("type") != "Doc") {
                request["type"] = mime.contains("javascript") ? "JS" : mime.contains("css") ? "CSS" : mime.startsWith("image/") ? "Img" : mime.contains("font") ? "Font" : "Other";
            }
            m_dirty.insert(key); schedule();
        };
        target.on_network_response_body_received = [this](u64 id, ByteBuffer bytes) {
            auto key = QString::number(id); if (!m_requests.contains(key)) return;
            if (bytes.size() > 8 * 1024 * 1024 || m_body_bytes + bytes.size() > 64 * 1024 * 1024) { m_body_errors.insert(key); return; }
            auto mime = m_requests[key].value("headers").toObject().value("content-type").toString().section(';', 0, 0).trimmed().toLower();
            if (mime.startsWith("image/")) {
                auto encoded = QByteArray(reinterpret_cast<char const*>(bytes.data()), bytes.size()).toBase64();
                m_body_bytes += bytes.size();
                m_images[key] = QJsonObject { {"encoding", "base64"}, {"mimeType", mime}, {"data", QString::fromLatin1(encoded)}, {"bytes", static_cast<double>(bytes.size())} };
                return;
            }
            auto text = String::from_utf8(StringView(bytes.bytes()));
            if (text.is_error()) { m_body_errors.insert(key); return; }
            m_body_bytes += bytes.size(); m_bodies[key] = to_qstring(text.release_value());
        };
        target.on_network_request_finished = [this](u64 id, u64 size, Requests::RequestTimingInfo const& timing, auto const& error) {
            auto key = QString::number(id); if (!m_requests.contains(key)) return;
            auto& request = m_requests[key]; request["bytes"] = static_cast<double>(size); request["complete"] = true;
            request["duration"] = timing.response_end_microseconds > timing.request_start_microseconds ? (timing.response_end_microseconds - timing.request_start_microseconds) / 1000.0 : m_clock.elapsed() - request["start"].toDouble();
            if (error.has_value()) { request["failed"] = true; m_body_errors.insert(key); }
            m_dirty.insert(key); schedule();
        };
        target.on_devtools_source_available = [this](auto source) { add_source(source); schedule(); };
        WebView::ViewImplementation::NavigationListener navigation;
        navigation.on_load_start = [this](auto const&) {
            ++m_generation; m_evaluation_waiting = false; m_style_loading = false; m_dom = {}; m_styles = {}; m_node.clear(); m_sources = {}; m_source_ids.clear(); m_dom_dirty = m_styles_dirty = m_sources_dirty = true;
            if (auto done = std::exchange(m_evaluation, {})) (*done)(Error::from_string_literal("Page navigated"));
            schedule();
        };
        navigation.on_load_finish = [this](auto const&) { if(m_target) m_target->set_listen_for_dom_mutations(true); refresh(); };
        m_navigation = target.add_navigation_listener(AK::move(navigation));
        target.did_connect_devtools_client();
        target.start_sending_console_output(true, true);
    }
    void request(JsonValue const& value, WebView::ViewImplementation::DerpBirdCompletion completion)
    {
            if (!m_target || !value.is_object()) { completion(Error::from_string_literal("Inspected tab closed")); return; }
            auto const& request = value.as_object();
            auto type = request.get_string("type"sv);
            if (type == "devtools.bootstrap"sv) {
                m_ready = true;
                completion(to_ak(snapshot()));
                m_new_messages = {}; m_dirty.clear();
                refresh();
            } else if (type == "devtools.response"sv) {
                auto id = to_qstring(request.get_string("id"sv).value_or(String {}));
                if (!m_requests.contains(id) || !m_requests[id].value("complete").toBool()) { completion(Error::from_string_literal("Response is not complete")); return; }
                if (m_body_errors.contains(id)) { completion(Error::from_string_view("Response unavailable: unsupported binary data, invalid UTF-8, or capture limit exceeded"sv)); return; }
                if (m_images.contains(id)) completion(to_ak(m_images.value(id)));
                else completion(JsonValue(to_string(m_bodies.value(id))));
            } else if (type == "devtools.evaluate"sv) {
                auto expression = request.get_string("expression"sv);
                if (!expression.has_value() || expression->bytes().size() > 65536 || m_evaluation_waiting) { completion(Error::from_string_literal("Invalid or concurrent console evaluation")); return; }
                m_evaluation_waiting = true;
                m_evaluation = std::make_shared<WebView::ViewImplementation::DerpBirdCompletion>(AK::move(completion));
                m_target->js_console_input(*expression);
                auto pending = m_evaluation;
                QTimer::singleShot(10000, this, [this, pending] { if (m_evaluation == pending) { m_evaluation.reset(); (*pending)(Error::from_string_literal("Console evaluation timed out")); } });
            } else if (type == "devtools.select"sv) {
                auto id = request.get_string("id"sv);
                auto number = id.has_value() ? id->to_number<i64>() : Optional<i64> {};
                if (!number.has_value() || *number <= 0) { completion(Error::from_string_literal("Invalid DOM node")); return; }
                inspect(Web::UniqueNodeID { *number });
                completion(JsonValue {});
            } else if (type == "devtools.source"sv) {
                auto key = to_qstring(request.get_string("id"sv).value_or(String {}));
                if (!m_source_ids.contains(key)) { completion(Error::from_string_literal("Source no longer exists")); return; }
                auto id = m_source_ids[key];
                if (m_target->on_received_devtools_source.contains(id)) { completion(Error::from_string_literal("Source is already loading")); return; }
                auto done = std::make_shared<WebView::ViewImplementation::DerpBirdCompletion>(AK::move(completion));
                m_pending_sources.append(id);
                m_target->on_received_devtools_source.set(id, [done](auto source) {
                    if (!source.has_value()) (*done)(Error::from_string_literal("Source is unavailable"));
                    else (*done)(JsonValue(source->text.to_utf8()));
                });
                m_target->request_devtools_source(id);
            } else if (type == "devtools.clear"sv) {
                m_requests.clear(); m_bodies.clear(); m_images.clear(); m_body_errors.clear(); m_dirty.clear(); m_body_bytes = 0; m_limit_reported = false;
                completion(JsonValue {});
            } else if (type == "devtools.refresh"sv) {
                refresh(); completion(JsonValue {});
            } else completion(Error::from_string_literal("Unknown developer tools request"));
    }
    ~DerpBirdDevToolsSession() override
    {
        if (m_evaluation) (*m_evaluation)(Error::from_string_literal("Developer tools closed"));
        if (!m_target) return;
        for (auto const& id : m_pending_sources) m_target->on_received_devtools_source.remove(id);
        m_target->remove_navigation_listener(m_navigation);
        m_target->set_listen_for_dom_mutations(false);
        m_target->on_dom_mutation_received = nullptr;
        m_target->on_received_dom_tree = nullptr;
        m_target->on_received_dom_node_properties = nullptr;
        m_target->on_console_message = nullptr;
        m_target->on_received_js_console_result = nullptr;
        m_target->on_network_request_started = nullptr;
        m_target->on_network_response_headers_received = nullptr;
        m_target->on_network_response_body_received = nullptr;
        m_target->on_network_request_finished = nullptr;
        m_target->on_devtools_source_available = nullptr;
        m_target->clear_highlighted_dom_node();
        m_target->did_disconnect_devtools_client();
    }
    void inspect(Optional<Web::UniqueNodeID> node = {}, bool reveal = true)
    {
        if (node.has_value() && m_target) {
            m_node = QString::number(node->value());
            m_reveal_node |= reveal;
            m_styles = {}; m_styles_dirty = true;
            if (reveal) m_target->highlight_dom_node(*node, {});
            if (m_style_loading) return;
            m_style_loading = true; m_style_node = m_node;
            m_target->inspect_dom_node(*node, WebView::DOMNodeProperties::Type::ComputedStyle, {});
            schedule();
        }
    }
private:
    static QString to_qstring(StringView value) { return QString::fromUtf8(value.characters_without_null_termination(), value.length()); }
    static String to_string(QString const& value) { auto bytes = value.toUtf8(); return MUST(String::from_utf8(StringView(bytes.constData(), bytes.size()))); }
    static JsonValue to_ak(QJsonObject const& object) { auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact); return MUST(JsonValue::from_string(StringView(bytes.constData(), bytes.size()))); }
    template<typename Headers> static QJsonObject header_object(Headers const& headers)
    {
        QJsonObject values;
        for (auto const& header : headers) {
            auto name = QString::fromLatin1(header.name.characters(), header.name.length()).toLower();
            auto value = QString::fromLatin1(header.value.characters(), header.value.length());
            values[name] = values.contains(name) ? values[name].toString() + "\n" + value : value;
        }
        return values;
    }
    void message(QString level, QString text)
    {
        QJsonObject entry { {"level", level}, {"text", text} };
        if (m_messages.size() >= 2000) m_messages.removeFirst();
        if (m_new_messages.size() >= 2000) m_new_messages.removeFirst();
        m_messages.append(entry); m_new_messages.append(entry); schedule();
    }
    void add_source(Web::HTML::ScriptRegistryDescription const& source)
    {
        auto key = QString::number(source.id.document_id.value()) + ":" + QString::number(source.id.script_id);
        if (m_source_ids.contains(key) || m_source_ids.size() >= 5000) return;
        m_source_ids.insert(key, source.id);
        auto name = source.url.has_value() ? to_qstring(source.url->serialize()) : to_qstring(source.display_url.to_utf8());
        if (name.isEmpty()) name = "(inline script)";
        m_sources.append(QJsonObject { {"id", key}, {"name", name}, {"text", ""} }); m_sources_dirty = true;
    }
    void refresh()
    {
        if (!m_target) return;
        m_target->inspect_dom_tree();
        auto generation = m_generation;
        m_target->retrieve_devtools_sources([self = QPointer(this), generation](auto result) {
            if (!self || self->m_generation != generation || result.is_error()) return;
            for (auto const& source : result.value()) self->add_source(source);
            self->schedule();
        });
    }
    QJsonObject snapshot() const
    {
        QJsonArray requests;
        for (auto const& record : m_requests) requests.append(record);
        return { {"requests", requests}, {"dom", m_dom}, {"messages", m_messages}, {"sources", m_sources}, {"nodeId", m_node}, {"styles", m_styles} };
    }
    void schedule() { if (!m_flush.isActive()) m_flush.start(); }
    void flush()
    {
        if (!m_ready) return;
        QJsonObject update;
        if (!m_dirty.isEmpty()) { QJsonArray rows; for (auto const& id : m_dirty) rows.append(m_requests[id]); update["requests"] = rows; m_dirty.clear(); }
        if (!m_new_messages.isEmpty()) { update["messages"] = m_new_messages; m_new_messages = {}; }
        if (m_dom_dirty) { update["dom"] = m_dom; m_dom_dirty = false; }
        if (m_sources_dirty) { update["sources"] = m_sources; m_sources_dirty = false; }
        if (m_styles_dirty) { update["styles"] = m_styles; update["nodeId"] = m_node; m_styles_dirty = false; }
        if (m_reveal_node) { update["revealElement"] = true; m_reveal_node = false; }
        if (!update.isEmpty()) m_publish(to_ak(update));
    }
    WeakPtr<WebView::ViewImplementation> m_target;
    Function<void(JsonValue)> m_publish;
    QTimer m_flush, m_dom_refresh;
    QElapsedTimer m_clock;
    QHash<QString, QJsonObject> m_requests;
    QHash<QString, QString> m_bodies;
    QHash<QString, QJsonObject> m_images;
    QSet<QString> m_body_errors, m_dirty;
    QHash<QString, Web::HTML::ScriptRegistryIdentifier> m_source_ids;
    Vector<Web::HTML::ScriptRegistryIdentifier> m_pending_sources;
    QJsonArray m_messages, m_new_messages, m_sources;
    QJsonObject m_dom, m_styles;
    QString m_node, m_style_node;
    bool m_reveal_node { false };
    bool m_style_loading { false }, m_evaluation_waiting { false };
    size_t m_body_bytes { 0 };
    u64 m_navigation { 0 }, m_generation { 0 };
    bool m_ready { false }, m_dom_dirty { false }, m_sources_dirty { false }, m_styles_dirty { false }, m_limit_reported { false };
    std::shared_ptr<WebView::ViewImplementation::DerpBirdCompletion> m_evaluation;
};
}
