#pragma once

#include <AK/JsonObject.h>
#include <QPointer>
#include <QTimer>
#include <UI/Qt/Application.h>
#include <UI/Qt/WebContentView.h>

namespace Ladybird {

// The Hub owns the canonical document and the single-use field ticket. The UI
// keeps secrets in native storage and sends them only to that checked endpoint.
class DerpBirdCredentialFill final : public QObject {
public:
    enum class Mode { Fill, Capture };
    using Completion = RemoteView::DerpBirdCompletion;
    DerpBirdCredentialFill(QObject* parent, WebContentView& view, JsonValue identity, Function<bool()> valid, Completion completion, Mode mode = Mode::Fill)
        : QObject(parent), m_view(&view), m_identity(move(identity)), m_valid(move(valid)), m_completion(move(completion)), m_mode(mode)
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { fail(); });
    }
    Function<bool()> document_authority() const
    {
        return [view = m_view, document = m_document, origin = m_origin] {
            return view && view->derpbird_page_open() && view->derpbird_document_is_current(document, origin);
        };
    }
    void cancel() { fail(); }
    ~DerpBirdCredentialFill() override { m_deleting = true; if (!m_done) finish(Error::from_string_literal("Password fill cancelled")); }
    void start()
    {
        if (!current() || !m_identity.is_object()) { fail(); return; }
        m_timer.start(25000);
        JsonObject payload;
        payload.set("capture"sv, m_mode == Mode::Capture);
        if (auto origin = m_identity.as_object().get_string("origin"sv); origin.has_value()) payload.set("origin"sv, *origin);
        m_request = m_view->derpbird_command("credential.select"_string, move(payload), [self = QPointer(this)](ErrorOr<JsonValue> result) {
            if (self) self->selected(move(result));
        });
    }
private:
    bool current() const { return !m_done && m_view && m_valid() && m_view->derpbird_page_open() && !m_view->crash_overlay_active(); }
    void selected(ErrorOr<JsonValue> result)
    {
        m_request = 0;
        if (!current() || result.is_error() || !result.value().is_object()) { fail(); return; }
        auto const& object = result.value().as_object();
        auto document = object.get_string("document"sv), origin = object.get_string("origin"sv), ticket = object.get_string("ticket"sv), kind = object.get_string("kind"sv);
        if (!document.has_value() || !origin.has_value() || !ticket.has_value() || ticket->is_empty()
            || (kind != "login"sv && !(m_mode == Mode::Capture && kind == "new-password"sv))
            || (m_mode == Mode::Fill && object.get_string("origin"sv) != m_identity.as_object().get_string("origin"sv))) { fail(); return; }
        m_document = *document; m_origin = *origin; m_ticket = *ticket;
        if (m_mode == Mode::Capture) { send_fields("credential.capture"_string, {}); return; }
        auto request = m_identity;
        request.as_object().set("type"sv, "credentials.for-fill"sv);
        Application::the().derpbird_credentials_request(request, false, [self = QPointer(this)](ErrorOr<JsonValue> response) {
            if (!self) return;
            if (!self->current() || response.is_error() || !response.value().is_object()) { self->fail(); return; }
            auto const& entry = response.value().as_object();
            if (entry.get_string("origin"sv) != self->m_origin
                || entry.get_string("id"sv) != self->m_identity.as_object().get_string("id"sv)
                || entry.get_string("revision"sv) != self->m_identity.as_object().get_string("revision"sv)
                || !entry.get_string("username"sv).has_value() || !entry.get_string("password"sv).has_value()) { self->fail(); return; }
            JsonObject fields;
            fields.set("username"sv, *entry.get_string("username"sv)); fields.set("password"sv, *entry.get_string("password"sv));
            self->send_fields("credential.fill"_string, move(fields));
        });
    }
    JsonObject selection() const
    {
        JsonObject payload; payload.set("document"sv, m_document); payload.set("origin"sv, m_origin); payload.set("ticket"sv, m_ticket);
        return payload;
    }
    void send_fields(String command, JsonObject payload)
    {
        if (!current()) { fail(); return; }
        payload.set("document"sv, m_document); payload.set("origin"sv, m_origin); payload.set("ticket"sv, m_ticket);
        m_request = m_view->derpbird_command(move(command), move(payload), [self = QPointer(this)](ErrorOr<JsonValue> result) {
            if (!self) return;
            self->m_request = 0;
            if (!self->current() || result.is_error()) { self->fail(); return; }
            if (self->m_mode == Mode::Capture) {
                if (!result.value().is_object()) { self->fail(); return; }
                auto const& data = result.value().as_object();
                auto user = data.get_string("username"sv), password = data.get_string("password"sv);
                if (data.get_string("origin"sv) != self->m_origin || !user.has_value() || !password.has_value()
                    || user->bytes().size() > 1024 || password->bytes().size() > 65536) { self->fail(); return; }
                self->finish(result.release_value());
            } else self->finish(JsonValue {});
        });
    }
    void fail() { finish(Error::from_string_literal("Не удалось заполнить: выберите поле пароля на том же сайте и повторите. Страница или запись могла измениться.")); }
    void finish(ErrorOr<JsonValue> result)
    {
        if (m_done) return;
        m_done = true; m_timer.stop();
        if (auto* hub = HubConnection::the_if_connected()) {
            if (m_request) hub->cancel_derpbird_request(exchange(m_request, 0));
            if (m_view && !m_ticket.is_empty()) hub->request_derpbird(m_view->view_id(), "credential.cancel"_string, selection(), [](auto) { });
        }
        auto completion = move(m_completion);
        if (!m_deleting) deleteLater();
        if (completion) completion(move(result));
    }
    QPointer<WebContentView> m_view;
    JsonValue m_identity;
    String m_document, m_origin, m_ticket;
    Function<bool()> m_valid;
    Completion m_completion;
    QTimer m_timer;
    u64 m_request { 0 };
    Mode m_mode;
    bool m_done { false }, m_deleting { false };
};
}
