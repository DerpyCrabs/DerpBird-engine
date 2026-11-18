/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/Page/DerpBirdScriptWorld.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace {

class TestPageClient final : public Web::PageClient {
    GC_CELL(TestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(TestPageClient);

public:
    TestPageClient()
        : m_palette(Gfx::PaletteImpl::create_with_anonymous_buffer(MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)))))
    {
    }
    virtual Web::PageId id() const override { return Web::PageId { 1 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { return m_palette; }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1; }
    virtual double device_pixel_ratio() const override { return 1; }
    virtual double device_pixels_per_css_pixel() const override { return 1; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(Web::PageId, u64, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { 1, m_next_cross_process_id++ }; }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    Gfx::Palette m_palette;
    GC::Ptr<Web::Page> m_page;
    u64 m_next_cross_process_id { 1 };
};

GC_DEFINE_ALLOCATOR(TestPageClient);

}



TEST_CASE(derpbird_function_timers_keep_receiver_in_script_world)
{
    Core::EventLoop event_loop;
    Web::Platform::FontPlugin font_plugin(false);
    Web::Platform::FontPlugin::install(font_plugin);
    auto principal = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal.ptr());
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    auto& main_realm = document->window()->principal_realm();
    vm.pop_execution_context();
    auto page_run = [&](Utf16View source) {
        auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(main_realm);
        vm.push_execution_context(settings.realm_execution_context());
        ScopeGuard pop_context = [&] { vm.pop_execution_context(); };
        auto script = JS::Script::parse(source, main_realm);
        VERIFY(!script.is_error());
        auto result = vm.run(script.value());
        VERIFY(!result.is_throw_completion());
        return result.release_value();
    };
    page_run("globalThis.pageOnly = 'private-page-value';"_utf16);
    auto world = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    auto run = [&](Utf16View source) {
        auto result = MUST(world->run(source));
        VERIFY(!result.is_abrupt());
        return result.release_value();
    };
    run(R"JS(
        globalThis.idleDone = false;
        globalThis.idleIsolated = false;
        requestIdleCallback(deadline => {
            idleIsolated = Object.getPrototypeOf(deadline) === IdleDeadline.prototype
                && typeof deadline.timeRemaining() === 'number';
            idleDone = true;
        }, { timeout: 1 });
        globalThis.timerDone = false;
        globalThis.timerIsolated = false;
        globalThis.intervalCount = 0;
        const argument = { marker: 'script argument' };
        setTimeout(function(delivered) {
            timerIsolated = this === window && typeof this.pageOnly === 'undefined'
                && delivered === argument;
            timerDone = true;
        }, 0, argument);
        globalThis.interval = setInterval(function() {
            intervalCount++;
            globalThis.intervalIsolated = this === window && typeof this.pageOnly === 'undefined';
            clearInterval(interval);
        }, 0);
    )JS"_utf16);
    bool expired = false;
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] { expired = true; });
    deadline->start();
    event_loop.spin_until([&] { return expired || run("timerDone && intervalCount > 0 && idleDone"_utf16).as_bool(); });
    deadline->stop();
    VERIFY(!expired);
    EXPECT(run("timerIsolated"_utf16).as_bool());
    EXPECT(run("idleIsolated"_utf16).as_bool());
    EXPECT(run("intervalIsolated"_utf16).as_bool());
    EXPECT(page_run("typeof timerDone === 'undefined' && typeof intervalCount === 'undefined'"_utf16).as_bool());
    run(R"JS(
        globalThis.stringCalls = 0;
        globalThis.stringTimeoutValue = false;
        setTimeout("globalThis.stringTimeoutValue = typeof pageOnly === 'undefined'; document.documentElement.setAttribute('data-string-timeout', 'ran')", 0);
        globalThis.stringIntervalId = setInterval("globalThis.stringCalls = (globalThis.stringCalls || 0) + 1; document.documentElement.setAttribute('data-string-interval', String(globalThis.stringCalls))", 0);
    )JS"_utf16);
    expired = false;
    deadline->start();
    event_loop.spin_until([&] {
        return expired || page_run("document.documentElement.hasAttribute('data-string-timeout') && Number(document.documentElement.getAttribute('data-string-interval')) >= 2"_utf16).as_bool();
    });
    deadline->stop();
    run("clearInterval(stringIntervalId)"_utf16);
    VERIFY(!expired);
    EXPECT(run("stringTimeoutValue && stringCalls >= 2"_utf16).as_bool());
    EXPECT(page_run("typeof stringCalls === 'undefined' && typeof stringTimeoutValue === 'undefined'"_utf16).as_bool());
    // Explicit policy denial makes real request/error delivery deterministic,
    // independent of initial about:blank trust and page visibility.
    auto geo_policy = document->permissions_policy();
    geo_policy.inherited_policy[to_underlying(Web::PermissionsPolicy::PolicyControlledFeature::Geolocation)] = Web::PermissionsPolicy::FeatureValue::Disabled;
    document->set_permissions_policy(move(geo_policy));
    auto geo_callback_result = MUST(world->run(R"JS(
        globalThis.geoDone = false;
        globalThis.geoIsolated = false;
        globalThis.geoCallback = error => {
            geoIsolated = Object.getPrototypeOf(error) === GeolocationPositionError.prototype
                && error.code === GeolocationPositionError.PERMISSION_DENIED;
            geoDone = true;
            document.documentElement.setAttribute('data-geo-callback', 'ran');
        };
        geoCallback;
    )JS"_utf16));
    VERIFY(!geo_callback_result.is_abrupt());
    run("navigator.geolocation.getCurrentPosition(geoCallback, geoCallback)"_utf16);
    expired = false;
    deadline->start();
    event_loop.spin_until([&] { return expired || run("geoDone"_utf16).as_bool(); });
    deadline->stop();
    VERIFY(!expired);
    EXPECT(run("geoIsolated"_utf16).as_bool());
    run("document.documentElement.removeAttribute('data-geo-callback')"_utf16);
    run("navigator.geolocation.getCurrentPosition(geoCallback, geoCallback)"_utf16);
    // Queue callbacks before revocation, then let real task delivery run. A
    // surviving world and the page must continue receiving their own timers.
    run(R"JS(
        setTimeout("document.documentElement.setAttribute('data-closed-string-timeout', 'ran')", 0);
        setInterval("document.documentElement.setAttribute('data-closed-string-interval', 'ran')", 0);
        requestIdleCallback(() => document.documentElement.setAttribute('data-closed-idle', 'ran'), { timeout: 1 });
        setTimeout(() => document.documentElement.setAttribute('data-closed-timeout', 'ran'), 0);
        setInterval(() => document.documentElement.setAttribute('data-closed-interval', 'ran'), 0);
    )JS"_utf16);
    auto surviving_world = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    EXPECT(!MUST(surviving_world->run(R"JS(
        setTimeout(() => document.documentElement.setAttribute('data-surviving-timeout', 'ran'), 0);
    )JS"_utf16)).is_abrupt());
    page_run(R"JS(
        globalThis.pageTimerDone = false;
        setTimeout(function() { pageTimerDone = this === window; }, 30);
    )JS"_utf16);
    world->close();
    expired = false;
    deadline->start();
    event_loop.spin_until([&] {
        return expired || page_run("pageTimerDone && document.documentElement.hasAttribute('data-surviving-timeout')"_utf16).as_bool();
    });
    deadline->stop();
    VERIFY(!expired);
    EXPECT(page_run("!document.documentElement.hasAttribute('data-closed-timeout')"_utf16).as_bool());
    EXPECT(page_run("!document.documentElement.hasAttribute('data-closed-interval')"_utf16).as_bool());
    EXPECT(page_run("!document.documentElement.hasAttribute('data-closed-idle')"_utf16).as_bool());
    EXPECT(page_run("!document.documentElement.hasAttribute('data-closed-string-timeout') && !document.documentElement.hasAttribute('data-closed-string-interval')"_utf16).as_bool());
    EXPECT(page_run("!document.documentElement.hasAttribute('data-geo-callback')"_utf16).as_bool());
    surviving_world->close();
}
