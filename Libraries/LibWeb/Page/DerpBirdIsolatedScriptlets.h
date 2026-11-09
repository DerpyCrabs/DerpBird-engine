#pragma once

#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Page/DerpBirdScriptWorld.h>
#include <LibWeb/Page/DerpBirdProceduralStyles.h>

namespace Web {

// One isolated blocker world; its native callbacks only add document-local
// user-origin CSS and inspect current filtering policy. No userscript grants.
class DerpBirdIsolatedScriptlets : public RefCounted<DerpBirdIsolatedScriptlets>, public Weakable<DerpBirdIsolatedScriptlets> {
    AK_MAKE_NONCOPYABLE(DerpBirdIsolatedScriptlets);
    AK_MAKE_NONMOVABLE(DerpBirdIsolatedScriptlets);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullRefPtr<DerpBirdIsolatedScriptlets>> create(DOM::Document& document)
    {
        auto& settings = Bindings::principal_host_defined_environment_settings_object(document.window()->principal_realm());
        if (HTML::can_run_script(settings) != HTML::RunScriptDecision::Run)
            return Error::from_string_literal("Page scripting is disabled");
        auto world = TRY(DerpBirdScriptWorld::create(document));
        auto owner = adopt_ref(*new DerpBirdIsolatedScriptlets(document, move(world)));
        owner->m_observer->set_document_became_inactive([weak = owner->make_weak_ptr()] {
            if (weak) {
                NonnullRefPtr keep_alive { *weak };
                keep_alive->close();
            }
        });
        auto& realm = owner->m_world->realm();
        auto add_css = JS::NativeFunction::create(realm, [weak = owner->make_weak_ptr()](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (!weak || weak->m_closed || !vm.argument(0).is_string())
                return JS::Value(false);
            NonnullRefPtr keep_alive { *weak };
            auto source = TRY(vm.argument(0).to_utf16_string(vm));
            return JS::Value(!add_derpbird_procedural_style(*keep_alive->m_document, move(source)).is_error());
        }, 1, "__derpbirdAddCSS"_utf16_fly_string, GC::Ptr<JS::Realm> { &realm });
        realm.global_object().define_direct_property("__derpbirdAddCSS"_utf16_fly_string, add_css, JS::default_attributes);
        auto enabled = JS::NativeFunction::create(realm, [weak = owner->make_weak_ptr()](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
            return JS::Value(weak && !weak->m_closed && weak->m_document->is_fully_active() && weak->m_document->content_blocking_enabled());
        }, 0, "__derpbirdFilterEnabled"_utf16_fly_string, GC::Ptr<JS::Realm> { &realm });
        realm.global_object().define_direct_property("__derpbirdFilterEnabled"_utf16_fly_string, enabled, JS::default_attributes);
        return owner;
    }

    ~DerpBirdIsolatedScriptlets() { close(); }

    ErrorOr<bool> run(Utf16View source)
    {
        if (m_closed || m_ran)
            return Error::from_string_literal("Isolated scriptlets are closed or already ran");
        NonnullRefPtr keep_alive { *this };
        m_ran = true;
        m_running = true;
        ScopeGuard finish = [&] {
            m_running = false;
            if (m_closed)
                m_world = nullptr;
        };
        auto result = TRY(m_world->run(source));
        return !result.is_abrupt();
    }

    void close()
    {
        if (m_closed)
            return;
        m_closed = true;
        m_observer->set_document_became_inactive(nullptr);
        m_observer = {};
        m_world->close();
        clear_derpbird_procedural_styles(*m_document);
        m_document = {};
        if (!m_running)
            m_world = nullptr;
    }

private:
    DerpBirdIsolatedScriptlets(DOM::Document& document, NonnullOwnPtr<DerpBirdScriptWorld> world)
        : m_world(move(world))
        , m_document(document)
        , m_observer(DOM::DocumentObserver::create(document))
    {
    }

    OwnPtr<DerpBirdScriptWorld> m_world;
    GC::Root<DOM::Document> m_document;
    GC::Root<DOM::DocumentObserver> m_observer;
    bool m_closed { false };
    bool m_ran { false };
    bool m_running { false };
};

}
