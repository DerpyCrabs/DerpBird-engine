#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Weakable.h>
#include <LibWeb/Page/DerpBirdInsertionAuthority.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Page/DerpBirdScriptValues.h>
#include <LibWeb/Page/DerpBirdWebRequestRegistration.h>
#include <LibGC/Root.h>
#include <LibJS/Script.h>
#include <LibJS/Runtime/GlobalEnvironment.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Module.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/HTML/WindowProxy.h>

namespace Web {

// A native-owned execution world for one script in one document. This class
// installs privileged APIs only through a native capability-checked transport.
// Document inactivity revokes the world; owners also close it on script removal.
class DerpBirdScriptWorld : public Weakable<DerpBirdScriptWorld> {
    AK_MAKE_NONCOPYABLE(DerpBirdScriptWorld);
    AK_MAKE_NONMOVABLE(DerpBirdScriptWorld);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<DerpBirdScriptWorld>> create(DOM::Document& document)
    {
        auto context = document.browsing_context();
        if (!context || !document.is_fully_active())
            return Error::from_string_literal("Userscript document is not active");
        auto window = document.window();
        auto& vm = window->principal_realm().vm();
        GC::Root<Bindings::WrapperWorld> world = vm.heap().allocate<Bindings::WrapperWorld>(Bindings::WrapperWorld::Type::Extension);
        auto execution = Bindings::create_a_new_javascript_realm(
            vm, HTML::CrossOriginIsolationMode::None,
            [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
                return Bindings::create_global_object_wrapper(realm, *window);
            },
            [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
                return context->window_proxy_for(*world, realm)->object();
            });
        auto& realm = *execution->realm();
        auto intrinsics = realm.create<Bindings::Intrinsics>(realm);
        realm.set_host_defined(vm.heap().allocate<Bindings::HostDefined>(intrinsics, *world, window->principal_realm()));
        Bindings::cache_global_object_wrapper(realm);
        auto owner = adopt_own(*new DerpBirdScriptWorld(document, realm, move(execution)));
        if (Bindings::initialize_window_web_interfaces(*window, realm).is_error())
            return Error::from_string_literal("Unable to initialize userscript interfaces");
        auto is_active = JS::NativeFunction::create(realm,
            GC::create_function(GC::Heap::the(), GC::captures(world, GC::Root { &document }), [](auto& world, auto& document, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
                return JS::Value { !world->is_detached() && document->is_fully_active() };
            }), 0, "__derpbirdIsActive"_utf16_fly_string, GC::Ptr<JS::Realm> { &realm });
        realm.global_object().define_direct_property("__derpbirdIsActive"_utf16_fly_string, is_active, JS::default_attributes);
        return owner;
    }

    ~DerpBirdScriptWorld() { close(); }

    JS::Realm& realm() { return *m_realm; }

    void close()
    {
        if (!m_closed) {
            m_closed = true;
            if (m_values) m_values->close();
            m_observer->set_observes_derpbird_url_changes(false);
            m_observer->set_document_became_inactive(nullptr);
            m_observer = {};
            Bindings::host_defined_wrapper_world(*m_realm).detach();
        }
    }

    // Explicit unsafeWindow grant: expose the page's WindowProxy, not this
    // extension world's DOM wrapper. GM capabilities stay in the isolated realm.
    void expose_unsafe_window()
    {
        VERIFY(!m_closed && m_document->is_fully_active());
        auto& principal = m_document->window()->principal_realm();
        m_realm->global_object().define_direct_property("unsafeWindow"_utf16_fly_string,
            &principal.global_environment().global_this_value(), JS::Attribute::Enumerable);
    }

    ErrorOr<void> expose_url_changes(bool raw = false)
    {
        auto setup = uR"JS((() => {
            if (Object.getOwnPropertyDescriptor(window, "onurlchange")) return;
            let handler = null;
            const add = window.addEventListener.bind(window);
            const remove = window.removeEventListener.bind(window);
            Object.defineProperty(window, 'onurlchange', {
                configurable: true, enumerable: true,
                get() { return handler; },
                set(value) {
                    if (handler) remove('urlchange', handler);
                    handler = typeof value === 'function' ? value : null;
                    if (handler) add('urlchange', handler);
                }
            });
        })())JS"sv;
        auto result = TRY(raw ? run_raw(setup) : run(setup));
        if (result.is_abrupt()) return Error::from_string_literal("Cannot install URL change events");
        m_observer->set_observes_derpbird_url_changes(true);
        return {};
    }

    void install_values(String script_id, Vector<String> const& grants, DerpBirdScriptValues::Send send, bool raw = false)
    {
        VERIFY(!m_values && !m_closed);
        auto world = GC::Ref { Bindings::host_defined_wrapper_world(*m_realm) };
        auto& api_realm = raw ? m_document->window()->principal_realm() : *m_realm;
        m_values = DerpBirdScriptValues::create(api_realm, move(script_id), move(send),
            [world = GC::Root { world }, document = m_document] {
                return !world->is_detached() && document->is_fully_active();
            });
        auto gm = JS::Object::create(api_realm, api_realm.intrinsics().object_prototype());
        m_values->install(*gm, grants);
        m_values->install_legacy(grants, &m_realm->global_object());
        m_realm->global_object().define_direct_property("GM"_utf16_fly_string, gm, JS::default_attributes);
        m_observer->set_document_became_inactive(GC::create_function(GC::Heap::the(), GC::captures(world, m_values->make_weak_ptr()), [](auto& world, auto& values) {
            if (values) values->close();
            world->detach();
        }));
    }

    void install_element_insertion(Vector<String> const& grants, bool raw)
    {
        bool elements = grants.contains_slow("GM_addElement"sv) || grants.contains_slow("GM.addElement"sv);
        bool styles = grants.contains_slow("GM_addStyle"sv) || grants.contains_slow("GM.addStyle"sv);
        if (!elements && !styles) return;
        auto world = GC::Root { &Bindings::host_defined_wrapper_world(*m_realm) };
        auto& realm = raw ? m_document->window()->principal_realm() : *m_realm;
        auto authority = DerpBirdInsertionAuthority::create([owner = make_weak_ptr()] {
            return owner && !owner->m_closed && owner->m_document->is_fully_active()
                && !Bindings::host_defined_wrapper_world(*owner->m_realm).is_detached();
        });
        // GM insertion supplies trusted extension input without invoking the
        // page's Trusted Types default policy. Accept only primitive strings:
        // conversions/getters must run before entering the native operation.
        auto configure = JS::NativeFunction::create(realm,
            GC::create_function(GC::Heap::the(), GC::captures(world, m_document, elements, styles, authority), [](auto& world, auto& document, auto& elements, auto& styles, auto& authority, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (world->is_detached() || !authority->active())
                    return vm.throw_completion<JS::TypeError>("Userscript was revoked"_utf16);
                if (!vm.argument(0).is_object() || !vm.argument(1).is_string() || !vm.argument(2).is_string())
                    return vm.throw_completion<JS::TypeError>("Invalid userscript element configuration"_utf16);
                auto* element = Bindings::impl_from<DOM::Element>(&vm.argument(0).as_object());
                if (!element || &element->document() != document.ptr() || element->parent() || element->is_connected()
                    || (!elements && !(styles && element->namespace_uri() == Namespace::HTML && element->local_name() == u"style"sv)))
                    return vm.throw_completion<JS::TypeError>("Userscript configuration is outside its grant or document"_utf16);
                auto const& name = vm.argument(1).as_string().utf16_string();
                auto const& value = vm.argument(2).as_string().utf16_string();
                if (name == u"textContent"sv) {
                    if (auto* script = as_if<HTML::HTMLScriptElement>(element))
                        script->set_string_text(value.utf16_view());
                    element->string_replace_all(value);
                } else {
                    if (!DOM::is_valid_attribute_local_name(name.utf16_view()))
                        return vm.throw_completion<JS::TypeError>("Invalid userscript attribute name"_utf16);
                    auto qualified_name = Utf16FlyString::from_utf16(name.utf16_view());
                    if (element->namespace_uri() == Namespace::HTML && document->document_type() == DOM::Document::Type::HTML)
                        qualified_name = qualified_name.to_ascii_lowercase();
                    element->set_attribute(FlyString { qualified_name.to_utf16_string().to_utf8() }, value);
                }
                return vm.argument(0);
            }), 3, "__derpbirdConfigureElement"_utf16_fly_string, GC::Ptr<JS::Realm> { &realm });
        m_realm->global_object().define_direct_property("__derpbirdConfigureElement"_utf16_fly_string, configure, JS::default_attributes);
        auto function = JS::NativeFunction::create(realm,
            GC::create_function(GC::Heap::the(), GC::captures(world, m_document, elements, styles, authority), [](auto& world, auto& document, auto& elements, auto& styles, auto& authority, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (world->is_detached() || !authority->active())
                    return vm.throw_completion<JS::TypeError>("Userscript was revoked"_utf16);
                if (!vm.argument(0).is_object() || !vm.argument(1).is_object())
                    return vm.throw_completion<JS::TypeError>("Invalid userscript insertion"_utf16);
                auto* element = Bindings::impl_from<DOM::Element>(&vm.argument(0).as_object());
                auto* parent = Bindings::impl_from<DOM::Node>(&vm.argument(1).as_object());
                if (!element || !parent || &element->document() != document.ptr() || &parent->document() != document.ptr()
                    || element->parent() || element->is_connected()
                    || (!elements && !(styles && element->namespace_uri() == Namespace::HTML && element->local_name() == u"style"sv)))
                    return vm.throw_completion<JS::TypeError>("Userscript insertion is outside its grant or document"_utf16);
                auto previous = element->derpbird_userscript_insertion_active();
                auto previous_authority = element->derpbird_insertion_authority();
                auto insertion_authority = DerpBirdInsertionAuthority::create([authority] { return authority->active(); });
                element->set_derpbird_insertion_authority(insertion_authority);
                element->set_derpbird_userscript_insertion_active(true);
                bool pending_connection = false;
                ScopeGuard restore = [&] {
                    if (pending_connection) return;
                    element->set_derpbird_userscript_insertion_active(previous);
                    element->set_derpbird_insertion_authority(move(previous_authority));
                };
                auto result = parent->append_child(*element);
                if (result.is_exception())
                    return vm.throw_completion<JS::TypeError>("Unable to insert userscript element"_utf16);
                // Setting src before insertion queued an unprivileged image
                // update. Replace that generation while this operation's
                // authority is available; its microtask snapshots the token.
                if (auto* image = as_if<HTML::HTMLImageElement>(element))
                    image->update_the_image_data();
                if (!element->is_connected() && element->parent() == parent) {
                    element->set_derpbird_insertion_pending(true);
                    pending_connection = true;
                }
                return vm.argument(0);
            }), 2, "__derpbirdInsertElement"_utf16_fly_string, GC::Ptr<JS::Realm> { &realm });
        m_realm->global_object().define_direct_property("__derpbirdInsertElement"_utf16_fly_string, function, JS::default_attributes);
    }

    bool receive_value_event(DerpBirdValueProtocol::Event const& event)
    {
        return !m_closed && m_values && m_values->receive_event(event);
    }

    void install_web_requests(DerpBirdWebRequestRegistration& registration, bool raw)
    {
        auto& realm = raw ? m_document->window()->principal_realm() : *m_realm;
        auto function = JS::NativeFunction::create(realm,
            GC::create_function(GC::Heap::the(), GC::captures(registration.make_weak_ptr(), GC::Root { &realm }), [](auto& owner, auto& realm, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (!owner || !owner->active()) return vm.throw_completion<JS::TypeError>("webRequest registration revoked"_utf16);
                if (!vm.argument(0).is_string() || (!vm.argument(1).is_undefined() && !vm.argument(1).is_function()))
                    return vm.throw_completion<JS::TypeError>("Invalid webRequest registration"_utf16);
                auto text = vm.argument(0).as_string().utf16_string().to_utf8();
                if (text.bytes().size() > 65536) return vm.throw_completion<JS::TypeError>("webRequest registration exceeds quota"_utf16);
                auto rules = JsonValue::from_string(text);
                if (rules.is_error() || owner->replace(rules.value()).is_error())
                    return vm.throw_completion<JS::TypeError>("Invalid webRequest rules"_utf16);
                owner->on_event = nullptr;
                if (vm.argument(1).is_function()) {
                    auto revision = owner->revision;
                    owner->on_event = [owner, realm, revision, callback = GC::Root { &vm.argument(1).as_object() }](JsonObject event) {
                        if (!owner || !owner->active() || owner->pending_events >= 32) return;
                        ++owner->pending_events;
                        auto text = event.serialized();
                        HTML::queue_global_task(HTML::Task::Source::Unspecified, realm->global_object(),
                            GC::create_function(GC::Heap::the(), GC::captures(owner, realm, revision, callback, move(text)), [](auto& owner, auto& realm, auto& revision, auto& callback, auto& text) {
                                if (!owner) return;
                                --owner->pending_events;
                                if (!owner->active() || owner->revision != revision) return;
                                HTML::TemporaryExecutionContext context(*realm);
                                auto& vm = realm->vm();
                                auto result = JS::call(vm, JS::Value(callback.ptr()), JS::js_undefined(), JS::PrimitiveString::create(vm, Utf16String::from_utf8(text)));
                                if (result.is_error()) HTML::report_exception(result, *realm);
                            }));
                    };
                }
                return JS::js_undefined();
            }), 2, "__derpbirdWebRequest"_utf16_fly_string, GC::Ptr<JS::Realm> { &realm });
        m_realm->global_object().define_direct_property("__derpbirdWebRequest"_utf16_fly_string, function, JS::default_attributes);
    }

    ErrorOr<JS::Completion> run_raw(Utf16View source)
    {
        if (m_closed || Bindings::host_defined_wrapper_world(*m_realm).is_detached() || !m_document->is_fully_active())
            return Error::from_string_literal("Userscript world was revoked");
        auto& principal = m_document->window()->principal_realm();
        auto& settings = Bindings::principal_host_defined_environment_settings_object(principal);
        if (HTML::can_run_script(settings) != HTML::RunScriptDecision::Run)
            return Error::from_string_literal("Page scripting is disabled");
        HTML::TemporaryExecutionContext context(principal);
        auto& vm = principal.vm();
        Utf16StringBuilder factory;
        factory.append_ascii("(function(__derpbirdGM"sv);
        GC::RootVector<JS::Value> arguments;
        auto gm = m_realm->global_object().get("GM"_utf16_fly_string);
        if (gm.is_error()) return gm.release_error();
        auto gm_value = gm.release_value();
        if (gm_value.is_undefined()) gm_value = JS::Object::create(principal, principal.intrinsics().object_prototype());
        arguments.append(gm_value);
        for (auto name : { "GM_getValue"sv, "GM_setValue"sv, "GM_deleteValue"sv, "GM_listValues"sv,
                 "GM_getValues"sv, "GM_setValues"sv, "GM_deleteValues"sv, "GM_addValueChangeListener"sv,
                 "GM_removeValueChangeListener"sv, "__derpbirdNetworkSync"sv, "__derpbirdWebRequest"sv, "__derpbirdIsActive"sv, "__derpbirdInsertElement"sv, "__derpbirdConfigureElement"sv, "unsafeWindow"sv }) {
            factory.append_ascii(","sv);
            auto parameter_name = Utf16String::from_utf8(name);
            factory.append(parameter_name.utf16_view());
            auto value = m_realm->global_object().get(parameter_name);
            if (value.is_error()) return value.release_error();
            arguments.append(value.release_value());
        }
        factory.append_ascii("){return "sv);
        factory.append(source);
        factory.append_ascii("\n})"sv);
        auto script = JS::Script::parse(factory.view(), principal);
        if (script.is_error()) {
            for (auto const& error : script.error())
                warnln("Raw userscript syntax error: {}", error.to_utf16_string().to_utf8());
            return Error::from_string_literal("Invalid raw userscript");
        }
        auto callable = vm.run(script.value());
        if (callable.is_error()) return callable.release_error();
        auto result = JS::call(vm, callable.release_value(), &principal.global_environment().global_this_value(), arguments.span());
        if (result.is_error()) return result.release_error();
        return JS::Completion { result.release_value() };
    }

    ErrorOr<JS::Completion> run(Utf16View source)
    {
        if (m_closed || Bindings::host_defined_wrapper_world(*m_realm).is_detached() || !m_document->is_fully_active())
            return Error::from_string_literal("Userscript world is closed or inactive");
        auto script = JS::Script::parse(source, *m_realm);
        if (script.is_error()) {
            for (auto const& error : script.error())
                warnln("Isolated userscript syntax error: {}", error.to_utf16_string().to_utf8());
            return Error::from_string_literal("Unable to parse userscript");
        }
        auto& vm = m_realm->vm();
        vm.push_execution_context(*m_execution);
        ScopeGuard pop_context = [&] { vm.pop_execution_context(); };
        return vm.run(script.value());
    }

private:
    DerpBirdScriptWorld(DOM::Document& document, JS::Realm& realm, NonnullOwnPtr<JS::ExecutionContext> execution)
        : m_document(document)
        , m_realm(realm)
        , m_execution(move(execution))
        , m_observer(DOM::DocumentObserver::create(document))
    {
        // The callback can outlive this native owner; never capture its address.
        m_observer->set_document_became_inactive(GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { Bindings::host_defined_wrapper_world(realm) }), [](auto& world) {
            world->detach();
        }));
    }

    GC::Root<DOM::Document> m_document;
    GC::Root<JS::Realm> m_realm;
    NonnullOwnPtr<JS::ExecutionContext> m_execution;
    GC::Root<DOM::DocumentObserver> m_observer;
    RefPtr<DerpBirdScriptValues> m_values;
    bool m_closed { false };
};

}
