#pragma once

#include <AK/Error.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web {
// Owned by the document's isolated blocker session; never by page JavaScript.
WEB_API ErrorOr<void> add_derpbird_procedural_style(DOM::Document&, Utf16String);
WEB_API void clear_derpbird_procedural_styles(DOM::Document&);
WEB_API Vector<Utf16String> const& derpbird_procedural_styles(DOM::Document const&);
}
