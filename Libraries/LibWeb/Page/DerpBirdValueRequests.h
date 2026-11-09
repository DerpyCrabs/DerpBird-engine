#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <AK/WeakPtr.h>
#include <LibCore/Timer.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>

namespace Web {

// Native request correlation only. Payloads are opaque; this table never grants
// storage access. The host authenticates the document and the script capability.
class DerpBirdValueRequests : public RefCounted<DerpBirdValueRequests>, public Weakable<DerpBirdValueRequests> {
public:
    AK_ALLOC_WITH_KMALLOC;
    using Completion = Function<void(ErrorOr<ByteBuffer>)>;
    static constexpr size_t max_payload_bytes = 16 * 1024 * 1024;
    static NonnullRefPtr<DerpBirdValueRequests> create() { return adopt_ref(*new DerpBirdValueRequests); }

    ErrorOr<u64> add(PageId page, HTML::CrossProcessId document, Completion completion, bool transport_timeout = true)
    {
        if (!completion || m_pending.size() >= 128 || m_next_id == NumericLimits<u64>::max())
            return Error::from_string_literal("Userscript request queue is full or invalid");
        auto id = m_next_id++;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [weak = make_weak_ptr(), id] {
            if (weak) {
                // A completion can release its owner's final reference.
                NonnullRefPtr owner { *weak };
                owner->fail(id);
            }
        });
        m_pending.set(id, { page, document, move(completion), timer });
        if (transport_timeout) timer->start();
        return id;
    }
    void complete(PageId page, HTML::CrossProcessId document, u64 id, ByteBuffer payload, Optional<String> error)
    {
        auto found = m_pending.find(id);
        if (found == m_pending.end() || found->value.page != page || found->value.document != document)
            return;
        auto completion = move(found->value.completion);
        found->value.timer->stop();
        m_pending.remove(found);
        if (error.has_value()) {
            // Error text from the host is not retained as an Error StringView.
            completion(Error::from_string_literal("Userscript storage request failed"));
        } else if (payload.size() > max_payload_bytes) {
            completion(Error::from_string_literal("Userscript storage response exceeds limits"));
        } else {
            completion(move(payload));
        }
    }
    void cancel_document(PageId page, HTML::CrossProcessId document)
    {
        // Inactive realm callbacks must not be invoked, even with an error.
        m_pending.remove_all_matching([&](auto const&, auto const& item) { return item.page == page && item.document == document; });
    }
    void cancel_page(PageId page)
    {
        m_pending.remove_all_matching([&](auto const&, auto const& item) { return item.page == page; });
    }
    size_t pending() const { return m_pending.size(); }

private:
    void fail(u64 id)
    {
        auto entry = m_pending.take(id);
        if (!entry.has_value()) return;
        entry->timer->stop();
        entry->completion(Error::from_string_literal("Userscript storage request timed out"));
    }
    struct Pending { PageId page; HTML::CrossProcessId document; Completion completion; NonnullRefPtr<Core::Timer> timer; };
    HashMap<u64, Pending> m_pending;
    u64 m_next_id { 1 };
};

}
