#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/DerpBirdProceduralStyles.h>

namespace Web {
namespace {
struct DocumentStyles {
    Vector<Utf16String> sources;
    size_t code_units { 0 };
};
auto& styles()
{
    static NeverDestroyed<HashMap<DOM::Document const*, DocumentStyles>> entries;
    return *entries;
}
void invalidate(DOM::Document& document)
{
    document.style_scope().invalidate_user_style_sheet();
    if (document.style_computer_if_created())
        CSS::record_non_author_stylesheets(document);
}
}

ErrorOr<void> add_derpbird_procedural_style(DOM::Document& document, Utf16String source)
{
    if (!document.is_fully_active() || !document.content_blocking_enabled())
        return Error::from_string_literal("Procedural filtering is disabled");
    auto length = source.utf16_view().length_in_code_units();
    if (length > 1024 * 1024)
        return Error::from_string_literal("Procedural stylesheet too large");
    auto& entry = styles().ensure(&document);
    if (entry.sources.contains_slow(source))
        return {};
    if (entry.sources.size() >= 4096 || entry.code_units + length > 4 * 1024 * 1024)
        return Error::from_string_literal("Procedural stylesheet budget exceeded");
    entry.code_units += length;
    entry.sources.append(move(source));
    invalidate(document);
    return {};
}

void clear_derpbird_procedural_styles(DOM::Document& document)
{
    if (styles().remove(&document))
        invalidate(document);
}

Vector<Utf16String> const& derpbird_procedural_styles(DOM::Document const& document)
{
    auto found = styles().find(&document);
    if (found != styles().end())
        return found->value.sources;
    static NeverDestroyed<Vector<Utf16String>> empty;
    return *empty;
}
}
