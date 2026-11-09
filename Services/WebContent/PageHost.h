/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Page/DerpBirdValueRequests.h>
#include <LibWeb/Page/DerpBirdCredentialSession.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <LibGC/Root.h>
#include <LibWeb/Page/DerpBirdDocumentScripts.h>
#include <LibWeb/Page/DerpBirdIsolatedScriptlets.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/VisibilityState.h>
#include <LibWebCommon/Page/PageId.h>
#include <WebContent/Forward.h>

namespace Web {

namespace Compositor {

class CompositorHost;

}

}

namespace WebContent {

class PageHost : public Weakable<PageHost> {
    AK_MAKE_NONCOPYABLE(PageHost);
    AK_MAKE_NONMOVABLE(PageHost);

public:
    AK_ALLOC_WITH_KMALLOC;

    ErrorOr<JsonValue> select_derpbird_credential(Web::PageId, Web::HTML::CrossProcessId);
    ErrorOr<void> fill_derpbird_credential(Web::PageId, Web::HTML::CrossProcessId, u64, String const&, String const&, String const&);
    ErrorOr<JsonValue> capture_derpbird_credential(Web::PageId, Web::HTML::CrossProcessId, u64, String const&);
    void cancel_derpbird_credential(Web::PageId, Web::HTML::CrossProcessId, u64);
    bool receive_derpbird_value_event(Web::PageId, Web::HTML::CrossProcessId, ReadonlyBytes);
    Web::DerpBirdValueRequests& value_requests() { return *m_value_requests; }
    ErrorOr<u64> request_derpbird_values(Web::PageId, Web::HTML::CrossProcessId, ByteBuffer, Web::DerpBirdValueRequests::Completion);

    static NonnullOwnPtr<PageHost> create(ConnectionFromClient& client) { return adopt_own(*new PageHost(client)); }
    virtual ~PageHost();

    void initialize(Web::PageId initial_page_id, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables, Web::HTML::CrossProcessId root_navigable_id, Web::HTML::CrossProcessIdAllocator, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::CrossProcessId initial_document_id, Web::HTML::VisibilityState system_visibility_state);
    Optional<PageClient&> page(Web::PageId page_id);
    PageClient& create_page(Web::PageId page_id, Optional<Web::HTML::CrossProcessId> pending_root_navigable_id = {});
    void remove_page(Badge<PageClient>, Web::PageId page_id);
    Web::HTML::CrossProcessId allocate_cross_process_id();
    Web::HTML::CrossProcessId allocate_navigable_id();
    void for_each_page(Function<void(PageClient&)>);

    ConnectionFromClient& client() const { return m_client; }
    void ensure_compositor_host();
    void compositor_process_reconnected();
    void compositor_process_lost();
    void invalidate_user_style();
    ErrorOr<void> set_derpbird_user_scripts(JsonValue const& snapshot) { return m_script_registry.replace(snapshot); }
    void invoke_derpbird_context_script(Web::PageId, Web::HTML::CrossProcessId, String const&, String const&);
    void derpbird_document_phase(Web::PageId, Web::DOM::Document&, Web::DerpBirdScriptRunAt);
    void derpbird_document_destroyed(Web::PageId, Web::DOM::Document&);
    Web::Compositor::CompositorHost* compositor_host() { return m_compositor_host.ptr(); }
    Web::Compositor::CompositorHost const* compositor_host() const { return m_compositor_host.ptr(); }

private:
    explicit PageHost(ConnectionFromClient&);

    struct CredentialSelection {
        NonnullRefPtr<Web::DerpBirdCredentialSession> session;
        Web::DerpBirdCredentialSession::Selection selection;
        NonnullRefPtr<Core::Timer> timer;
    };
    HashMap<Web::PageId, CredentialSelection> m_credential_selections;
    NonnullRefPtr<Web::DerpBirdValueRequests> m_value_requests { Web::DerpBirdValueRequests::create() };
    ConnectionFromClient& m_client;
    OwnPtr<Web::Compositor::CompositorHost> m_compositor_host;
    HashMap<Web::PageId, GC::Root<PageClient>> m_pages;
    Optional<Web::HTML::CrossProcessIdAllocator> m_cross_process_id_allocator;
    Web::DerpBirdScriptRegistry m_script_registry;
    HashMap<Web::PageId, HashMap<Web::HTML::CrossProcessId, NonnullRefPtr<Web::DerpBirdDocumentScripts>>> m_script_sessions;
    HashMap<Web::PageId, HashMap<Web::HTML::CrossProcessId, NonnullRefPtr<Web::DerpBirdIsolatedScriptlets>>> m_scriptlet_sessions;
};

}
