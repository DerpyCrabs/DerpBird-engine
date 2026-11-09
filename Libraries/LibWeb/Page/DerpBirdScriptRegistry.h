#pragma once

#include <AK/HashTable.h>
#include <AK/Function.h>
#include <LibWeb/Page/DerpBirdScriptInfo.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Utf16String.h>
#include <LibWeb/Page/DerpBirdScriptURLPolicy.h>
#include <LibWeb/Page/DerpBirdScriptPhase.h>

namespace Web {

inline u8 derpbird_script_contexts(JsonObject const& metadata)
{
    auto entries = metadata.get_array("entries"sv);
    if (!entries.has_value()) return 3;
    bool specified = false;
    u8 contexts = 0;
    for (auto const& raw : entries->values()) {
        if (!raw.is_object() || raw.as_object().get_string("key"sv) != "run-in"sv) continue;
        specified = true;
        auto value = raw.as_object().get_string("value"sv);
        if (value == "normal-tabs"sv) contexts |= 1;
        if (value == "incognito-tabs"sv) contexts |= 2;
        // Container-specific scripts never fall back to an unrelated context.
    }
    return specified ? contexts : 3;
}

inline JsonValue derpbird_scripts_for_context(JsonValue snapshot, bool is_private)
{
    // Registry snapshots are arrays, just like DerpBirdScriptRegistry::replace.
    // Preserve invalid input for the decoder to reject rather than asserting here.
    if (!snapshot.is_array()) return snapshot;
    JsonArray selected;
    for (auto const& item : snapshot.as_array().values()) {
        if (!item.is_object()) return snapshot;
        auto metadata = item.as_object().get_object("metadata"sv);
        if (metadata.has_value() && (derpbird_script_contexts(*metadata) & (is_private ? 2 : 1))) {
            auto scoped = item.as_object();
            scoped.set("derpbirdPrivateContext"sv, is_private);
            selected.must_append(move(scoped));
        }
    }
    return JsonValue { move(selected) };
}

struct DerpBirdScriptDefinition {
    String id;
    String name;
    Utf16String source;
    DerpBirdScriptRunAt run_at;
    DerpBirdScriptURLPolicy url_policy;
    bool enabled;
    bool grant_none;
    // This field is supplied by the trusted profile service, never by metadata.
    bool file_access_granted;
    Vector<String> grants;
    Vector<String> required_scripts;
    Vector<Utf16String> required_sources;
    Vector<String> connects;
    struct Resource { String name; String url; };
    Vector<Resource> resources;
    JsonObject resource_values;
    JsonObject info;
    String configuration;
    u64 generation { 0 };
    u8 run_contexts { 3 };
    String sandbox;
    bool has_web_request_metadata { false };
    bool runs_in(bool is_private) const { return run_contexts & (is_private ? 2 : 1); }

    static ErrorOr<DerpBirdScriptDefinition> decode(JsonObject const& object)
    {
        auto id = object.get_string("id"sv);
        auto name = object.get_string("name"sv);
        auto source = object.get_string("source"sv);
        auto enabled = object.get_bool("enabled"sv);
        auto file_access = object.get_bool("fileAccessGranted"sv);
        auto metadata = object.get_object("metadata"sv);
        if (!id.has_value() || id->is_empty() || id->bytes().size() > 128
            || !name.has_value() || name->is_empty() || name->bytes().size() > 1024
            || !source.has_value() || source->bytes().size() > 2 * 1024 * 1024
            || !enabled.has_value() || !file_access.has_value() || !metadata.has_value())
            return Error::from_string_literal("Invalid userscript definition");
        for (auto ch : id->bytes_as_string_view()) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                    || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
                return Error::from_string_literal("Invalid userscript identifier");
        }
        auto run_at_text = metadata->get_string("runAt"sv);
        auto grant_none = metadata->get_bool("grantNone"sv);
        auto no_frames = metadata->get_bool("noFrames"sv);
        if (!run_at_text.has_value() || !grant_none.has_value() || !no_frames.has_value())
            return Error::from_string_literal("Missing userscript execution metadata");
        DerpBirdScriptRunAt run_at;
        if (*run_at_text == "document-start"sv) run_at = DerpBirdScriptRunAt::DocumentStart;
        else if (*run_at_text == "document-body"sv) run_at = DerpBirdScriptRunAt::DocumentBody;
        else if (*run_at_text == "document-end"sv) run_at = DerpBirdScriptRunAt::DocumentEnd;
        else if (*run_at_text == "document-idle"sv) run_at = DerpBirdScriptRunAt::DocumentIdle;
        else if (*run_at_text == "context-menu"sv) run_at = DerpBirdScriptRunAt::ContextMenu;
        else return Error::from_string_literal("Unknown userscript run-at");
        size_t metadata_bytes = 0;
        size_t directives = 0;
        auto strings = [&](StringView key) -> ErrorOr<Vector<String>> {
            auto array = metadata->get_array(key);
            if (!array.has_value())
                return Error::from_string_literal("Missing userscript metadata array");
            directives += array->size();
            if (directives > 1024)
                return Error::from_string_literal("Too many userscript directives");
            Vector<String> values;
            for (auto const& entry : array->values()) {
                if (!entry.is_string() || entry.as_string().is_empty())
                    return Error::from_string_literal("Invalid userscript metadata value");
                metadata_bytes += entry.as_string().bytes().size();
                if (metadata_bytes > 65536)
                    return Error::from_string_literal("Userscript metadata exceeds 64 KiB");
                values.append(entry.as_string());
            }
            return values;
        };
        auto matches = TRY(strings("matches"sv));
        auto includes = TRY(strings("includes"sv));
        auto exclusions = TRY(strings("excludeMatches"sv));
        auto excludes = TRY(strings("excludes"sv));
        auto grants = TRY(strings("grants"sv));
        auto required_scripts = TRY(strings("requires"sv));
        Vector<Utf16String> required_sources;
        if (auto cached = object.get("requiredSources"sv); cached.has_value()) {
            if (!cached->is_array() || cached->as_array().size() != required_scripts.size())
                return Error::from_string_literal("Incomplete userscript dependencies");
            size_t index = 0;
            for (auto const& value : cached->as_array().values()) {
                if (!value.is_object())
                    return Error::from_string_literal("Invalid userscript dependency");
                auto url = value.as_object().get_string("url"sv);
                auto body = value.as_object().get_string("source"sv);
                if (!url.has_value() || *url != required_scripts[index++] || !body.has_value()
                    || body->bytes().size() > 2 * 1024 * 1024)
                    return Error::from_string_literal("Invalid userscript dependency source");
                required_sources.append(Utf16String::from_utf8(*body));
            }
        }
        auto connects = TRY(strings("connects"sv));
        if (*grant_none && !grants.is_empty())
            return Error::from_string_literal("Grant none conflicts with requested capabilities");
        auto resource_array = metadata->get_array("resources"sv);
        if (!resource_array.has_value() || directives + resource_array->size() > 1024)
            return Error::from_string_literal("Invalid userscript resources");
        Vector<Resource> resources;
    JsonObject info;
        HashTable<String> resource_names;
        for (auto const& resource : resource_array->values()) {
            if (!resource.is_object())
                return Error::from_string_literal("Invalid userscript resource");
            auto resource_name = resource.as_object().get_string("name"sv);
            auto resource_url = resource.as_object().get_string("url"sv);
            if (!resource_name.has_value() || resource_name->is_empty() || !resource_url.has_value() || resource_url->is_empty()
                || resource_names.contains(*resource_name))
                return Error::from_string_literal("Invalid or duplicate userscript resource");
            metadata_bytes += resource_name->bytes().size() + resource_url->bytes().size();
            if (metadata_bytes > 65536)
                return Error::from_string_literal("Userscript metadata exceeds 64 KiB");
            resource_names.set(*resource_name);
            resources.append({ *resource_name, *resource_url });
        }
        JsonObject resource_values;
        if (auto cached = object.get("cachedResources"sv); cached.has_value()) {
            if (!cached->is_array() || cached->as_array().size() != resources.size())
                return Error::from_string_literal("Incomplete userscript resources");
            size_t index = 0;
            for (auto const& value : cached->as_array().values()) {
                if (!value.is_object()) return Error::from_string_literal("Invalid userscript resource cache");
                auto const& resource = resources[index++];
                auto name = value.as_object().get_string("name"sv);
                auto url = value.as_object().get_string("url"sv);
                auto text = value.as_object().get_string("text"sv);
                auto data_url = value.as_object().get_string("dataUrl"sv);
                if (name != resource.name || url != resource.url || !text.has_value() || !data_url.has_value()
                    || text->bytes().size() > 6 * 1024 * 1024 || data_url->bytes().size() > 3 * 1024 * 1024
                    || !data_url->bytes_as_string_view().starts_with("data:"sv))
                    return Error::from_string_literal("Invalid userscript resource cache");
                JsonObject entry;
                entry.set("text"sv, *text);
                entry.set("url"sv, *data_url);
                resource_values.set(*name, move(entry));
            }
        }
        String sandbox;
        bool has_web_request_metadata = false;
        if (auto entries = metadata->get_array("entries"sv); entries.has_value()) {
            for (auto const& raw : entries->values()) {
                if (raw.is_object() && raw.as_object().get_string("key"sv) == "webRequest"sv) has_web_request_metadata = true;
                if (!raw.is_object() || raw.as_object().get_string("key"sv) != "sandbox"sv) continue;
                auto mode = raw.as_object().get_string("value"sv);
                if (!mode.has_value() || !mode->is_one_of("raw"sv, "DOM"sv, "JavaScript"sv) || !sandbox.is_empty())
                    return Error::from_string_literal("Invalid or duplicate userscript sandbox mode");
                sandbox = *mode;
            }
        }
        if (sandbox.is_empty()) sandbox = "raw"_string;
        auto policy = TRY(DerpBirdScriptURLPolicy::create(matches, exclusions, *no_frames, includes, excludes, run_at == DerpBirdScriptRunAt::ContextMenu));
        return DerpBirdScriptDefinition {
            *id, *name, Utf16String::from_utf8(*source), run_at, move(policy), *enabled, *grant_none, *file_access,
            move(grants), move(required_scripts), move(required_sources), move(connects), move(resources), move(resource_values), TRY(create_derpbird_script_info(*metadata, *name, object)), object.serialized(), 0, derpbird_script_contexts(*metadata), move(sandbox), has_web_request_metadata
        };
    }
};

// Replacement is atomic: malformed snapshots cannot partially install scripts
// or remove previously installed restrictions. Only the native profile service
// may supply this normalized representation; it is not a content-page API.
class DerpBirdScriptRegistry {
public:
    ErrorOr<void> replace(JsonValue const& snapshot)
    {
        if (!snapshot.is_array() || snapshot.as_array().size() > 256)
            return Error::from_string_literal("Invalid userscript registry snapshot");
        Vector<DerpBirdScriptDefinition> definitions;
        HashTable<String> ids;
        size_t source_bytes = 0;
        auto next_generation = m_next_generation;
        for (auto const& value : snapshot.as_array().values()) {
            if (!value.is_object())
                return Error::from_string_literal("Invalid userscript registry item");
            auto definition = TRY(DerpBirdScriptDefinition::decode(value.as_object()));
            if (ids.contains(definition.id))
                return Error::from_string_literal("Duplicate userscript identifier");
            source_bytes += value.as_object().get_string("source"sv)->bytes().size();
            if (auto cached = value.as_object().get_array("requiredSources"sv); cached.has_value()) {
                for (auto const& dependency : cached->values())
                    source_bytes += dependency.as_object().get_string("source"sv)->bytes().size();
            }
            if (auto cached = value.as_object().get_array("cachedResources"sv); cached.has_value()) {
                for (auto const& resource : cached->values())
                    source_bytes += resource.as_object().get_string("text"sv)->bytes().size() + resource.as_object().get_string("dataUrl"sv)->bytes().size();
            }
            if (source_bytes > 8 * 1024 * 1024)
                return Error::from_string_literal("Userscript registry exceeds 8 MiB");
            for (auto const& old : m_definitions) {
                if (old.id == definition.id && old.configuration == definition.configuration) {
                    definition.generation = old.generation;
                    break;
                }
            }
            if (!definition.generation) {
                if (next_generation == NumericLimits<u64>::max())
                    return Error::from_string_literal("Userscript generation exhausted");
                definition.generation = next_generation++;
            }
            ids.set(definition.id);
            definitions.append(move(definition));
        }
        m_definitions = move(definitions);
        m_next_generation = next_generation;
        if (on_replace)
            on_replace();
        return {};
    }

    Function<void()> on_replace;

    Vector<DerpBirdScriptDefinition> const& definitions() const { return m_definitions; }

private:
    Vector<DerpBirdScriptDefinition> m_definitions;
    u64 m_next_generation { 1 };
};

}
