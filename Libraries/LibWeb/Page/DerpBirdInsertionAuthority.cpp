#include <LibWeb/Page/DerpBirdInsertionAuthority.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>

namespace Web {

// One registry in LibWeb, shared by its embedding executables. It owns no
// authorities; each destructor removes its entry. Identities are never reused.
static auto& authorities()
{
    static NeverDestroyed<HashMap<u64, DerpBirdInsertionAuthority*>> entries;
    return *entries;
}

DerpBirdInsertionAuthority::DerpBirdInsertionAuthority(Function<bool()> active)
    : m_active(move(active))
{
    static u64 next_identity = 0;
    VERIFY(next_identity < NumericLimits<u64>::max());
    m_identity = ++next_identity;
    authorities().set(m_identity, this);
}

NonnullRefPtr<DerpBirdInsertionAuthority> DerpBirdInsertionAuthority::create(Function<bool()> active)
{
    return adopt_ref(*new DerpBirdInsertionAuthority(move(active)));
}

DerpBirdInsertionAuthority::~DerpBirdInsertionAuthority()
{
    authorities().remove(m_identity);
}

RefPtr<DerpBirdInsertionAuthority> DerpBirdInsertionAuthority::resolve(u64 identity)
{
    if (!identity) return nullptr;
    if (auto entry = authorities().get(identity); entry.has_value())
        return RefPtr { *entry };
    // An expired nonzero identity is revoked, not an ordinary page request.
    // Retain no document and never turn it back into an unprivileged fallback.
    static NeverDestroyed<NonnullRefPtr<DerpBirdInsertionAuthority>> revoked { create([] { return false; }) };
    return *revoked;
}

}
