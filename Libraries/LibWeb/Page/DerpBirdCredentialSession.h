#pragma once

#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/Time.h>
#include <AK/Weakable.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/Page/DerpBirdCredentialFields.h>

namespace Web {

// Native-only correlation for a host-authorized credential operation. Tickets are
// not permissions and are never exposed to page script. One selection per document
// bounds retained state; no saved password is retained between calls.
class DerpBirdCredentialSession final : public RefCounted<DerpBirdCredentialSession>, public Weakable<DerpBirdCredentialSession> {
public:
    AK_ALLOC_WITH_KMALLOC;
    using Clock = Function<MonotonicTime()>;
    struct Selection {
        u64 ticket;
        HTML::CrossProcessId document;
        String origin;
        DerpBirdCredentialFields::Kind kind;
    };
    static NonnullRefPtr<DerpBirdCredentialSession> create(DOM::Document& document, Clock clock = [] { return MonotonicTime::now(); })
    {
        auto session = adopt_ref(*new DerpBirdCredentialSession(document, move(clock)));
        session->m_observer->set_document_became_inactive([weak = session->make_weak_ptr()] {
            if (weak) weak->close();
        });
        return session;
    }
    ~DerpBirdCredentialSession() { close(); }

    ErrorOr<Selection> select(HTML::HTMLInputElement& password, bool require_visible = false)
    {
        // Even a failed new selection retires the previous candidate.
        cancel();
        if (m_closed || !m_document || &password.document() != m_document.ptr().ptr()
            || s_next_ticket == NumericLimits<u64>::max())
            return Error::from_string_literal("Credential document session is unavailable");
        auto fields = TRY(DerpBirdCredentialFields::select_from_focus(password, require_visible));
        Selection selection { s_next_ticket++, fields.document_id(), fields.origin(), fields.kind() };
        m_generation = selection.ticket;
        m_pending = Pending { selection.ticket, m_clock() + AK::Duration::from_seconds(30), move(fields) };
        return selection;
    }
    ErrorOr<void> fill(Selection const& selection, String const& username, String const& password)
    {
        NonnullRefPtr protect { *this };
        auto ticket = selection.ticket;
        auto fields = TRY(take(selection));
        return fields.fill(username, password, [this, ticket] { return !m_closed && m_generation == ticket; });
    }
    ErrorOr<DerpBirdCredentialFields::Values> capture(Selection const& selection)
    {
        auto fields = TRY(take(selection));
        return fields.capture();
    }
    void cancel() { m_generation = 0; m_pending.clear(); }
    void close()
    {
        if (m_closed) return;
        m_closed = true;
        cancel();
        m_observer->set_document_became_inactive(nullptr);
        m_observer = {};
        m_document = {};
    }
    bool is_closed() const { return m_closed; }

private:
    DerpBirdCredentialSession(DOM::Document& document, Clock clock)
        : m_document(&document), m_observer(DOM::DocumentObserver::create(document)), m_clock(move(clock)) { }
    ErrorOr<DerpBirdCredentialFields> take(Selection const& selection)
    {
        // Do not let an old reply consume a newer selection. Once the matching
        // ticket arrives it is consumed before any page callbacks can reenter.
        if (m_closed || !m_pending.has_value() || m_pending->ticket != selection.ticket)
            return Error::from_string_literal("Credential selection is unavailable");
        auto pending = m_pending.release_value();
        if (m_clock() >= pending.expires || pending.fields.document_id() != selection.document
            || pending.fields.origin() != selection.origin || pending.fields.kind() != selection.kind)
            return Error::from_string_literal("Credential selection expired or changed");
        return move(pending.fields);
    }
    struct Pending { u64 ticket; MonotonicTime expires; DerpBirdCredentialFields fields; };
    GC::Weak<DOM::Document> m_document;
    GC::Root<DOM::DocumentObserver> m_observer;
    Clock m_clock;
    Optional<Pending> m_pending;
    // All access is on the renderer main thread. Never recycle across sessions.
    static inline u64 s_next_ticket { 1 };
    u64 m_generation { 0 };
    bool m_closed { false };
};

}
