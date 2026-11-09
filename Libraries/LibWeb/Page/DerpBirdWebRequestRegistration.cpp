#include <LibWeb/Page/DerpBirdWebRequestRegistration.h>

namespace Web {

// Both WebContent's IPC handlers and LibWeb's request loader use these tables.
// Keep storage out of inline headers: hidden inline symbols otherwise create
// a separate table in each shared object.
Vector<DerpBirdWebRequestRegistration::Mirror>& DerpBirdWebRequestRegistration::mirrors()
{
    static NeverDestroyed<Vector<Mirror>> values;
    return *values;
}

HashMap<HTML::CrossProcessId, HashMap<String, WeakPtr<DerpBirdWebRequestRegistration>>>& DerpBirdWebRequestRegistration::local_index()
{
    static NeverDestroyed<HashMap<HTML::CrossProcessId, HashMap<String, WeakPtr<DerpBirdWebRequestRegistration>>>> values;
    return *values;
}

Vector<WeakPtr<DerpBirdWebRequestRegistration>>& DerpBirdWebRequestRegistration::registrations()
{
    static NeverDestroyed<Vector<WeakPtr<DerpBirdWebRequestRegistration>>> values;
    return *values;
}

}
