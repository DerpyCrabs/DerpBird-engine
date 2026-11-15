#pragma once

#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibWeb/Export.h>
#include <LibGC/CaptureTraits.h>

namespace Web {

// Native-only authority. Rust style workers may copy identity(), never a native
// pointer. Creation, resolution and destruction are on the LibWeb main thread.
// The active callback holds a weak owner, so it cannot prolong the document.
class WEB_API DerpBirdInsertionAuthority final : public RefCounted<DerpBirdInsertionAuthority> {
public:
    GC_HOLDS_NO_CELL_EDGES;
    static NonnullRefPtr<DerpBirdInsertionAuthority> create(Function<bool()> active);
    static RefPtr<DerpBirdInsertionAuthority> resolve(u64 identity);
    ~DerpBirdInsertionAuthority();
    bool active() const { return !m_revoked && m_active(); }
    void revoke() { m_revoked = true; }
    u64 identity() const { return m_identity; }
private:
    explicit DerpBirdInsertionAuthority(Function<bool()>);
    bool m_revoked { false };
    u64 m_identity;
    Function<bool()> m_active;
};

}
