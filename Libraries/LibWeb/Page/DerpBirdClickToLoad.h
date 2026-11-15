#pragma once
#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web {
inline void install_derpbird_click_to_load(DOM::Document& document)
{
    document.derpbird_click_to_load = true;
    auto& realm = document.relevant_settings_object().realm();
    auto listener = JS::NativeFunction::create(realm, GC::create_function(GC::Heap::the(), GC::captures(document), [](auto& document, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (!document.derpbird_click_to_load || !document.is_fully_active() || !vm.argument(0).is_object()) return JS::js_undefined();
        auto* event = Bindings::impl_from<DOM::Event>(&vm.argument(0).as_object());
        if (!event || !event->is_trusted()) return JS::js_undefined();
        auto* element = as_if<DOM::Element>(event->target().ptr());
        if (!element || element->id() != "derpbird-click-to-load"_utf16_fly_string) return JS::js_undefined();
        auto navigable = document.navigable();
        auto window = document.window();
        if (!navigable || navigable->is_top_level_traversable() || !window || !window->has_transient_activation()) return JS::js_undefined();
        if (!document.url().scheme().is_one_of("http"sv, "https"sv)) return JS::js_undefined();
        window->consume_user_activation();
        navigable->derpbird_click_to_load_url = document.url();
        navigable->reload({}, HTML::UserNavigationInvolvement::Activation);
        return JS::js_undefined();
    }), 1, Utf16FlyString {}, &realm);
    auto callback = GC::Heap::the().allocate<WebIDL::CallbackType>(*listener, realm);
    document.add_event_listener_without_options("click"_utf16_fly_string, *DOM::IDLEventListener::create(*callback));
}
}
