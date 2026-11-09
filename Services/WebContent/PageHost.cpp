/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>

#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/PageHost.h>
#include <WebContent/WebContentCompositorHost.h>
#include <WebContent/WebDriverConnection.h>

namespace WebContent {

PageHost::PageHost(ConnectionFromClient& client)
    : m_client(client)
{
    m_script_registry.on_replace = [this] {
        for (auto& page_sessions : m_script_sessions) {
            for (auto& session : page_sessions.value)
                session.value->reconcile(m_script_registry);
        }
    };
}

void PageHost::initialize(Web::PageId initial_page_id, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables, Web::HTML::CrossProcessId root_navigable_id, Web::HTML::CrossProcessIdAllocator cross_process_id_allocator, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::CrossProcessId initial_document_id, Web::HTML::VisibilityState system_visibility_state)
{
    VERIFY(m_pages.is_empty());
    m_cross_process_id_allocator = cross_process_id_allocator;

    // The process displays a tab, whose traversable takes the id the UI process gave it, or hosts an isolated iframe
    // of a tab whose graph it is given whole.
    if (remote_navigables.is_empty()) {
        auto& first_page = create_page(initial_page_id, root_navigable_id);
        first_page.page().set_system_visibility_state(system_visibility_state);
        Web::HTML::LocalTraversableNavigable::create_a_fresh_top_level_traversable(first_page.page(), move(initial_history_entry), initial_document_id);
        return;
    }
    auto& first_page = create_page(initial_page_id);
    first_page.page().set_system_visibility_state(system_visibility_state);
    first_page.page().create_remote_navigable_graph(move(remote_navigables));
    first_page.page().begin_hosting(root_navigable_id, initial_history_entry);
}

PageClient& PageHost::create_page(Web::PageId page_id, Optional<Web::HTML::CrossProcessId> pending_root_navigable_id)
{
    VERIFY(page_id > 0);
    VERIFY(!m_pages.contains(page_id));
    m_pages.set(page_id, PageClient::create(*this, page_id, pending_root_navigable_id));
    return *m_pages.get(page_id).value();
}

Web::HTML::CrossProcessId PageHost::allocate_cross_process_id()
{
    VERIFY(m_cross_process_id_allocator.has_value());
    return m_cross_process_id_allocator->allocate();
}

Web::HTML::CrossProcessId PageHost::allocate_navigable_id()
{
    return allocate_cross_process_id();
}

void PageHost::remove_page(Badge<PageClient>, Web::PageId page_id)
{
    // The cursor and hovered link of the page ask nothing more of the connection, which a hover of the render clock may
    // still ask it of.
    if (auto page = m_pages.get(page_id); page.has_value()) {
        page.value()->page().cursor().set_send({});
        page.value()->page().hover_link().set_send({});
    }
    if (auto entry = m_credential_selections.take(page_id); entry.has_value()) {
        entry->timer->stop();
        entry->session->close();
    }
    m_scriptlet_sessions.remove(page_id);
    m_script_sessions.remove(page_id);
    m_pages.remove(page_id);
}

ErrorOr<JsonValue> PageHost::select_derpbird_credential(Web::PageId page_id, Web::HTML::CrossProcessId document_id)
{
    if (auto old = m_credential_selections.take(page_id); old.has_value()) {
        old->timer->stop();
        old->session->close();
    }
    auto target = page(page_id);
    if (!target.has_value()) return Error::from_string_literal("Credential page unavailable");
    auto navigable = target->page().hosted_focused_navigable();
    auto document = navigable ? navigable->active_document() : nullptr;
    if (!document || document->id() != document_id || !document->is_fully_active())
        return Error::from_string_literal("Credential document changed");
    auto element = document->focused_area();
    if (!element || !is<Web::HTML::HTMLInputElement>(*element))
        return Error::from_string_literal("Focus a password field first");
    auto session = Web::DerpBirdCredentialSession::create(*document);
    auto selection = TRY(session->select(as<Web::HTML::HTMLInputElement>(*element), true));
    JsonObject result;
    result.set("ticket"sv, String::number(selection.ticket));
    result.set("origin"sv, selection.origin);
    result.set("kind"sv, selection.kind == Web::DerpBirdCredentialFields::Kind::Login ? "login"sv : "new-password"sv);
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [weak = make_weak_ptr(), page_id, document_id, ticket = selection.ticket] {
        if (weak) weak->cancel_derpbird_credential(page_id, document_id, ticket);
    });
    timer->start();
    m_credential_selections.set(page_id, { move(session), move(selection), move(timer) });
    return JsonValue { move(result) };
}

ErrorOr<void> PageHost::fill_derpbird_credential(Web::PageId page_id, Web::HTML::CrossProcessId document_id, u64 ticket, String const& origin, String const& username, String const& password)
{
    auto found = m_credential_selections.find(page_id);
    if (found == m_credential_selections.end() || found->value.selection.document != document_id || found->value.selection.ticket != ticket)
        return Error::from_string_literal("Credential selection changed");
    // Keep the entry discoverable while page callbacks run. A new selection,
    // explicit cancellation or page removal can then revoke this operation.
    auto session = found->value.session;
    auto selection = found->value.selection;
    found->value.timer->stop();
    ScopeGuard close = [&] { cancel_derpbird_credential(page_id, document_id, ticket); };
    if (selection.origin != origin || username.bytes().size() > 1024 || password.bytes().size() > 65536)
        return Error::from_string_literal("Credential does not match selection");
    return session->fill(selection, username, password);
}

ErrorOr<JsonValue> PageHost::capture_derpbird_credential(Web::PageId page_id, Web::HTML::CrossProcessId document_id, u64 ticket, String const& origin)
{
    auto found = m_credential_selections.find(page_id);
    if (found == m_credential_selections.end() || found->value.selection.document != document_id || found->value.selection.ticket != ticket)
        return Error::from_string_literal("Credential selection changed");
    auto entry = m_credential_selections.take(page_id).release_value();
    entry.timer->stop();
    ScopeGuard close = [&] { entry.session->close(); };
    if (entry.selection.origin != origin)
        return Error::from_string_literal("Credential origin changed");
    auto values = TRY(entry.session->capture(entry.selection));
    JsonObject result;
    result.set("origin"sv, entry.selection.origin);
    result.set("username"sv, move(values.username));
    result.set("password"sv, move(values.password));
    // Native browser recipient only; do not forward this payload to WebUI.
    return JsonValue { move(result) };
}

void PageHost::cancel_derpbird_credential(Web::PageId page_id, Web::HTML::CrossProcessId document_id, u64 ticket)
{
    auto found = m_credential_selections.find(page_id);
    if (found == m_credential_selections.end() || found->value.selection.document != document_id || found->value.selection.ticket != ticket) return;
    auto entry = m_credential_selections.take(page_id).release_value();
    entry.timer->stop();
    entry.session->close();
}

void PageHost::for_each_page(Function<void(PageClient&)> callback)
{
    for (auto& page : m_pages)
        callback(*page.value);
}

Optional<PageClient&> PageHost::page(Web::PageId page_id)
{
    return m_pages.get(page_id).map([](auto& value) -> PageClient& {
        return *value;
    });
}

PageHost::~PageHost()
{
    m_scriptlet_sessions.clear();
    m_script_sessions.clear();
    // The pages' cursors and hovered links ask nothing more of the connection, which goes away with this.
    for (auto& page : m_pages) {
        page.value->page().cursor().set_send({});
        page.value->page().hover_link().set_send({});
    }
}

bool PageHost::receive_derpbird_value_event(Web::PageId page_id, Web::HTML::CrossProcessId document_id, ReadonlyBytes payload)
{
    if (payload.size() > 2 * 1024 * 1024 + 4096) return false;
    auto page = m_script_sessions.find(page_id);
    if (page == m_script_sessions.end()) return false;
    auto session = page->value.find(document_id);
    if (session == page->value.end()) return false;
    auto owner = session->value;
    return owner->receive_value_event(payload);
}

ErrorOr<u64> PageHost::request_derpbird_values(Web::PageId page_id, Web::HTML::CrossProcessId document_id, ByteBuffer payload, Web::DerpBirdValueRequests::Completion completion)
{
    if (payload.size() > Web::DerpBirdValueRequests::max_payload_bytes)
        return Error::from_string_literal("Userscript storage request exceeds limits");
    auto sessions = m_script_sessions.find(page_id);
    if (sessions == m_script_sessions.end())
        return Error::from_string_literal("Userscript document is unavailable");
    auto session = sessions->value.find(document_id);
    if (session == sessions->value.end() || session->value->is_closed())
        return Error::from_string_literal("Userscript document is inactive");
    session->value->on_close = [weak = m_value_requests->make_weak_ptr(), page_id, document_id] {
        if (weak) weak->cancel_document(page_id, document_id);
    };
    auto decoded = TRY(Web::DerpBirdValueProtocol::decode_request(payload.bytes()));
    using Method = Web::DerpBirdValueProtocol::Method;
    bool network = decoded.method == Method::Network || decoded.method == Method::NetworkLegacy || decoded.method == Method::NetworkRead;
    // The provider owns details.timeout and cancellation. A storage RPC timer
    // must not cut off slow headers, long responses or an idle streaming read.
    auto request = TRY(m_value_requests->add(page_id, document_id, move(completion), !network));
    m_client.async_did_request_derpbird_values(page_id, document_id, request, payload.bytes());
    return request;
}

void PageHost::ensure_compositor_host()
{
    if (m_compositor_host)
        return;
    m_compositor_host = create_web_content_compositor_host(m_client);
}

void PageHost::compositor_process_reconnected()
{
    if (m_compositor_host)
        m_compositor_host->register_animated_images_with_new_compositor();
    for (auto& [_, page] : m_pages)
        page->compositor_process_reconnected();
}

void PageHost::invalidate_user_style()
{
    for (auto& [_, page] : m_pages)
        page->page().invalidate_user_style();
}

void PageHost::compositor_process_lost()
{
    for (auto& [_, page] : m_pages)
        page->compositor_process_lost();
}

void PageHost::invoke_derpbird_context_script(Web::PageId page_id, Web::HTML::CrossProcessId document_id, String const& script_id, String const& configuration)
{
    auto sessions = m_script_sessions.find(page_id);
    if (sessions == m_script_sessions.end()) return;
    auto found = sessions->value.find(document_id);
    if (found == sessions->value.end()) return;
    u64 generation = 0;
    for (auto const& definition : m_script_registry.definitions()) {
        if (definition.id == script_id && definition.enabled && definition.configuration == configuration)
            generation = definition.generation;
    }
    if (!generation) return;
    auto session = found->value; // Running script can detach its document.
    auto result = session->dispatch(Web::DerpBirdScriptRunAt::ContextMenu, script_id, generation);
    if (result.is_error()) warnln("Unable to run context-menu script: {}", result.error());
}

void PageHost::derpbird_document_phase(Web::PageId page_id, Web::DOM::Document& document, Web::DerpBirdScriptRunAt phase)
{
    if (!document.is_fully_active())
        return;
    if (phase == Web::DerpBirdScriptRunAt::DocumentStart) {
        if (!document.is_decoded_svg() && document.content_blocking_enabled() && !document.origin().is_opaque()) {
            auto& blockers = m_scriptlet_sessions.ensure(page_id);
            if (!blockers.contains(document.id())) {
                auto source = Web::ContentBlocker::the().scriptlets_for_url(document.url(), true);
                if (!source.is_empty()) {
                    auto created = Web::DerpBirdIsolatedScriptlets::create(document);
                    if (!created.is_error()) {
                        auto owner = created.release_value();
                        blockers.set(document.id(), owner);
                        auto result = owner->run(source.utf16_view());
                        if (result.is_error() || !result.value())
                            dbgln("Isolated content blocker scriptlet execution failed");
                    }
                }
            }
        }
        auto& sessions = m_script_sessions.ensure(page_id);
        if (!sessions.contains(document.id())) {
            auto session = Web::DerpBirdDocumentScripts::create(document, m_script_registry,
                [weak = make_weak_ptr(), page_id, document_id = document.id()](ByteBuffer payload, Web::DerpBirdScriptValues::Completion completion) -> ErrorOr<void> {
                    if (!weak)
                        return Error::from_string_literal("Userscript host is closed");
                    TRY(weak->request_derpbird_values(page_id, document_id, move(payload), move(completion)));
                    return {};
                });
            if (session.is_error()) {
                warnln("Unable to select userscripts: {}", session.error());
                return;
            }
            sessions.set(document.id(), session.release_value());
        }
    }
    auto page_sessions = m_script_sessions.find(page_id);
    if (page_sessions == m_script_sessions.end())
        return;
    auto entry = page_sessions->value.find(document.id());
    if (entry == page_sessions->value.end())
        return;
    // Script execution may destroy its document and remove the map entry.
    auto session = entry->value;
    if (session->is_closed())
        return;
    auto result = session->dispatch(phase);
    if (result.is_error())
        warnln("Unable to dispatch userscripts: {}", result.error());
}

void PageHost::derpbird_document_destroyed(Web::PageId page_id, Web::DOM::Document& document)
{
    if (auto sessions = m_scriptlet_sessions.find(page_id); sessions != m_scriptlet_sessions.end())
        sessions->value.remove(document.id());
    auto credential = m_credential_selections.find(page_id);
    if (credential != m_credential_selections.end() && credential->value.selection.document == document.id())
        cancel_derpbird_credential(page_id, document.id(), credential->value.selection.ticket);
    if (auto page_sessions = m_script_sessions.find(page_id); page_sessions != m_script_sessions.end()) {
        if (auto entry = page_sessions->value.find(document.id()); entry != page_sessions->value.end()) {
            entry->value->close();
            page_sessions->value.remove(entry);
        }
    }
}

}
