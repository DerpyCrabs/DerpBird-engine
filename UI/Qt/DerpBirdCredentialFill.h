#pragma once

#include <AK/JsonObject.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentPage.h>
#include <QPointer>
#include <QTimer>
#include <UI/Qt/Application.h>
#include <UI/Qt/WebContentView.h>

namespace Ladybird {

// One explicit trusted-shell action. The saved secret never enters the shell:
// metadata chooses a record, canonical browser state authorizes its destination.
class DerpBirdCredentialFill final : public QObject {
public:
    enum class Mode { Fill, Capture };
    using Completion = WebView::ViewImplementation::DerpBirdCompletion;
    DerpBirdCredentialFill(QObject* parent, WebContentView& view, JsonValue identity, Function<bool()> valid, Completion completion, Mode mode = Mode::Fill)
        : QObject(parent), m_view(&view), m_identity(move(identity)), m_valid(move(valid)), m_completion(move(completion)), m_mode(mode)
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { fail(); });
    }
    Function<bool()> document_authority() const
    {
        return [document = m_document, page = m_page, origin = m_origin] {
            return document && page && page->is_open() && document->host() == page
                && !document->browsing_context().has_been_discarded()
                && document->browsing_context().active_document().ptr() == document.ptr()
                && page->hosted_document_with_id(document->id()) == document
                && document->origin().serialize() == origin;
        };
    }
    void cancel() { fail(); }
    ~DerpBirdCredentialFill() override { m_deleting = true; if (m_stage != Stage::Done) finish(Error::from_string_literal("Password fill cancelled")); }
    void start()
    {
        if (!m_view || !m_valid() || !m_identity.is_object()) { fail(); return; }
        auto& traversable = m_view->page().traversable();
        auto focused = traversable.focused_navigable_id();
        traversable.for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable& navigable) {
            if (focused.has_value() && navigable.id() == *focused)
                m_document = navigable.active_document();
            return IterationDecision::Continue;
        });
        if (!m_document) m_document = traversable.active_document();
        m_page = m_document->host();
        auto requested_origin = m_identity.as_object().get_string("origin"sv);
        m_origin = m_document->origin().serialize();
        if ((m_mode == Mode::Fill && (!requested_origin.has_value() || *requested_origin != m_origin)) || m_document->origin().is_opaque()
            || !m_document->creation_url().scheme().is_one_of("http"sv, "https"sv)
            || !current() || m_page->on_derpbird_credential_result) { fail(); return; }
        m_page->on_derpbird_credential_result = [self = QPointer { this }](auto document, u64 request, JsonValue result, Optional<String> error) {
            if (!self || document != self->m_document->id() || request != self->m_request
                || (self->m_stage != Stage::Selecting && self->m_stage != Stage::Filling && self->m_stage != Stage::Capturing)) return;
            auto stage = self->m_stage;
            // Finish clears the endpoint callback. Recheck correlation after the
            // deferral too: duplicate replies may have queued before the first
            // response advanced the operation to another stage.
            QTimer::singleShot(0, self, [self, request, stage, result = move(result), error = move(error)]() mutable {
                if (self && self->m_request == request && self->m_stage == stage)
                    self->receive(move(result), move(error));
            });
        };
        m_owns_callback = true;
        m_stage = Stage::Selecting;
        m_request = next_request();
        m_timer.start(25000);
        m_page->async_request_derpbird_credential_fields(m_document->id(), m_request);
    }
private:
    enum class Stage { Starting, Selecting, Loading, Filling, Capturing, Done };
    static u64 next_request() { static u64 next = 1; VERIFY(next != NumericLimits<u64>::max()); return next++; }
    bool current() const
    {
        return m_view && m_valid() && !m_view->crash_overlay_active() && m_document && m_page && m_page->is_open()
            && m_document->host() == m_page && !m_document->browsing_context().has_been_discarded()
            && m_document->browsing_context().active_document().ptr() == m_document.ptr()
            && m_page->hosted_document_with_id(m_document->id()) == m_document
            && m_document->origin().serialize() == m_origin;
    }
    void receive(JsonValue result, Optional<String> error)
    {
        if (m_stage == Stage::Done) return;
        if (!current() || error.has_value()) { fail(); return; }
        if (m_stage == Stage::Filling) { finish(JsonValue {}); return; }
        if (m_stage == Stage::Capturing) {
            if (!result.is_object()) { fail(); return; }
            auto const& object = result.as_object();
            auto username = object.get_string("username"sv);
            auto password = object.get_string("password"sv);
            if (object.get_string("origin"sv) != m_origin || !username.has_value() || !password.has_value()
                || username->bytes().size() > 1024 || password->bytes().size() > 65536) { fail(); return; }
            // Native completion only: Chrome converts this to a metadata proposal.
            finish(move(result));
            return;
        }
        if (m_stage != Stage::Selecting || !result.is_object()) { fail(); return; }
        auto origin = result.as_object().get_string("origin"sv);
        auto ticket = result.as_object().get_string("ticket"sv);
        auto kind = result.as_object().get_string("kind"sv);
        if (!origin.has_value() || *origin != m_origin || (kind != "login"sv && !(m_mode == Mode::Capture && kind == "new-password"sv))
            || !ticket.has_value() || ticket->bytes().size() > 20) { fail(); return; }
        auto parsed = ticket->to_number<u64>();
        if (!parsed.has_value() || !*parsed || String::number(*parsed) != *ticket) { fail(); return; }
        m_ticket = *parsed;
        if (m_mode == Mode::Capture) {
            m_stage = Stage::Capturing;
            m_request = next_request();
            m_page->async_capture_derpbird_credential_fields(m_document->id(), m_request, m_ticket, m_origin);
            return;
        }
        m_stage = Stage::Loading;
        auto payload = m_identity;
        payload.as_object().set("type"sv, "credentials.for-fill"sv);
        Application::the().derpbird_credentials_request(payload, false, [self = QPointer { this }](ErrorOr<JsonValue> response) {
            if (!self || self->m_stage != Stage::Loading) return;
            if (!self->current() || response.is_error()) { self->fail(); return; }
            auto value = response.release_value();
            if (!value.is_object()) { self->fail(); return; }
            auto const& entry = value.as_object();
            auto username = entry.get_string("username"sv);
            auto password = entry.get_string("password"sv);
            if (!username.has_value() || !password.has_value() || entry.get_string("origin"sv) != self->m_origin
                || entry.get_string("id"sv) != self->m_identity.as_object().get_string("id"sv)
                || entry.get_string("revision"sv) != self->m_identity.as_object().get_string("revision"sv)) { self->fail(); return; }
            self->m_stage = Stage::Filling;
            self->m_request = next_request();
            self->m_page->async_fill_derpbird_credential_fields(self->m_document->id(), self->m_request, self->m_ticket, self->m_origin, *username, *password);
        });
    }
    void fail() { finish(Error::from_string_literal("Не удалось заполнить: выберите поле пароля на том же сайте и повторите. Страница или запись могла измениться.")); }
    void finish(ErrorOr<JsonValue> result)
    {
        if (m_stage == Stage::Done) return;
        m_stage = Stage::Done;
        m_timer.stop();
        if (m_page && m_owns_callback) {
            m_page->on_derpbird_credential_result = nullptr;
            if (m_ticket && m_page->is_open()) m_page->async_cancel_derpbird_credential_fields(m_document->id(), m_ticket);
        }
        m_owns_callback = false;
        auto completion = move(m_completion);
        if (!m_deleting) deleteLater();
        if (completion) completion(move(result));
    }
    QPointer<WebContentView> m_view;
    RefPtr<WebView::CanonicalDocument> m_document;
    RefPtr<WebView::WebContentPage> m_page;
    JsonValue m_identity;
    String m_origin;
    Function<bool()> m_valid;
    Completion m_completion;
    QTimer m_timer;
    u64 m_request { 0 }, m_ticket { 0 };
    Stage m_stage { Stage::Starting };
    Mode m_mode;
    bool m_owns_callback { false };
    bool m_deleting { false };
};

}
