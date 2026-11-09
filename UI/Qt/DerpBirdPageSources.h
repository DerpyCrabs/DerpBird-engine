#pragma once
#include "DerpBirdScriptStore.h"
#include "DerpBirdStyleStore.h"

namespace Ladybird {
// Use the same native matchers as document injection, including exclusions and
// regex budgets. Disabled matches remain visible so they can be enabled again.
inline ErrorOr<JsonObject> derpbird_page_sources(URL::URL const& url, String tab_id, bool is_private, DerpBirdStyleStore const& styles, DerpBirdScriptStore const& scripts)
{
    JsonArray style_rows;
    Web::DerpBirdStyleMatchBudget style_budget;
    for (auto const& raw : styles.items().values()) {
        auto const& item = raw.as_object();
        auto sections = TRY(Web::DerpBirdStyleSection::decode(JsonValue { *item.get_array("sections"sv) }));
        bool matches = false;
        for (auto const& section : sections) matches |= section.matches(url, style_budget);
        if (!matches) continue;
        JsonObject row;
        row.set("id"sv, *item.get_string("id"sv));
        row.set("name"sv, *item.get_string("name"sv));
        row.set("enabled"sv, *item.get_bool("enabled"sv));
        style_rows.must_append(move(row));
    }
    JsonArray script_rows;
    Web::DerpBirdScriptMatchBudget script_budget;
    for (auto const& raw : scripts.items().values()) {
        auto definition = TRY(Web::DerpBirdScriptDefinition::decode(raw.as_object()));
        if (!definition.runs_in(is_private)) continue;
        auto match = definition.url_policy.evaluate(url, true, definition.file_access_granted, script_budget);
        if (match != Web::DerpBirdScriptEligibility::Allowed) continue;
        JsonObject row;
        row.set("id"sv, definition.id);
        row.set("name"sv, definition.name);
        row.set("enabled"sv, definition.enabled);
        row.set("manual"sv, definition.run_at == Web::DerpBirdScriptRunAt::ContextMenu);
        script_rows.must_append(move(row));
    }
    JsonObject result;
    result.set("tabId"sv, move(tab_id));
    result.set("url"sv, url.serialize());
    result.set("styles"sv, move(style_rows));
    result.set("scripts"sv, move(script_rows));
    result.set("styleRevision"sv, styles.revision());
    result.set("scriptRevision"sv, scripts.revision());
    return result;
}
}
