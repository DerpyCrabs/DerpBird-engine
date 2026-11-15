/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/BindingsGlue.h>
#include <LibWeb/CSS/ImageStyleValueResourceKey.h>
#include <AK/HashMap.h>
#include <LibWeb/DOM/TreeWalker.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/Scripting/ModuleMap.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Page/DerpBirdIsolatedScriptlets.h>
#include <LibWeb/ResizeObserver/ResizeObserver.h>
#include <LibWeb/IntersectionObserver/IntersectionObserver.h>
#include <LibWeb/Page/DerpBirdScriptWorld.h>
#include <LibWeb/Page/DerpBirdCredentialFields.h>
#include <LibWeb/Page/DerpBirdCredentialSession.h>
#include <LibWeb/Page/DerpBirdDocumentScripts.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace {

void install_test_fonts()
{
    static NeverDestroyed<Web::Platform::FontPlugin> plugin(false);
    static bool installed = [] { Web::Platform::FontPlugin::install(*plugin); return true; }();
    (void)installed;
}

class TestPageClient final : public Web::PageClient {
    GC_CELL(TestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(TestPageClient);

public:
    virtual Web::PageId id() const override { return Web::PageId { 1 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override
    {
        // Same minimal system palette as BenchmarkDisplayListRecording: form
        // submission tests exercise actual layout/checkVisibility.
        static auto palette = [] {
            auto buffer = MUST(Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme)));
            auto* theme = buffer.data<Gfx::SystemTheme>();
            theme->color[to_underlying(Gfx::ColorRole::Window)] = Color(Color::White).value();
            theme->color[to_underlying(Gfx::ColorRole::WindowText)] = Color(Color::Black).value();
            theme->color[to_underlying(Gfx::ColorRole::Base)] = Color(Color::White).value();
            theme->color[to_underlying(Gfx::ColorRole::BaseText)] = Color(Color::Black).value();
            return Gfx::PaletteImpl::create_with_anonymous_buffer(move(buffer));
        }();
        return Gfx::Palette(*palette);
    }
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
    virtual bool wants_derpbird_credential_submissions() const override { return m_observe_submissions; }
    virtual void derpbird_credential_submitted(Web::DOM::Document& document, String const& username, String const& password) override
    {
        m_submissions.append({ document.id(), document.origin().serialize(), username, password });
    }
    struct Submission {
        Web::HTML::CrossProcessId document;
        String origin;
        String username;
        String password;
    };
    bool m_observe_submissions { false };
    Vector<Submission> m_submissions;
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    u64 m_next_cross_process_id { 1 };
};

GC_DEFINE_ALLOCATOR(TestPageClient);

}


TEST_CASE(derpbird_script_world_isolates_globals_prototypes_and_dom_expandos)
{
    install_test_fonts();
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
    page_run(R"JS(
        globalThis.pageOnly = 123;
        globalThis.GM_getValue = () => 'page forgery';
        Object.prototype.pagePoison = true;
        const probe = document.createElement('div');
        probe.id = 'probe';
        probe.pageSecret = 456;
        (document.body || document.documentElement || document).appendChild(probe);
        Document.prototype.querySelector = () => { throw new Error('page interception'); };
    )JS"_utf16);
    auto world = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    auto check = [&](Web::DerpBirdScriptWorld& target, Utf16View source) {
        auto result = TRY_OR_FAIL(target.run(source));
        VERIFY(!result.is_abrupt());
        EXPECT(result.value().is_boolean());
        EXPECT(result.value().as_bool());
    };
    check(*world, R"JS(
        const probe = document.querySelector('#probe');
        globalThis.scriptOnly = 789;
        probe.scriptSecret = 'private';
        probe.setAttribute('data-shared', 'yes');
        globalThis.callbackIsolated = false;
        probe.addEventListener('world-test', event => {
            callbackIsolated = event.target === probe && event.target.pageSecret === undefined;
            probe.setAttribute('data-callback', 'ran');
        });
        typeof pageOnly === 'undefined' && typeof GM_getValue === 'undefined'
            && !Object.prototype.pagePoison && probe.pageSecret === undefined
            && window === self && window === globalThis && top === window;
    )JS"_utf16);
    EXPECT(page_run(R"JS(
        document.getElementById('probe').dispatchEvent(new Event('world-test'));
        typeof scriptOnly === 'undefined'
            && document.getElementById('probe').scriptSecret === undefined
            && document.getElementById('probe').getAttribute('data-shared') === 'yes';
    )JS"_utf16).as_bool());
    check(*world, "callbackIsolated === true"_utf16);
    check(*world, R"JS(
        probe.onclick = () => { probe.setAttribute('data-idl-handler', 'ran'); };
        probe.onclick !== null;
    )JS"_utf16);
    EXPECT(page_run(R"JS(
        document.getElementById('probe').dispatchEvent(new MouseEvent('click'));
        document.getElementById('probe').getAttribute('data-idl-handler') === 'ran';
    )JS"_utf16).as_bool());

    check(*world, R"JS(
        globalThis.mutationIsolated = false;
        const observer = new MutationObserver((records, deliveredObserver) => {
            mutationIsolated = Object.getPrototypeOf(records) === Array.prototype
                && records[0].target === probe && records[0].target.pageSecret === undefined
                && deliveredObserver === observer;
        });
        observer.observe(probe, { attributes: true });
        probe.setAttribute('data-observe', 'changed');
        true;
    )JS"_utf16);
    Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
    check(*world, "mutationIsolated === true"_utf16);
    auto resize_result = TRY_OR_FAIL(world->run(R"JS(
        globalThis.resizeIsolated = false;
        globalThis.resizeObserver = new ResizeObserver(function(records, delivered) {
            resizeIsolated = Object.getPrototypeOf(records) === Array.prototype
                && delivered === resizeObserver && this === resizeObserver;
            probe.setAttribute('data-resize-callback', 'ran');
        }); resizeObserver;
    )JS"_utf16));
    VERIFY(!resize_result.is_abrupt());
    auto* resize = Web::Bindings::impl_from<Web::ResizeObserver::ResizeObserver>(&resize_result.value().as_object());
    VERIFY(resize);
    Web::ResizeObserver::invoke_resize_observer_callback(*resize, {});
    check(*world, "resizeIsolated === true"_utf16);

    auto intersection_result = TRY_OR_FAIL(world->run(R"JS(
        globalThis.intersectionIsolated = false;
        globalThis.intersectionObserver = new IntersectionObserver(function(records, delivered) {
            intersectionIsolated = Object.getPrototypeOf(records) === Array.prototype
                && delivered === intersectionObserver && this === intersectionObserver;
            probe.setAttribute('data-intersection-callback', 'ran');
        }); intersectionObserver;
    )JS"_utf16));
    VERIFY(!intersection_result.is_abrupt());
    auto* intersection = Web::Bindings::impl_from<Web::IntersectionObserver::IntersectionObserver>(&intersection_result.value().as_object());
    VERIFY(intersection);
    Vector<GC::Root<Web::IntersectionObserver::IntersectionObserverEntry>> intersection_entries;
    Web::IntersectionObserver::invoke_intersection_observer_callback(*intersection, intersection_entries);
    check(*world, "intersectionIsolated === true"_utf16);

    auto performance_result = TRY_OR_FAIL(world->run(R"JS(
        globalThis.performanceIsolated = false;
        globalThis.performanceObserver = new PerformanceObserver(function(entries, delivered, options) {
            performanceIsolated = Object.getPrototypeOf(entries.getEntries()) === Array.prototype
                && Object.getPrototypeOf(options) === Object.prototype
                && delivered === performanceObserver && this === performanceObserver;
            probe.setAttribute('data-performance-callback', 'ran');
        }); performanceObserver.observe({ entryTypes: ["mark"] });
        performance.mark("derpbird-world-callback"); performanceObserver;
    )JS"_utf16));
    VERIFY(!performance_result.is_abrupt());
    // Exercise the real delivery task; PerformanceObserver now lives in Rust.
    for (size_t turn = 0; turn < 64; ++turn) {
        auto delivered = MUST(world->run("performanceIsolated"_utf16));
        VERIFY(!delivered.is_abrupt());
        if (delivered.value().as_bool()) break;
        Web::HTML::main_thread_event_loop().process();
    }
    check(*world, "performanceIsolated === true"_utf16);

    auto walker_result = TRY_OR_FAIL(world->run(R"JS(
        const filterRoot = document.createElement('section');
        filterRoot.append(document.createElement('span'), document.createElement('span'));
        globalThis.filterIsolated = false;
        globalThis.walker = document.createTreeWalker(filterRoot, NodeFilter.SHOW_ELEMENT, node => {
            filterIsolated = Object.getPrototypeOf(node) === HTMLSpanElement.prototype
                && node.pageSecret === undefined;
            probe.setAttribute('data-filter-callback', 'ran');
            return NodeFilter.FILTER_ACCEPT;
        });
        walker.nextNode();
        walker;
    )JS"_utf16));
    VERIFY(!walker_result.is_abrupt());
    GC::Root<Web::DOM::TreeWalker> walker = Web::Bindings::impl_from<Web::DOM::TreeWalker>(&walker_result.value().as_object());
    VERIFY(walker);
    check(*world, "filterIsolated === true"_utf16);

    auto second = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    check(*second, R"JS(
        typeof scriptOnly === 'undefined'
            && document.querySelector('#probe').scriptSecret === undefined
            && document.querySelector('#probe').getAttribute('data-shared') === 'yes';
    )JS"_utf16);
    check(*world, R"JS(
        Promise.resolve().then(() => probe.setAttribute('data-closed-job', 'ran'));
        true;
    )JS"_utf16);
    check(*world, "performance.mark('derpbird-revoked-callback'); true"_utf16);
    world->close();
    page_run(R"JS(
        Promise.resolve().then(() => document.getElementById('probe').setAttribute('data-page-job', 'ran'));
    )JS"_utf16);
    EXPECT(world->run("true"_utf16).is_error());
    page_run("document.getElementById('probe').removeAttribute('data-filter-callback')"_utf16);
    auto closed_filter_result = Web::Bindings::next_node(main_realm, *walker);
    VERIFY(closed_filter_result.is_error());
    auto exception_value = closed_filter_result.release_error().value();
    VERIFY(exception_value.is_object());
    auto* exception = Web::Bindings::impl_from<Web::WebIDL::DOMException>(&exception_value.as_object());
    VERIFY(exception);
    EXPECT_EQ(exception->name(), "InvalidStateError"_utf16_fly_string);
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-filter-callback')"_utf16).as_bool());
    EXPECT(page_run(R"JS(
        document.getElementById('probe').removeAttribute('data-callback');
        document.getElementById('probe').dispatchEvent(new Event('world-test'));
        !document.getElementById('probe').hasAttribute('data-callback');
    )JS"_utf16).as_bool());
    page_run(R"JS(
        for (const name of ['resize', 'intersection', 'performance'])
            document.getElementById('probe').removeAttribute('data-' + name + '-callback');
    )JS"_utf16);
    Web::ResizeObserver::invoke_resize_observer_callback(*resize, {});
    Web::IntersectionObserver::invoke_intersection_observer_callback(*intersection, intersection_entries);
    for (size_t turn = 0; turn < 64; ++turn) Web::HTML::main_thread_event_loop().process();
    Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-closed-job')"_utf16).as_bool());
    EXPECT(page_run("document.getElementById('probe').getAttribute('data-page-job') === 'ran'"_utf16).as_bool());
    EXPECT(page_run(R"JS(
        ['resize', 'intersection', 'performance'].every(name =>
            !document.getElementById('probe').hasAttribute('data-' + name + '-callback'));
    )JS"_utf16).as_bool());
    check(*second, "document.getElementById('probe') !== null"_utf16);
    second->close();

    document->set_url(*URL::Parser::basic_parse("https://example.com/page"sv));
    JsonArray installed;
    auto add_script = [&](StringView id, StringView phase, StringView source, bool needs_grant = false, bool grant_none = false) {
        auto metadata = MUST(JsonValue::from_string(R"JSON({
            "runAt":"document-start", "grantNone":false, "noFrames":false,
            "matches":["https://example.com/*"], "includes":[], "excludeMatches":[], "excludes":[],
            "grants":[], "requires":[], "connects":[], "resources":[]
        })JSON"sv)).as_object();
        metadata.set("runAt"sv, phase);
        metadata.set("grantNone"sv, grant_none);
        metadata.set("description"sv, "\");globalThis.metadataInjection=true;// Привет"sv);
        if (needs_grant) {
            JsonArray grants;
            MUST(grants.append("GM_getValue"sv));
            metadata.set("grants"sv, move(grants));
        }
        metadata.set("entries"sv, MUST(JsonValue::from_string(R"JSON([{"key":"sandbox","value":"DOM"}])JSON"sv)));
        JsonObject record;
        record.set("id"sv, id);
        record.set("name"sv, id);
        record.set("source"sv, source);
        record.set("enabled"sv, true);
        record.set("fileAccessGranted"sv, false);
        record.set("metadata"sv, move(metadata));
        MUST(installed.append(move(record)));
    };
    add_script("start"sv, "document-start"sv, R"JS(
        if (GM.info !== GM_info || GM_info.script.name !== 'start' || GM_info.scriptHandler !== 'DerpBird' || typeof metadataInjection !== 'undefined')
            throw new Error('incorrect script info');
        GM_info.script.name = 'mutated';
        GM_info.script.matches.push('https://mutation.invalid/*');
        if (typeof GM_addStyle !== 'undefined' || typeof GM.addStyle !== 'undefined' || typeof GM_addElement !== 'undefined' || typeof GM.addElement !== 'undefined' || typeof GM_log !== 'undefined' || typeof GM.setClipboard !== 'undefined') throw new Error('ungranted style API');
        globalThis.ownValue = 'private';
        const node = document.getElementById('probe');
        node.setAttribute('data-session-start', String(Number(node.getAttribute('data-session-start') || 0) + 1));
        node.addEventListener('session-event', () => node.setAttribute('data-session-event', 'ran'));
    )JS"sv);
    add_script("end"sv, "document-end"sv, "if (GM.info !== GM_info || GM_info.script.name !== 'end' || GM_info.script.matches.length !== 1) throw new Error('metadata leaked'); if (typeof ownValue !== 'undefined') throw new Error('shared realm'); document.getElementById('probe').setAttribute('data-session-end', 'ran')"sv);
    add_script("bad"sv, "document-start"sv, "throw new Error('expected fixture exception')"sv);
    add_script("idle"sv, "document-idle"sv, "document.getElementById('probe').setAttribute('data-session-idle', 'ran')"sv);
    add_script("needs-grant"sv, "document-start"sv, "throw new Error('must not execute')"sv, true);
    add_script("menu"sv, "context-menu"sv, "const node=document.getElementById('probe');node.setAttribute('data-menu-runs',String(Number(node.getAttribute('data-menu-runs')||0)+1));node.addEventListener('menu-event',()=>node.setAttribute('data-menu-events',String(Number(node.getAttribute('data-menu-events')||0)+1)));"sv);
    add_script("page-world"sv, "document-start"sv, R"JS(
        if (GM.info !== GM_info || GM_info.script.name !== 'page-world' || GM_info.sandboxMode !== 'raw'
            || GM_info.script.grant[0] !== 'none') throw new Error('page metadata missing');
        if (pageOnly !== 123 || this !== window || !Object.prototype.pagePoison)
            throw new Error('not the page realm');
        var localDeclaration = 'must stay local';
        const localConstant = 'must stay local';
        window.pageScriptRuns = (window.pageScriptRuns || 0) + 1;
        document.getElementById('probe').pageScriptExpando = 42;
        Promise.resolve().then(() => window.pageScriptJob = true);
        return;
        throw new Error('top-level return ignored');
    )JS"sv, false, true);
    add_script("page-syntax-error"sv, "document-start"sv, "let = ;"sv, false, true);
    add_script("isolated-return"sv, "document-start"sv, "var localDeclaration = 1; return; throw new Error('unreachable');"sv);
    add_script("info-only"sv, "document-start"sv, R"JS(
        if (GM_info.script.name !== 'info-only' || GM.info !== GM_info) throw new Error('missing info grant');
        if (typeof GM.addStyle !== 'undefined') throw new Error('ungranted dotted alias');
        const style = GM_addStyle('#probe { --derpbird-script-style: granted; }');
        if (!(style instanceof HTMLStyleElement) || !style.isConnected || style.sheet.cssRules.length !== 1)
            throw new Error('style not attached');
        if (getComputedStyle(document.getElementById('probe')).getPropertyValue('--derpbird-script-style').trim() !== 'granted')
            throw new Error('style not applied');
        style.remove();
        if (getComputedStyle(document.getElementById('probe')).getPropertyValue('--derpbird-script-style').trim() !== '')
            throw new Error('style not removable');
    )JS"sv);
    auto info_metadata = installed.values().last().as_object().get_object("metadata"sv).value();
    JsonArray info_grants;
    MUST(info_grants.append("GM_info"sv));
    MUST(info_grants.append("GM_addStyle"sv));
    MUST(info_grants.append("GM.info"sv));
    info_metadata.set("grants"sv, move(info_grants));
    JsonArray resource_grants = info_metadata.get_array("grants"sv).value();
    resource_grants.must_append("GM_getResourceText"sv);
    resource_grants.must_append("GM_getResourceURL"sv);
    info_metadata.set("grants"sv, move(resource_grants));
    info_metadata.set("resources"sv, MUST(JsonValue::from_string(R"JSON([{"name":"__proto__","url":"https://example.com/style"}])JSON"sv)));
    installed.values().last().as_object().set("metadata"sv, move(info_metadata));
    auto& resource_record = installed.values().last().as_object();
    resource_record.set("cachedResources"sv, MUST(JsonValue::from_string(R"JSON([{"name":"__proto__","url":"https://example.com/style","text":"resource fixture","dataUrl":"data:text/plain;base64,cmVzb3VyY2UgZml4dHVyZQ=="}])JSON"sv)));
    resource_record.set("source"sv, MUST(String::formatted("if (GM_getResourceText('__proto__') !== 'resource fixture' || !GM_getResourceURL('__proto__').startsWith('data:')) throw new Error('resource API');\n{}", resource_record.get_string("source"sv).value())));

    // Required sources share lexical scope, run in order and stay within the
    // selected userscript realm. A trailing line comment must not swallow code.
    auto start_record = installed.values()[0].as_object();
    auto start_meta = start_record.get_object("metadata"sv).value();
    start_meta.set("requires"sv, MUST(JsonValue::from_string(R"JSON(["https://example.com/a.js","https://example.com/b.js"])JSON"sv)));
    start_record.set("metadata"sv, move(start_meta));
    start_record.set("requiredSources"sv, MUST(JsonValue::from_string(R"JSON([
        {"url":"https://example.com/a.js","source":"const requiredOrder = ['first']; // trailing comment"},
        {"url":"https://example.com/b.js","source":"requiredOrder.push('second'); var requiredAnswer = 42;"}
    ])JSON"sv)));
    start_record.set("source"sv, MUST(String::formatted("if (requiredOrder.join(',') !== 'first,second' || requiredAnswer !== 42) throw new Error('require order');\n{}", start_record.get_string("source"sv).value())));
    installed.values()[0] = move(start_record);
    Web::DerpBirdScriptRegistry registry;
    MUST(registry.replace(installed));
    auto session = TRY_OR_FAIL(Web::DerpBirdDocumentScripts::create(*document, registry));
    JsonArray empty_registry;
    MUST(registry.replace(empty_registry));
    MUST(session->dispatch(Web::DerpBirdScriptRunAt::DocumentStart));
    MUST(session->dispatch(Web::DerpBirdScriptRunAt::DocumentStart));
    EXPECT(page_run("document.getElementById('probe').getAttribute('data-session-start') === '1' && !document.getElementById('probe').hasAttribute('data-session-end')"_utf16).as_bool());
    MUST(session->dispatch(Web::DerpBirdScriptRunAt::DocumentEnd));
    EXPECT(page_run("document.getElementById('probe').getAttribute('data-session-end') === 'ran'"_utf16).as_bool());
    using ScriptStatus = Web::DerpBirdDocumentScripts::Status;
    VERIFY(session->entries().size() == 10);
    EXPECT(session->entries()[0].status == ScriptStatus::Executed);
    EXPECT(session->entries()[1].status == ScriptStatus::Executed);
    EXPECT(session->entries()[2].status == ScriptStatus::Failed);
    EXPECT(session->entries()[4].status == ScriptStatus::UnsupportedRequirements);
    EXPECT(session->entries()[6].status == ScriptStatus::Executed);
    EXPECT(!session->entries()[6].world);
    EXPECT(session->entries()[7].status == ScriptStatus::Failed);
    EXPECT(session->entries()[8].status == ScriptStatus::Executed);
    EXPECT(session->entries()[9].status == ScriptStatus::Executed);
    EXPECT(session->entries()[9].info.is_empty());
    EXPECT(page_run(R"JS(
        pageScriptRuns === 1 && pageScriptJob === true
            && document.getElementById('probe').pageScriptExpando === 42
            && typeof localDeclaration === 'undefined' && typeof localConstant === 'undefined'
            && typeof GM_info === 'undefined' && typeof GM === 'undefined';
    )JS"_utf16).as_bool());
    check(*session->entries()[0].world, "typeof pageScriptRuns === 'undefined' && document.getElementById('probe').pageScriptExpando === undefined"_utf16);
    EXPECT(session->dispatch(Web::DerpBirdScriptRunAt::ContextMenu).is_error());
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-menu-runs')"_utf16).as_bool());
    MUST(session->dispatch(Web::DerpBirdScriptRunAt::ContextMenu, "menu"sv, session->entries()[5].generation + 1));
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-menu-runs')"_utf16).as_bool());
    MUST(session->dispatch(Web::DerpBirdScriptRunAt::ContextMenu, "menu"sv, session->entries()[5].generation));
    MUST(session->dispatch(Web::DerpBirdScriptRunAt::ContextMenu, "menu"sv, session->entries()[5].generation));
    EXPECT(page_run("document.getElementById('probe').getAttribute('data-menu-runs')==='2'"_utf16).as_bool());
    EXPECT(page_run("document.getElementById('probe').dispatchEvent(new Event('menu-event'));document.getElementById('probe').getAttribute('data-menu-events')==='2'"_utf16).as_bool());


    auto sandbox_flags = document->active_sandboxing_flag_set();
    document->set_active_sandboxing_flag_set(sandbox_flags | Web::HTML::SandboxingFlagSet::SandboxedScripts);
    EXPECT(Web::run_derpbird_page_script(*document, "blocked"sv, "window.sandboxBypass = true"_utf16).is_error());
    document->set_active_sandboxing_flag_set(sandbox_flags);
    EXPECT(page_run("typeof sandboxBypass === 'undefined'"_utf16).as_bool());

    // Reinstall into a fresh registry/session so generations describe this run.
    Web::DerpBirdScriptRegistry updates;
    MUST(updates.replace(installed));
    auto live = TRY_OR_FAIL(Web::DerpBirdDocumentScripts::create(*document, updates));
    updates.on_replace = [&] { live->reconcile(updates); };
    MUST(live->dispatch(Web::DerpBirdScriptRunAt::DocumentStart));
    MUST(updates.replace(installed));
    EXPECT(live->entries()[0].status == ScriptStatus::Executed);
    EXPECT(live->entries()[0].world);
    EXPECT(updates.replace(JsonValue { false }).is_error());
    EXPECT(live->entries()[0].world);
    check(*live->entries()[0].world, "document.getElementById('probe').addEventListener('revoked-event', () => document.getElementById('probe').setAttribute('data-revoked', 'ran')); true;"_utf16);
    auto changed = installed;
    changed.values()[0].as_object().set("enabled"sv, false);
    changed.values()[3].as_object().set("source"sv, "throw new Error('must wait for reload');"sv);
    MUST(updates.replace(changed));
    EXPECT(live->entries()[0].status == ScriptStatus::Cancelled);
    EXPECT(!live->entries()[0].world);
    page_run("document.getElementById('probe').dispatchEvent(new Event('revoked-event'))"_utf16);
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-revoked')"_utf16).as_bool());
    EXPECT(live->entries()[3].status == ScriptStatus::Cancelled);
    EXPECT(live->entries()[8].status == ScriptStatus::Executed);
    EXPECT(live->entries()[8].world);
    // Reenable cannot resurrect a revoked world in this document.
    MUST(updates.replace(installed));
    EXPECT(live->entries()[0].status == ScriptStatus::Cancelled);
    MUST(live->dispatch(Web::DerpBirdScriptRunAt::DocumentIdle));
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-session-idle')"_utf16).as_bool());
    JsonArray removed;
    MUST(updates.replace(removed));
    EXPECT(!live->entries()[8].world);
    updates.on_replace = {};
    live->close();

    // Native GM bindings are available only when the session has a provider.
    installed = {};
    add_script("values"sv, "document-start"sv, R"JS(
        if(GM.info!==GM_info || typeof GM.setValue!=='undefined' || Object.getPrototypeOf(GM.getValue)!==Function.prototype) throw new Error('wrong grants');
        GM.getValue('key').then(value=>document.getElementById('probe').setAttribute('data-value',String(value)));
    )JS"sv);
    auto value_metadata = installed.values()[0].as_object().get_object("metadata"sv).value();
    JsonArray value_grants;
    MUST(value_grants.append("GM.getValue"sv));
    value_metadata.set("grants"sv, move(value_grants));
    installed.values()[0].as_object().set("metadata"sv, move(value_metadata));
    Web::DerpBirdScriptRegistry value_registry;
    MUST(value_registry.replace(installed));
    auto without_provider = TRY_OR_FAIL(Web::DerpBirdDocumentScripts::create(*document, value_registry));
    EXPECT(without_provider->entries()[0].status == ScriptStatus::UnsupportedRequirements);
    using ValueProtocol = Web::DerpBirdValueProtocol;
    Vector<Web::DerpBirdScriptValues::Completion> value_callbacks;
    auto with_provider = TRY_OR_FAIL(Web::DerpBirdDocumentScripts::create(*document, value_registry,
        [&](ByteBuffer payload, Web::DerpBirdScriptValues::Completion completion) -> ErrorOr<void> {
            auto request = TRY(ValueProtocol::decode_request(payload.bytes()));
            if(request.method == ValueProtocol::Method::Open) {
                EXPECT(request.identity == "values"sv);
                completion(TRY(ValueProtocol::encode(ValueProtocol::Response { ValueProtocol::Kind::Token, "token"_string, {}, {}, 0 })));
            } else {
                EXPECT(request.method == ValueProtocol::Method::Get);
                EXPECT(request.identity == "token"sv);
                value_callbacks.append(move(completion));
            }
            return {};
        }));
    MUST(with_provider->dispatch(Web::DerpBirdScriptRunAt::DocumentStart));
    EXPECT(with_provider->entries()[0].status == ScriptStatus::Executed);
    EXPECT_EQ(value_callbacks.size(), 1u);
    auto encoded_value = MUST(Web::encode_derpbird_value(vm, JS::Value(42)));
    value_callbacks.take_first()(MUST(ValueProtocol::encode(ValueProtocol::Response { ValueProtocol::Kind::Value, {}, move(encoded_value), {}, 0 })));
    Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
    EXPECT(page_run("document.getElementById('probe').getAttribute('data-value')==='42' && typeof GM==='undefined'"_utf16).as_bool());
    check(*with_provider->entries()[0].world, "GM.getValue('key').then(()=>document.getElementById('probe').setAttribute('data-value-late','ran')); true;"_utf16);
    EXPECT_EQ(value_callbacks.size(), 1u);
    MUST(value_registry.replace(JsonArray {}));
    with_provider->reconcile(value_registry);
    EXPECT(!with_provider->entries()[0].world);
    value_callbacks.take_first()(Error::from_string_literal("late"));
    Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-value-late')"_utf16).as_bool());
    with_provider->close();
    without_provider->close();

    page_run("document.getElementById('probe').style.setProperty('display','block','important')"_utf16);
    page->set_content_blocking_policy(true, {});
    auto blocker_world = TRY_OR_FAIL(Web::DerpBirdIsolatedScriptlets::create(*document));
    EXPECT(TRY_OR_FAIL(blocker_world->run(R"JS(
        if (typeof GM !== 'undefined' || typeof GM_getValue !== 'undefined' || typeof pageOnly !== 'undefined')
            throw new Error('Page globals or privileged APIs leaked');
        const node = document.querySelector('#probe');
        node.setAttribute('data-blocker-world', 'isolated');
        if (!__derpbirdFilterEnabled() || !__derpbirdAddCSS('#probe { display: none !important; }'))
            throw new Error('native user-origin style rejected');

        node.addEventListener('blocker-event', () => node.setAttribute('data-blocker-event', 'ran'));
    )JS"_utf16)));
    EXPECT(page_run("document.getElementById('probe').getAttribute('data-blocker-world')==='isolated'"_utf16).as_bool());
    EXPECT(page_run("getComputedStyle(document.getElementById('probe')).display==='none' && typeof __derpbirdAddCSS==='undefined' && typeof __derpbirdFilterEnabled==='undefined'"_utf16).as_bool());
    page->set_content_blocking_policy(false, {});
    EXPECT(page_run("getComputedStyle(document.getElementById('probe')).display==='block'"_utf16).as_bool());
    page->set_content_blocking_policy(true, {});
    EXPECT(page_run("getComputedStyle(document.getElementById('probe')).display==='none'"_utf16).as_bool());
    EXPECT(blocker_world->run("true"_utf16).is_error());
    blocker_world->close();
    EXPECT(page_run("getComputedStyle(document.getElementById('probe')).display==='block'"_utf16).as_bool());
    page_run("document.getElementById('probe').dispatchEvent(new Event('blocker-event'))"_utf16);
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-blocker-event')"_utf16).as_bool());
    EXPECT(blocker_world->run("true"_utf16).is_error());
    auto inactive_blocker = TRY_OR_FAIL(Web::DerpBirdIsolatedScriptlets::create(*document));
    EXPECT(TRY_OR_FAIL(inactive_blocker->run(R"JS(
        const node = document.getElementById('probe');
        node.addEventListener('inactive-blocker-event', () => node.setAttribute('data-inactive-blocker', 'ran'));
    )JS"_utf16)));

    auto lifecycle_world = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    check(*lifecycle_world, R"JS(
        const lifecycleProbe = document.getElementById('probe');
        lifecycleProbe.addEventListener('lifecycle-test', () => lifecycleProbe.setAttribute('data-lifecycle-event', 'ran'));
        Promise.resolve().then(() => lifecycleProbe.setAttribute('data-lifecycle-job', 'ran'));
        true;
    )JS"_utf16);
    // Exercise the document's actual inactivity notification path without an
    // explicit owner close. Full process-level navigation coverage is still required.
    document->did_stop_being_active_document_in_navigable();
    EXPECT(inactive_blocker->run("true"_utf16).is_error());
    page_run("document.getElementById('probe').dispatchEvent(new Event('inactive-blocker-event'))"_utf16);
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-inactive-blocker')"_utf16).as_bool());

    EXPECT(lifecycle_world->run("true"_utf16).is_error());
    EXPECT(session->is_closed());
    EXPECT(session->entries()[3].status == ScriptStatus::Cancelled);
    EXPECT(session->entries()[5].status == ScriptStatus::Executed);
    EXPECT(!session->entries()[5].world);
    EXPECT(session->entries()[5].previous_invocations.is_empty());
    EXPECT(page_run("document.getElementById('probe').dispatchEvent(new Event('menu-event'));document.getElementById('probe').getAttribute('data-menu-events')==='2'"_utf16).as_bool());
    EXPECT(session->dispatch(Web::DerpBirdScriptRunAt::DocumentIdle).is_error());
    page_run("document.getElementById('probe').dispatchEvent(new Event('session-event'))"_utf16);
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-session-event') && !document.getElementById('probe').hasAttribute('data-session-idle')"_utf16).as_bool());
    page_run("document.getElementById('probe').dispatchEvent(new Event('lifecycle-test'))"_utf16);
    Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
    EXPECT(page_run("!document.getElementById('probe').hasAttribute('data-lifecycle-event') && !document.getElementById('probe').hasAttribute('data-lifecycle-job')"_utf16).as_bool());
}

TEST_CASE(derpbird_script_world_revocation_releases_queued_web_lock)
{
    install_test_fonts();
    Web::Bindings::create_a_principal_javascript_realm();
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    auto address = URL::Parser::basic_parse("https://example.com/locks"sv).release_value();
    document->set_url(address);
    document->set_origin(address.origin());
    auto& realm = document->window()->principal_realm();
    vm.pop_execution_context();
    auto run = [&](Utf16View source) {
        auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(realm);
        vm.push_execution_context(settings.realm_execution_context());
        ScopeGuard pop = [&] { vm.pop_execution_context(); };
        auto script = JS::Script::parse(source, realm);
        VERIFY(!script.is_error());
        auto result = vm.run(script.value());
        VERIFY(!result.is_error());
        return result.release_value();
    };
    run(R"JS(
        const probe = document.createElement('div');
        probe.id = 'lock-probe';
        (document.body || document.documentElement || document).appendChild(probe);
    )JS"_utf16);
    auto world = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    auto queued = TRY_OR_FAIL(world->run(R"JS(
        navigator.locks.request('derpbird-revoked-lock', () => {
            document.getElementById('lock-probe').setAttribute('data-revoked', 'ran');
            return new Promise(() => {});
        });
        true;
    )JS"_utf16));
    VERIFY(!queued.is_abrupt());
    world->close();
    run(R"JS(
        globalThis.pageLockAcquired = false;
        navigator.locks.request('derpbird-revoked-lock', () => { pageLockAcquired = true; });
    )JS"_utf16);
    for (size_t turn = 0; turn < 64; ++turn)
        Web::HTML::main_thread_event_loop().process();
    Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
    EXPECT(run("!document.getElementById('lock-probe').hasAttribute('data-revoked')"_utf16).as_bool());
    EXPECT(run("pageLockAcquired"_utf16).as_bool());
}

TEST_CASE(derpbird_credential_fields_revalidate_forms_documents_and_script_mutation)
{
    install_test_fonts();
    Web::Bindings::create_a_principal_javascript_realm();
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    auto address = URL::Parser::basic_parse("https://example.com/login"sv).release_value();
    document->set_url(address);
    document->set_origin(address.origin());
    auto& realm = document->window()->principal_realm();
    vm.pop_execution_context();
    auto run = [&](Utf16View source) {
        auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(realm);
        vm.push_execution_context(settings.realm_execution_context());
        ScopeGuard pop = [&] { vm.pop_execution_context(); };
        auto script = JS::Script::parse(source, realm);
        VERIFY(!script.is_error());
        auto result = vm.run(script.value());
        VERIFY(!result.is_error());
        return result.release_value();
    };
    run(R"JS(
        if (!document.documentElement) document.appendChild(document.createElement('html'));
        if (!document.body) document.documentElement.appendChild(document.createElement('body'));
        document.body.innerHTML='<form id="f"><input id="u" autocomplete="section-login username"><input id="p" type="password" autocomplete="current-password"></form><form id="other"></form>';
        globalThis.inputs=[];
        document.getElementById('u').addEventListener('input',e=>inputs.push('u:'+e.isTrusted));
        document.getElementById('p').addEventListener('input',e=>inputs.push('p:'+e.isTrusted));
    )JS"_utf16);
    auto select = [&] { return Web::DerpBirdCredentialFields::select(as<Web::HTML::HTMLInputElement>(*document->get_element_by_id("p"_utf16))); };
    auto username_fields = MUST(Web::DerpBirdCredentialFields::select_from_focus(as<Web::HTML::HTMLInputElement>(*document->get_element_by_id("u"_utf16))));
    MUST(username_fields.fill("from-focus"_string, "from-focus-secret"_string));
    EXPECT(run("document.getElementById('u').value==='from-focus' && document.getElementById('p').value==='from-focus-secret'"_utf16).as_bool());
    run("inputs=[]"_utf16);
    auto fields = MUST(select());
    EXPECT_EQ(fields.origin(), "https://example.com"sv);
    EXPECT(fields.kind() == Web::DerpBirdCredentialFields::Kind::Login);
    MUST(fields.fill("alice"_string, "fixture-password"_string));
    auto values = MUST(fields.capture());
    EXPECT_EQ(values.username, "alice"sv);
    EXPECT_EQ(values.password, "fixture-password"sv);
    EXPECT(run("inputs.join(',')==='u:true,p:true'"_utf16).as_bool());
    run("document.getElementById('p').type='text'"_utf16);
    EXPECT(fields.capture().is_error());
    EXPECT(fields.fill("x"_string, "must-not-fill"_string).is_error());
    run("document.getElementById('p').type='password'; document.getElementById('p').setAttribute('autocomplete','new-password')"_utf16);
    EXPECT(fields.capture().is_error());
    auto registration = MUST(select());
    EXPECT(registration.kind() == Web::DerpBirdCredentialFields::Kind::NewPassword);
    EXPECT(registration.fill("x"_string, "saved-password"_string).is_error());
    run("document.getElementById('p').setAttribute('autocomplete','current-password'); document.getElementById('u').setAttribute('form','other')"_utf16);
    EXPECT(fields.capture().is_error());
    run("document.getElementById('u').removeAttribute('form'); document.getElementById('p').readOnly=true"_utf16);
    EXPECT(select().is_error());
    run("document.getElementById('p').readOnly=false; document.getElementById('p').value=''; document.getElementById('u').addEventListener('input',()=>{document.getElementById('p').type='text'},{once:true})"_utf16);
    fields = MUST(select());
    EXPECT(fields.fill("bob"_string, "must-not-leak"_string).is_error());
    EXPECT(run("document.getElementById('p').value===''"_utf16).as_bool());
    run("document.getElementById('p').type='password'; document.getElementById('u').addEventListener('input',()=>document.getElementById('p').remove(),{once:true})"_utf16);
    fields = MUST(select());
    EXPECT(fields.fill("bob"_string, "must-not-fill-detached"_string).is_error());
    run("document.getElementById('f').insertAdjacentHTML('beforeend','<input id=p type=password autocomplete=current-password>')"_utf16);
    fields = MUST(select());
    run("document.getElementById('p').value='x'.repeat(65537)"_utf16);
    EXPECT(fields.capture().is_error());
    run("document.getElementById('p').value='é'.repeat(32769)"_utf16);
    EXPECT(fields.capture().is_error());
    run("document.getElementById('p').value='fixture'; document.getElementById('u').value='x'.repeat(1025)"_utf16);
    EXPECT(fields.capture().is_error());
    run("document.getElementById('u').value='alice'; document.getElementById('p').autocomplete='one-time-code'"_utf16);
    EXPECT(select().is_error());
    run("document.getElementById('p').autocomplete='current-password'; document.getElementById('p').disabled=true"_utf16);
    EXPECT(select().is_error());
    run("document.getElementById('p').disabled=false; document.getElementById('f').insertAdjacentHTML('beforeend','<input id=duplicate autocomplete=username>')"_utf16);
    EXPECT(select().is_error());
    run("document.getElementById('duplicate').remove(); document.getElementById('u').autocomplete=''; document.getElementById('f').insertAdjacentHTML('beforeend','<input id=confirmation type=password>')"_utf16);
    EXPECT(select().is_error());
    run("document.getElementById('confirmation').remove(); document.getElementById('u').autocomplete='username'"_utf16);
    fields = MUST(select());
    MonotonicTime now;
    auto session = Web::DerpBirdCredentialSession::create(*document, [&] { return now; });
    auto select_session = [&] { return session->select(as<Web::HTML::HTMLInputElement>(*document->get_element_by_id("p"_utf16))); };
    auto first = MUST(select_session());
    auto second = MUST(select_session());
    EXPECT(first.ticket != second.ticket);
    EXPECT(session->fill(first, "stale"_string, "must-not-fill"_string).is_error());
    MUST(session->fill(second, "session-user"_string, "session-fixture"_string));
    EXPECT(session->fill(second, "replay"_string, "must-not-fill"_string).is_error());
    auto captured = MUST(session->capture(MUST(select_session())));
    EXPECT_EQ(captured.username, "session-user"sv);
    EXPECT_EQ(captured.password, "session-fixture"sv);
    auto expired = MUST(select_session());
    now += AK::Duration::from_seconds(30);
    EXPECT(session->capture(expired).is_error());
    auto wrong_origin = MUST(select_session());
    wrong_origin.origin = "https://other.example"_string;
    EXPECT(session->fill(wrong_origin, "x"_string, "must-not-fill"_string).is_error());
    wrong_origin.origin = "https://example.com"_string;
    EXPECT(session->capture(wrong_origin).is_error()); // consumed on matching invalid reply
    auto cancelled = MUST(select_session());
    session->cancel();
    EXPECT(session->capture(cancelled).is_error());
    auto invalidated = MUST(select_session());
    run("document.getElementById('p').disabled=true"_utf16);
    EXPECT(select_session().is_error());
    run("document.getElementById('p').disabled=false"_utf16);
    EXPECT(session->capture(invalidated).is_error());
    auto another_session = Web::DerpBirdCredentialSession::create(*document);
    auto another_selection = MUST(another_session->select(as<Web::HTML::HTMLInputElement>(*document->get_element_by_id("p"_utf16))));
    auto reentrant = MUST(select_session());
    EXPECT(another_selection.ticket != reentrant.ticket);
    EXPECT(session->capture(another_selection).is_error());
    struct ReplayState : RefCounted<ReplayState> {
        GC_HOLDS_NO_CELL_EDGES;
        bool attempted { false };
        WeakPtr<Web::DerpBirdCredentialSession> session;
        Web::DerpBirdCredentialSession::Selection selection;
    };
    auto replay = make_ref_counted<ReplayState>();
    replay->session = session->make_weak_ptr();
    replay->selection = reentrant;
    vm.push_execution_context(Web::Bindings::principal_host_defined_environment_settings_object(realm).realm_execution_context());
    auto revoke = JS::NativeFunction::create(realm, GC::create_function(vm.heap(), GC::captures(replay), [](auto& replay, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        replay->attempted = true;
        VERIFY(replay->session);
        EXPECT(replay->session->fill(replay->selection, "replayed"_string, "must-not-fill"_string).is_error());
        replay->session->cancel();
        return JS::js_undefined();
    }), 0, "cancelCredential"_utf16_fly_string);
    realm.global_object().define_direct_property("cancelCredential"_utf16_fly_string, revoke, JS::default_attributes);
    vm.pop_execution_context();
    run("document.getElementById('p').value='unchanged'; document.getElementById('u').addEventListener('input',()=>cancelCredential(),{once:true})"_utf16);
    EXPECT(session->fill(reentrant, "new-user"_string, "must-not-fill-after-cancel"_string).is_error());
    EXPECT(replay->attempted);
    EXPECT(run("document.getElementById('p').value==='unchanged'"_utf16).as_bool());
    realm.global_object().define_direct_property("cancelCredential"_utf16_fly_string, JS::js_undefined(), JS::default_attributes);
    auto navigation_selection = MUST(select_session());
    auto attacker = URL::Parser::basic_parse("https://other.example/"sv).release_value();
    document->set_origin(attacker.origin());
    EXPECT(fields.capture().is_error());
    document->set_origin(address.origin());
    document->did_stop_being_active_document_in_navigable();
    EXPECT(session->is_closed());
    EXPECT(session->capture(navigation_selection).is_error());
    EXPECT(select_session().is_error());
    traversable->set_active_document(nullptr);
    EXPECT(fields.capture().is_error());
    EXPECT(fields.fill("alice"_string, "must-not-fill-inactive"_string).is_error());
}


TEST_CASE(derpbird_credential_submitted_form_selects_login_and_new_password)
{
    install_test_fonts();
    Web::Bindings::create_a_principal_javascript_realm();
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    auto address = URL::Parser::basic_parse("https://example.com/login"sv).release_value();
    document->set_url(address);
    document->set_origin(address.origin());
    auto& realm = document->window()->principal_realm();
    vm.pop_execution_context();
    auto run = [&](Utf16View source) {
        auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(realm);
        vm.push_execution_context(settings.realm_execution_context());
        ScopeGuard pop = [&] { vm.pop_execution_context(); };
        auto script = JS::Script::parse(source, realm);
        VERIFY(!script.is_error());
        auto result = vm.run(script.value());
        VERIFY(!result.is_error());
        return result.release_value();
    };
    run(R"JS(
        if (!document.documentElement) document.appendChild(document.createElement('html'));
        if (!document.body) document.documentElement.appendChild(document.createElement('body'));
        document.body.innerHTML='<form id="f"><input id="u" autocomplete="section-login username"><input id="p" type="password" autocomplete="current-password"></form><form id="other"></form>';
        globalThis.inputs=[];
        document.getElementById('u').addEventListener('input',e=>inputs.push('u:'+e.isTrusted));
        document.getElementById('p').addEventListener('input',e=>inputs.push('p:'+e.isTrusted));
    )JS"_utf16);
    auto form = [&]() -> Web::HTML::HTMLFormElement& {
        return as<Web::HTML::HTMLFormElement>(*document->get_element_by_id("f"_utf16));
    };
    auto capture = [&] { return Web::DerpBirdCredentialFields::capture_form(form()); };
    EXPECT(capture().is_error()); // Never suggest an empty password.
    run("document.getElementById('u').value='alice'; document.getElementById('p').value='old-fixture'"_utf16);
    auto login = MUST(capture());
    EXPECT_EQ(login.document_id, document->id());
    EXPECT_EQ(login.origin, "https://example.com"sv);
    EXPECT_EQ(login.values.username, "alice"sv);
    EXPECT_EQ(login.values.password, "old-fixture"sv);
    EXPECT(login.kind == Web::DerpBirdCredentialFields::Kind::Login);
    // form= controls need not be descendants. Another form must not participate.
    run(R"JS(
        document.body.appendChild(document.getElementById('p'));
        document.getElementById('p').setAttribute('form','f');
        document.getElementById('other').innerHTML='<input type=password value=unrelated>';
    )JS"_utf16);
    EXPECT_EQ(MUST(capture()).values.password, "old-fixture"sv);
    run(R"JS(
        document.getElementById('f').insertAdjacentHTML('beforeend',
          '<input id=n type=password autocomplete=new-password value=new-fixture><input id=c type=password autocomplete=new-password value=new-fixture>');
    )JS"_utf16);
    auto changed = MUST(capture());
    EXPECT(changed.kind == Web::DerpBirdCredentialFields::Kind::NewPassword);
    EXPECT_EQ(changed.values.password, "new-fixture"sv);
    EXPECT_EQ(changed.values.username, "alice"sv);
    run("document.getElementById('c').value='different'"_utf16);
    EXPECT(capture().is_error());
    run("document.getElementById('c').value='new-fixture'; document.getElementById('u').autocomplete=''"_utf16);
    EXPECT_EQ(MUST(capture()).values.username, "alice"sv); // Unambiguous preceding username.
    run("document.getElementById('n').autocomplete='current-password'"_utf16);
    EXPECT(capture().is_error()); // Two login passwords cannot be guessed.
    run("document.getElementById('n').autocomplete='new-password'; document.getElementById('c').remove(); document.getElementById('n').value=''"_utf16);
    EXPECT(capture().is_error()); // Do not fall back to old password on empty replacement.
    run("document.getElementById('n').remove(); document.getElementById('p').autocomplete='one-time-code'"_utf16);
    EXPECT(capture().is_error());
    run("document.getElementById('p').autocomplete='current-password'; document.getElementById('p').disabled=true"_utf16);
    EXPECT(capture().is_error());
    run("document.getElementById('p').disabled=false; document.getElementById('p').value='é'.repeat(32769)"_utf16);
    EXPECT(capture().is_error());
    run("document.getElementById('p').value='fixture'; document.getElementById('u').value='x'.repeat(1025)"_utf16);
    EXPECT(capture().is_error());
    run("document.getElementById('u').value='alice'; document.getElementById('p').hidden=true"_utf16);
    EXPECT(capture().is_error());
    run("document.getElementById('p').hidden=false; document.getElementById('u').autocomplete='username'; document.getElementById('f').insertAdjacentHTML('beforeend','<input autocomplete=username>')"_utf16);
    EXPECT(capture().is_error());
    // A snapshot is independent of later page mutations, unlike a field ticket.
    run("document.getElementById('p').value='after-submit'"_utf16);
    EXPECT_EQ(login.values.password, "old-fixture"sv);
    run("document.querySelector('#f input:not([id])').remove(); document.getElementById('p').readOnly=true"_utf16);
    EXPECT(capture().is_error());
    run("document.getElementById('p').readOnly=false; document.getElementById('other').innerHTML='<span></span>'.repeat(65536)"_utf16);
    EXPECT(capture().is_error()); // Bound work on a large page, even in unrelated forms.
    run("document.getElementById('other').textContent=''"_utf16);
    EXPECT_EQ(MUST(capture()).values.password, "after-submit"sv);
    run("document.getElementById('f').insertAdjacentHTML('beforeend','<input type=password autocomplete=new-password value=matching>'.repeat(9))"_utf16);
    EXPECT(capture().is_error()); // Bound total confirmation comparisons too.
    auto insecure = URL::Parser::basic_parse("file:///login.html"sv).release_value();
    document->set_url(insecure);
    document->set_origin(insecure.origin());
    EXPECT(capture().is_error());
}


TEST_CASE(derpbird_credential_submission_requires_activation_and_valid_form)
{
    install_test_fonts();
    Web::Bindings::create_a_principal_javascript_realm();
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    auto address = URL::Parser::basic_parse("https://example.com/login"sv).release_value();
    document->set_url(address);
    document->set_origin(address.origin());
    auto& realm = document->window()->principal_realm();
    vm.pop_execution_context();
    auto run = [&](Utf16View source) {
        auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(realm);
        vm.push_execution_context(settings.realm_execution_context());
        ScopeGuard pop = [&] { vm.pop_execution_context(); };
        auto script = JS::Script::parse(source, realm);
        VERIFY(!script.is_error());
        auto result = vm.run(script.value());
        VERIFY(!result.is_error());
        return result.release_value();
    };
    run(R"JS(
        if (!document.documentElement) document.appendChild(document.createElement('html'));
        if (!document.body) document.documentElement.appendChild(document.createElement('body'));
        document.body.innerHTML='<form id="f"><input id="u" autocomplete="section-login username"><input id="p" type="password" autocomplete="current-password"></form><form id="other"></form>';
        globalThis.inputs=[];
        document.getElementById('u').addEventListener('input',e=>inputs.push('u:'+e.isTrusted));
        document.getElementById('p').addEventListener('input',e=>inputs.push('p:'+e.isTrusted));
    )JS"_utf16);
    traversable->set_viewport_size({ 800, 600 });
    client->m_observe_submissions = true;
    run(R"JS(
        globalThis.submits = 0;
        const f=document.getElementById('f');
        f.addEventListener('submit',e=>{
            e.preventDefault(); ++submits;
            document.getElementById('p').value='';
        });
        document.getElementById('u').value='alice';
        document.getElementById('p').value='before-handler';
        f.requestSubmit();
    )JS"_utf16);
    EXPECT(client->m_submissions.is_empty()); // Programmatic submission without user activation.
    EXPECT(run("submits===1 && document.getElementById('p').value===''"_utf16).as_bool());
    document->window()->notify_about_user_activation();
    EXPECT(document->window()->has_transient_activation());
    run("document.getElementById('p').value='before-handler'; document.getElementById('f').requestSubmit()"_utf16);
    EXPECT_EQ(client->m_submissions.size(), 1u);
    if (client->m_submissions.size() != 1) return;
    EXPECT_EQ(client->m_submissions[0].document, document->id());
    EXPECT_EQ(client->m_submissions[0].origin, "https://example.com"sv);
    EXPECT_EQ(client->m_submissions[0].username, "alice"sv);
    EXPECT_EQ(client->m_submissions[0].password, "before-handler"sv);
    EXPECT(run("submits===2 && document.getElementById('p').value===''"_utf16).as_bool());
    // Required unrelated field makes the complete form invalid, despite a password.
    run("document.getElementById('p').value='fixture'; document.getElementById('f').insertAdjacentHTML('beforeend','<input id=required required>'); document.getElementById('f').requestSubmit()"_utf16);
    EXPECT_EQ(client->m_submissions.size(), 1u);
    EXPECT(run("submits===2"_utf16).as_bool());
    run("document.getElementById('required').value='ok'; document.getElementById('p').style.display='none'; document.getElementById('f').requestSubmit()"_utf16);
    EXPECT_EQ(client->m_submissions.size(), 1u); // Real layout visibility, not only hidden attribute.
    run("document.getElementById('p').style.display=''; document.getElementById('p').value='dialog-fixture'; document.getElementById('f').method='dialog'; document.getElementById('f').requestSubmit()"_utf16);
    EXPECT_EQ(client->m_submissions.size(), 1u);
    client->m_observe_submissions = false;
    run("document.getElementById('f').method='post'; document.getElementById('p').value='opt-out'; document.getElementById('f').requestSubmit()"_utf16);
    EXPECT_EQ(client->m_submissions.size(), 1u);
    client->m_observe_submissions = true;
    document->window()->consume_user_activation();
    run("document.getElementById('p').value='expired'; document.getElementById('f').requestSubmit()"_utf16);
    EXPECT_EQ(client->m_submissions.size(), 1u);
    document->window()->notify_about_user_activation();
    run(R"JS(
        document.getElementById('p').value='removed-form';
        document.getElementById('f').addEventListener('submit',e=>e.target.remove(),{once:true});
        document.getElementById('f').requestSubmit();
    )JS"_utf16);
    EXPECT_EQ(client->m_submissions.size(), 2u);
    if (client->m_submissions.size() == 2)
        EXPECT_EQ(client->m_submissions[1].password, "removed-form"sv);
    EXPECT(run("!document.getElementById('f')"_utf16).as_bool());
    // submit() bypasses validation/submit event by specification. Removing the
    // form during formdata prevents navigation while exercising the real path.
    run(R"JS(
        document.body.innerHTML='<form id=f><input autocomplete=username value=bob><input type=password value=direct-fixture><input required></form>';
        const direct=document.getElementById('f');
        direct.addEventListener('submit',()=>{throw new Error('unexpected submit event')});
        direct.addEventListener('formdata',()=>direct.remove(),{once:true});
        direct.submit();
    )JS"_utf16);
    EXPECT_EQ(client->m_submissions.size(), 3u);
    if (client->m_submissions.size() == 3) {
        EXPECT_EQ(client->m_submissions[2].username, "bob"sv);
        EXPECT_EQ(client->m_submissions[2].password, "direct-fixture"sv);
    }
}

TEST_CASE(derpbird_add_style_before_document_element_and_after_revocation)
{
    install_test_fonts();
    Web::Bindings::create_a_principal_javascript_realm();
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    document->set_url(*URL::Parser::basic_parse("https://example.com/early-style"sv));
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
    auto records = MUST(JsonValue::from_string(R"JSON([{
        "id":"early-style", "name":"Early style", "enabled":true, "fileAccessGranted":false,
        "source":"const style=GM_addStyle(':root { --early-style: active; }'); if(!(style instanceof HTMLStyleElement)||style.isConnected)throw new Error('expected detached style');",
        "metadata":{"runAt":"document-start","grantNone":false,"noFrames":false,
        "matches":["https://example.com/*"],"includes":[],"excludeMatches":[],"excludes":[],
        "grants":["GM_addStyle"],"requires":[],"connects":[],"resources":[]}
    }])JSON"sv));
    Web::DerpBirdScriptRegistry registry;
    MUST(registry.replace(records.as_array()));
    for (bool revoke : { false, true }) {
        page_run("while(document.firstChild)document.firstChild.remove();"_utf16);
        auto session = TRY_OR_FAIL(Web::DerpBirdDocumentScripts::create(*document, registry));
        MUST(session->dispatch(Web::DerpBirdScriptRunAt::DocumentStart));
        EXPECT(session->entries()[0].status == Web::DerpBirdDocumentScripts::Status::Executed);
        if (revoke)
            session->close();
        page_run("document.appendChild(document.createElement('html'));"_utf16);
        Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
        EXPECT_EQ(page_run("document.getElementsByTagName('style').length"_utf16).as_i32(), revoke ? 0 : 1);
        EXPECT_EQ(page_run("getComputedStyle(document.documentElement).getPropertyValue('--early-style').trim()==='active'"_utf16).as_bool(), !revoke);
        EXPECT(page_run("typeof GM_addStyle==='undefined'"_utf16).as_bool());
        session->close();
    }
}

TEST_CASE(derpbird_raw_world_uses_page_objects_without_publishing_capabilities)
{
    install_test_fonts();
    auto principal = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal.ptr());
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    vm.pop_execution_context();
    auto world = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    size_t requests = 0;
    world->install_values("raw-fixture"_string, { "GM.getValue"_string },
        [&](ByteBuffer, Web::DerpBirdScriptValues::Completion) -> ErrorOr<void> {
            ++requests;
            return Error::from_string_literal("Fixture transport denied");
        }, true);
    world->install_element_insertion(Vector<String> { "GM_addElement"_string }, true);
    auto result = TRY_OR_FAIL(world->run_raw(R"JS((() => {
        const script = document.createElement('script');
        __derpbirdConfigureElement(script, 'src', 'https://example.com/raw.js');
        __derpbirdConfigureElement(script, 'textContent', 'window.rawInjected = true;');
        if (script.getAttribute('src') !== 'https://example.com/raw.js'
            || script.textContent !== 'window.rawInjected = true;') return false;
        if (typeof globalThis.__derpbirdConfigureElement !== 'undefined'
            || typeof globalThis.__derpbirdInsertElement !== 'undefined') return false;
        globalThis.rawFixtureObject = { marker: 42 };
        const blob = new Blob(['raw']);
        const promise = __derpbirdGM.getValue('key').catch(() => {});
        return rawFixtureObject === window.rawFixtureObject && blob instanceof Blob
            && promise instanceof Promise && typeof globalThis.GM === 'undefined'
            && typeof globalThis.__derpbirdGM === 'undefined';
    })())JS"_utf16));
    EXPECT(!result.is_abrupt());
    if (!result.is_abrupt()) EXPECT(result.value().as_bool());
    EXPECT_EQ(requests, 1u);
    world->close();
    EXPECT(world->run_raw("true"_utf16).is_error());
}

TEST_CASE(derpbird_userscript_insertion_is_grant_and_document_bound)
{
    install_test_fonts();
    auto principal = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal.ptr());
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<TestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };
    vm.pop_execution_context();
    auto world = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    world->install_element_insertion(Vector<String> { "GM_addStyle"_string }, false);
    auto result = TRY_OR_FAIL(world->run(R"JS((() => {
        const parent = document.head || document.documentElement;
        const style = document.createElement('style');
        style.id = 'granted-style';
        __derpbirdConfigureElement(style, 'textContent', 'body { color: red }');
        __derpbirdConfigureElement(style, 'DATA-GRANTED', 'yes');
        if (style.textContent !== 'body { color: red }' || style.getAttribute('data-granted') !== 'yes') return false;
        const returned = __derpbirdInsertElement(style, parent);
        if (returned !== style || style.parentNode !== parent) return false;
        let denied = 0;
        try { __derpbirdInsertElement(document.createElement('script'), parent); } catch { denied++; }
        try { __derpbirdInsertElement(style, parent); } catch { denied++; }
        const other = document.implementation.createHTMLDocument('other');
        try { __derpbirdInsertElement(document.createElement('style'), other.head); } catch { denied++; }
        try { __derpbirdInsertElement(other.createElement('style'), parent); } catch { denied++; }
        try { __derpbirdConfigureElement(style, 'textContent', 'changed'); } catch { denied++; }
        try { __derpbirdConfigureElement(document.createElement('script'), 'src', 'https://example.com/a.js'); } catch { denied++; }
        try { __derpbirdConfigureElement(other.createElement('style'), 'textContent', 'changed'); } catch { denied++; }
        try { __derpbirdConfigureElement(document.createElement('style'), 'bad name', 'x'); } catch { denied++; }
        let converted = false;
        try { __derpbirdConfigureElement(document.createElement('style'), 'textContent', {toString() { converted = true; return ''; }}); } catch { denied++; }
        return denied === 9 && !converted;
    })())JS"_utf16));
    EXPECT(!result.is_abrupt());
    if (!result.is_abrupt()) EXPECT(result.value().to_boolean());
    for (auto operation : { "connect"sv, "attribute"sv, "text"sv, "remove"sv, "adopt"sv }) {
        auto pending = TRY_OR_FAIL(world->run(R"JS((() => {
            globalThis.pendingContainer = document.createElement('div');
            globalThis.pendingStyle = document.createElement('style');
            __derpbirdConfigureElement(pendingStyle, 'textContent', 'body { --pending-style: yes }');
            __derpbirdInsertElement(pendingStyle, pendingContainer);
            return pendingStyle;
        })())JS"_utf16));
        VERIFY(!pending.is_abrupt() && pending.value().is_object());
        auto* element = Web::Bindings::impl_from<Web::DOM::Element>(&pending.value().as_object());
        VERIFY(element && element->derpbird_insertion_pending());
        auto authority = element->derpbird_insertion_authority();
        VERIFY(authority && authority->active());
        Utf16View mutation = operation == "connect"sv ? u"document.documentElement.appendChild(pendingContainer)"sv
            : operation == "attribute"sv ? u"pendingStyle.setAttribute('media', 'screen')"sv
            : operation == "text"sv ? u"pendingStyle.firstChild.data = 'body { --pending-style: changed }'"sv
            : operation == "remove"sv ? u"pendingStyle.remove()"sv
            : u"document.implementation.createHTMLDocument('other').adoptNode(pendingStyle)"sv;
        auto mutated = TRY_OR_FAIL(world->run(mutation));
        VERIFY(!mutated.is_abrupt());
        EXPECT(!element->derpbird_insertion_pending());
        EXPECT(!element->derpbird_insertion_authority());
        EXPECT_EQ(authority->active(), operation == "connect"sv);
    }
    auto ungranted = TRY_OR_FAIL(Web::DerpBirdScriptWorld::create(*document));
    ungranted->install_element_insertion({}, false);
    auto absent = TRY_OR_FAIL(ungranted->run(u"typeof __derpbirdInsertElement === 'undefined' && typeof __derpbirdConfigureElement === 'undefined'"sv));
    EXPECT(!absent.is_abrupt());
    if (!absent.is_abrupt()) EXPECT(absent.value().to_boolean());
    world->close();
    EXPECT(world->run(u"__derpbirdInsertElement(document.createElement('style'), document.head)"sv).is_error());
}

TEST_CASE(derpbird_css_image_cache_separates_page_and_script_authorities)
{
    bool first_active = true;
    auto first = Web::DerpBirdInsertionAuthority::create([&] { return first_active; });
    auto second = Web::DerpBirdInsertionAuthority::create([] { return true; });
    auto url = URL::Parser::basic_parse("https://assets.example/image.png"sv).release_value();
    Web::CSS::ImageStyleValueResourceKey page_key { .url = url, .cross_origin = {}, .integrity = {}, .referrer_policy = {}, .derpbird_insertion_authority = {} };
    auto first_key = page_key;
    first_key.derpbird_insertion_authority = first;
    auto second_key = page_key;
    second_key.derpbird_insertion_authority = second;
    HashMap<Web::CSS::ImageStyleValueResourceKey, int> cache;
    cache.set(page_key, 1);
    cache.set(first_key, 2);
    cache.set(second_key, 3);
    EXPECT_EQ(cache.size(), 3u);
    EXPECT_EQ(cache.get(page_key).value(), 1);
    EXPECT_EQ(cache.get(first_key).value(), 2);
    EXPECT_EQ(cache.get(second_key).value(), 3);
    first_active = false;
    EXPECT(!first_key.derpbird_insertion_authority->active());
    EXPECT(second_key.derpbird_insertion_authority->active());
    EXPECT_EQ(cache.get(page_key).value(), 1);
}

TEST_CASE(derpbird_insertion_identity_expiry_cannot_become_page_authority)
{
    EXPECT(!Web::DerpBirdInsertionAuthority::resolve(0));
    u64 expired_identity;
    {
        auto authority = Web::DerpBirdInsertionAuthority::create([] { return true; });
        expired_identity = authority->identity();
        auto resolved = Web::DerpBirdInsertionAuthority::resolve(expired_identity);
        EXPECT(resolved.ptr() == authority.ptr());
        EXPECT(resolved->active());
    }
    auto expired = Web::DerpBirdInsertionAuthority::resolve(expired_identity);
    VERIFY(expired);
    EXPECT(!expired->active());
    auto next = Web::DerpBirdInsertionAuthority::create([] { return true; });
    EXPECT(next->identity() != expired_identity);
    EXPECT(!Web::DerpBirdInsertionAuthority::resolve(expired_identity)->active());
}


TEST_CASE(derpbird_module_waiters_do_not_cross_insertion_authorities)
{
    auto principal = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal.ptr());
    auto& vm = Web::Bindings::main_thread_vm();
    auto map = GC::Root { vm.heap().allocate<Web::HTML::ModuleMap>() };
    auto url = URL::Parser::basic_parse("https://example.com/shared.mjs"sv).release_value();
    auto first = Web::DerpBirdInsertionAuthority::create([] { return true; });
    auto second = Web::DerpBirdInsertionAuthority::create([] { return true; });
    struct Calls : RefCounted<Calls> {
        GC_HOLDS_NO_CELL_EDGES;
        size_t value { 0 };
    };
    auto page_calls = make_ref_counted<Calls>();
    auto first_calls = make_ref_counted<Calls>();
    auto second_calls = make_ref_counted<Calls>();
    auto callback = [&](NonnullRefPtr<Calls> calls) {
        return GC::create_function(vm.heap(), GC::captures(move(calls)), [](auto& calls, GC::Ptr<Web::HTML::ModuleScript> script) {
            EXPECT(!script);
            ++calls->value;
        });
    };
    map->set(url, u"javascript-or-wasm"sv, { callback(page_calls) });
    map->set(url, u"javascript-or-wasm"sv, { callback(first_calls) }, first->identity());
    map->set(url, u"javascript-or-wasm"sv, { callback(second_calls) }, second->identity());
    map->append(url, u"javascript-or-wasm"sv, callback(first_calls), first->identity());
    first->revoke();
    map->complete_fetch(url, u"javascript-or-wasm"sv, nullptr, first->identity());
    EXPECT_EQ(first_calls->value, 2u);
    EXPECT_EQ(page_calls->value, 0u);
    EXPECT_EQ(second_calls->value, 0u);
    EXPECT(map->get(url, u"javascript-or-wasm"sv).has_value());
    EXPECT(map->get(url, u"javascript-or-wasm"sv, second->identity()).has_value());
    EXPECT(!map->get(url, u"javascript-or-wasm"sv, first->identity()).has_value());
    map->complete_fetch(url, u"javascript-or-wasm"sv, nullptr);
    map->complete_fetch(url, u"javascript-or-wasm"sv, nullptr, second->identity());
    EXPECT_EQ(page_calls->value, 1u);
    EXPECT_EQ(second_calls->value, 1u);
    vm.pop_execution_context();
}
