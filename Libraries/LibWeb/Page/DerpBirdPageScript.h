#pragma once

#include <AK/Error.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web {

// Execute a previously selected @grant none script in the page realm. Supplies
// no native capabilities. The source already has a per-script function scope.
WEB_API ErrorOr<bool> run_derpbird_page_script(DOM::Document&, StringView id, Utf16View source);

}
