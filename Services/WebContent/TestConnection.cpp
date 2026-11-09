/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <WebContent/PageHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Compositor/RenderClock.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/TestConnection.h>

namespace WebContent {

TestConnection::TestConnection(NonnullOwnPtr<IPC::Transport> transport, WebContent::ConnectionFromClient& client)
    : IPC::ConnectionFromClient<WebContentTestClientEndpoint, WebContentTestServerEndpoint>(*this, move(transport), 0)
    , m_client(client)
{
}

TestConnection::~TestConnection() = default;

// This transport has its own send queue, so what the main connection queued first has to reach its socket first.
void TestConnection::prepare_to_send()
{
    m_client.transport().flush();
}

void TestConnection::die()
{
}

void TestConnection::reset_state_for_testing(Web::PageId page_id)
{
    m_client.dispatch_pending_messages();

    if (auto page = m_client.page(page_id); page.has_value()) {
        // A test that times out may never restore automatic rendering in its finally block. Both controls outlive
        // the document, so restore them at the test boundary before acknowledging the reset.
        Web::Compositor::RenderClock::the().set_manual_for_testing(false);
        page->page().client().set_manual_rendering_opportunities(false);
        as<Web::HTML::LocalTraversableNavigable>(*page->page().local_traversable()).reset_session_history_for_testing();
        // The navigable outlives the document too, and with it the navigations and history updates the previous test
        // made, so a test that made many of them would leave the next test in this view blocked from making its own.
        page->page().local_traversable()->reset_navigation_or_history_update_rate_limit_for_testing();
        // A test that sets a preferred color scheme may never put it back, and the override outlives the document.
        page->page().set_preferred_color_scheme_override_for_testing({});
        // A test that asks for accessibility updates never stops asking, and the page outlives the document, so every
        // later test in this view would build and serialize the accessibility tree on each change.
        page->page().set_accessibility_interested(false);
        auto active_entry = page->page().local_traversable()->active_session_history_entry();
        VERIFY(active_entry);
        async_did_reset_state_for_testing(page_id, Web::HTML::create_session_history_entry_descriptor(*active_entry));
    }
}

void TestConnection::probe_derpbird_values(Web::PageId page_id, u64 probe_id, bool unknown_document, ByteBuffer payload)
{
    m_client.dispatch_pending_messages();
    auto page = m_client.page(page_id);
    if (!page.has_value()) {
        async_did_probe_derpbird_values(probe_id, false, {});
        return;
    }
    auto document_id = page->page().local_traversable()->active_document()->id();
    auto complete = [weak = make_weak_ptr<TestConnection>(), probe_id](ErrorOr<ByteBuffer> result) {
        if (!weak) return;
        weak->prepare_to_send();
        if (result.is_error()) weak->async_did_probe_derpbird_values(probe_id, false, {});
        else weak->async_did_probe_derpbird_values(probe_id, true, result.value().bytes());
    };
    if (unknown_document) {
        document_id.namespace_id = NumericLimits<u64>::max();
        auto request = m_client.page_host().value_requests().add(page_id, document_id, move(complete));
        if (request.is_error()) async_did_probe_derpbird_values(probe_id, false, {});
        else m_client.async_did_request_derpbird_values(page_id, document_id, request.value(), payload.bytes());
        return;
    }
    auto request = m_client.page_host().request_derpbird_values(page_id, document_id, move(payload), move(complete));
    if (request.is_error()) async_did_probe_derpbird_values(probe_id, false, {});
}

}
