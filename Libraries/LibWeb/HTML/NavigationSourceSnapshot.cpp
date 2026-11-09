/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/FetchClient.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>

namespace Web::HTML {

NavigationSourceSnapshot create_navigation_source_snapshot(SourceSnapshotParams const& snapshot)
{
    return {
        .has_transient_activation = snapshot.has_transient_activation,
        .sandboxing_flags = snapshot.sandboxing_flags,
        .allows_downloading = snapshot.allows_downloading,
        .fetch_client = snapshot.fetch_client.map([](FetchClient const& fetch_client) {
            return fetch_client.visit(
                [](GC::Ref<EnvironmentSettingsObject> const& settings_object) { return settings_object->serialize(); },
                [](NonnullRefPtr<RemoteEnvironmentSettings const> const& settings) { return settings->settings; });
        }),
        .source_policy_container = snapshot.source_policy_container->serialize(),
        .text_directive_user_activation = snapshot.text_directive_user_activation,
        .browsing_context_group_has_multiple_contexts = snapshot.browsing_context_group_has_multiple_contexts,
        .browsing_context_group_has_remote_contexts = snapshot.browsing_context_group_has_remote_contexts,
        .derpbird_popup_source = snapshot.derpbird_popup_source,
        .derpbird_popup_top = snapshot.derpbird_popup_top,
        .derpbird_popunder = snapshot.derpbird_popunder,
    };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#snapshotting-source-snapshot-params
GC::Ref<SourceSnapshotParams> create_source_snapshot_params_from_navigation_source_snapshot(JS::Realm& realm, NavigationSourceSnapshot const& snapshot)
{
    Optional<FetchClient> fetch_client;
    if (snapshot.fetch_client.has_value())
        fetch_client = RemoteEnvironmentSettings::create(*snapshot.fetch_client);

    auto result = realm.heap().allocate<SourceSnapshotParams>(
        snapshot.has_transient_activation,
        snapshot.sandboxing_flags,
        snapshot.allows_downloading,
        fetch_client,
        create_a_policy_container_from_serialized_policy_container(snapshot.source_policy_container),
        snapshot.text_directive_user_activation,
        snapshot.browsing_context_group_has_multiple_contexts,
        snapshot.browsing_context_group_has_remote_contexts);
    result->derpbird_popup_source = snapshot.derpbird_popup_source;
    result->derpbird_popup_top = snapshot.derpbird_popup_top;
    result->derpbird_popunder = snapshot.derpbird_popunder;
    return result;
}

}
