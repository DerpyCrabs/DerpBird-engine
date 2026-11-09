#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <AK/RefCounted.h>
#include <AK/ScopeGuard.h>
#include <AK/Weakable.h>
#include <AK/WeakPtr.h>
#include <LibCore/Timer.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>

namespace WebView {

// One channel per WebContent connection. Admission errors are explicit: callers
// must reserve/reject before committing, or terminate the affected subscription.
// This queue alone is not a transaction/event admission policy.
class DerpBirdValueEvents : public RefCounted<DerpBirdValueEvents>, public Weakable<DerpBirdValueEvents> {
public:
    AK_ALLOC_WITH_KMALLOC;
    using Send = Function<ErrorOr<void>(Web::PageId, Web::HTML::CrossProcessId, u64, ReadonlyBytes)>;
    using Completion = Function<void(ErrorOr<void>)>;
    class Reservation : public RefCounted<Reservation> {
    public:
        AK_ALLOC_WITH_KMALLOC;
        ~Reservation() { if (m_owner && m_id) m_owner->release_reservation(m_id); }
        ErrorOr<u64> enqueue(ByteBuffer payload, Completion completion)
        {
            NonnullRefPtr protect { *this };
            if (!m_owner || !m_id) return Error::from_string_literal("Userscript event reservation expired");
            NonnullRefPtr owner { *m_owner };
            auto id = exchange(m_id, 0);
            return owner->enqueue_reserved(id, move(payload), move(completion));
        }
        u64 id() const { return m_id; }
    private:
        friend class DerpBirdValueEvents;
        Reservation(DerpBirdValueEvents& owner, u64 id) : m_owner(owner.make_weak_ptr()), m_id(id) { }
        WeakPtr<DerpBirdValueEvents> m_owner;
        u64 m_id;
    };
    static constexpr size_t max_payload = 2 * 1024 * 1024 + 4096;
    static constexpr size_t max_bytes = 32 * 1024 * 1024;
    static constexpr size_t max_pending = 64;
    static constexpr size_t max_inflight = 4;
    static NonnullRefPtr<DerpBirdValueEvents> create(Send send, AK::Duration timeout = AK::Duration::from_seconds(30))
    {
        VERIFY(send && timeout > AK::Duration::zero());
        return adopt_ref(*new DerpBirdValueEvents(move(send), timeout));
    }
    ~DerpBirdValueEvents()
    {
        revoke_weak_ptrs();
        for (auto& entry : m_pending) entry.value->timer->stop();
    }
    ErrorOr<NonnullRefPtr<Reservation>> reserve(Web::PageId page, Web::HTML::CrossProcessId document, size_t bytes)
    {
        if (m_closed || page == 0 || !document.namespace_id || !document.local_id
            || !bytes || bytes > max_payload || m_pending.size() + m_reserved.size() >= max_pending
            || bytes > max_bytes - m_bytes || m_next_id == NumericLimits<u64>::max())
            return Error::from_string_literal("Userscript event queue is closed, full or invalid");
        auto id = m_next_id++;
        m_reserved.set(id, { page, document, bytes });
        m_bytes += bytes;
        return adopt_ref(*new Reservation(*this, id));
    }
    ErrorOr<u64> enqueue(Web::PageId page, Web::HTML::CrossProcessId document, ByteBuffer payload, Completion completion)
    {
        if (!completion) return Error::from_string_literal("Missing userscript event completion");
        auto reservation = TRY(reserve(page, document, payload.size()));
        return reservation->enqueue(move(payload), move(completion));
    }
    void acknowledge(Web::PageId page, Web::HTML::CrossProcessId document, u64 id, bool accepted)
    {
        auto found = m_pending.find(id);
        if (found == m_pending.end() || !found->value->sent || found->value->page != page || found->value->document != document) return;
        NonnullRefPtr owner { *this };
        if (accepted) finish(id, {});
        else finish(id, Error::from_string_literal("Userscript event rejected by document"));
    }
    void cancel_document(Web::PageId page, Web::HTML::CrossProcessId document)
    {
        NonnullRefPtr owner { *this };
        bool was_pumping = m_pumping;
        m_pumping = true;
        Vector<u64> reserved;
        for (auto const& [id, entry] : m_reserved)
            if (entry.page == page && entry.document == document) reserved.append(id);
        for (auto id : reserved) release_reservation(id);
        Vector<u64> ids;
        for (auto const& [id, pending] : m_pending)
            if (pending->page == page && pending->document == document) ids.append(id);
        for (auto id : ids) finish(id, Error::from_string_literal("Userscript event document closed"));
        m_pumping = was_pumping;
        if (!was_pumping) pump();
    }
    void close()
    {
        if (m_closed) return;
        NonnullRefPtr owner { *this };
        m_closed = true;
        for (auto const& entry : m_reserved) m_bytes -= entry.value.bytes;
        m_reserved.clear();
        Vector<u64> ids;
        for (auto const& entry : m_pending) ids.append(entry.key);
        for (auto id : ids) finish(id, Error::from_string_literal("Userscript event connection closed"));
        m_queue.clear();
    }
    size_t reserved() const { return m_reserved.size(); }
    size_t pending() const { return m_pending.size(); }
    size_t bytes() const { return m_bytes; }
    size_t inflight() const { return m_inflight; }
    size_t queued() const { return m_queue.size(); }

private:
    struct Reserved { Web::PageId page; Web::HTML::CrossProcessId document; size_t bytes; };
    void release_reservation(u64 id)
    {
        auto reservation = m_reserved.take(id);
        if (reservation.has_value()) m_bytes -= reservation->bytes;
    }
    ErrorOr<u64> enqueue_reserved(u64 id, ByteBuffer payload, Completion completion)
    {
        auto reservation = m_reserved.take(id);
        if (!reservation.has_value()) return Error::from_string_literal("Userscript event reservation was cancelled");
        m_bytes -= reservation->bytes;
        if (m_closed || !completion || payload.is_empty() || payload.size() > reservation->bytes)
            return Error::from_string_literal("Userscript event exceeds its reservation");
        NonnullRefPtr owner { *this };
        auto timer = Core::Timer::create_single_shot(m_timeout, [weak = make_weak_ptr(), id] {
            if (!weak) return;
            NonnullRefPtr owner { *weak };
            owner->finish(id, Error::from_string_literal("Userscript event delivery timed out"));
        });
        m_bytes += payload.size();
        m_pending.set(id, adopt_ref(*new Pending { reservation->page, reservation->document, move(payload), move(completion), timer }));
        m_queue.append(id);
        timer->start();
        pump();
        return id;
    }
    struct Pending : RefCounted<Pending> {
        AK_ALLOC_WITH_KMALLOC;
        Pending(Web::PageId page, Web::HTML::CrossProcessId document, ByteBuffer payload, Completion completion, NonnullRefPtr<Core::Timer> timer)
            : page(page), document(document), payload(move(payload)), completion(move(completion)), timer(move(timer)) { }
        Web::PageId page;
        Web::HTML::CrossProcessId document;
        ByteBuffer payload;
        Completion completion;
        NonnullRefPtr<Core::Timer> timer;
        bool sent { false };
    };
    DerpBirdValueEvents(Send send, AK::Duration timeout) : m_send(move(send)), m_timeout(timeout) { }
    void pump()
    {
        if (m_pumping || m_closed) return;
        m_pumping = true;
        ScopeGuard reset = [&] { m_pumping = false; };
        while (!m_closed && m_inflight < max_inflight && !m_queue.is_empty()) {
            auto id = m_queue.take_first();
            auto found = m_pending.find(id);
            if (found == m_pending.end()) continue;
            // A synchronous ack may remove the map entry. Keep the borrowed
            // payload valid until the send callback itself returns.
            auto pending = found->value;
            pending->sent = true;
            ++m_inflight;
            auto result = m_send(pending->page, pending->document, id, pending->payload.bytes());
            if (result.is_error()) finish(id, result.release_error());
        }
    }
    void finish(u64 id, ErrorOr<void> result)
    {
        auto pending = m_pending.take(id);
        if (!pending.has_value()) return;
        (*pending)->timer->stop();
        m_bytes -= (*pending)->payload.size();
        if ((*pending)->sent) --m_inflight;
        else m_queue.remove_all_matching([&](u64 queued) { return queued == id; });
        auto completion = move((*pending)->completion);
        completion(move(result));
        pump();
    }
    Send m_send;
    AK::Duration m_timeout;
    HashMap<u64, NonnullRefPtr<Pending>> m_pending;
    HashMap<u64, Reserved> m_reserved;
    Vector<u64> m_queue;
    u64 m_next_id { 1 };
    size_t m_bytes { 0 };
    size_t m_inflight { 0 };
    bool m_closed { false };
    bool m_pumping { false };
};

}
