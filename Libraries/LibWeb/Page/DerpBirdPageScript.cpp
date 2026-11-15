#include <LibWeb/Page/DerpBirdPageScript.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web {

ErrorOr<bool> run_derpbird_page_script(DOM::Document& document, StringView id, Utf16View source)
{
    if (!document.is_fully_active())
        return Error::from_string_literal("Userscript document is inactive");
    auto& settings = Bindings::principal_host_defined_environment_settings_object(document.window()->principal_realm());
    if (HTML::can_run_script(settings) != HTML::RunScriptDecision::Run)
        return Error::from_string_literal("Page scripting is disabled");
    auto script = HTML::ClassicScript::create(ByteString::formatted("derpbird-userscript-{}.js", id), Utf16String::from_utf16(source), settings, document.url());
    return !script->run(HTML::ClassicScript::RethrowErrors::Yes).is_abrupt();
}

}
