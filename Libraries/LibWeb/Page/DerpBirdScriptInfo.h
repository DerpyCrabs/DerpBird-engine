#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>

namespace Web {

// Metadata is descriptive only. Never infer capabilities from this object.
// Fields that require unimplemented host services are omitted rather than guessed.
inline ErrorOr<JsonObject> create_derpbird_script_info(JsonObject const& metadata, String const& fallback_name, JsonObject const& installed)
{
    size_t descriptive_bytes = 0;
    auto text = [&](StringView key, String fallback = {}) -> ErrorOr<String> {
        auto value = metadata.get(key);
        if (!value.has_value())
            return fallback;
        if (!value->is_string())
            return Error::from_string_literal("Invalid descriptive userscript metadata");
        auto result = value->as_string();
        descriptive_bytes += result.bytes().size();
        if (descriptive_bytes > 128 * 1024)
            return Error::from_string_literal("Descriptive userscript metadata exceeds 128 KiB");
        return result;
    };
    auto name = TRY(text("name"sv, fallback_name));
    auto name_space = TRY(text("namespace"sv));
    auto version = TRY(text("version"sv));
    auto description = TRY(text("description"sv));
    auto header = TRY(text("header"sv));
    if (header.bytes().size() > 65536)
        return Error::from_string_literal("Userscript header exceeds 64 KiB");
    JsonObject script;
    script.set("name"sv, move(name));
    script.set("namespace"sv, move(name_space));
    script.set("version"sv, move(version));
    script.set("description"sv, move(description));
    script.set("header"sv, header);
    for (auto key : { "matches"sv, "includes"sv, "excludes"sv, "connects"sv })
        script.set(key, metadata.get(key).value());
    JsonArray resources;
    for (auto const& value : metadata.get_array("resources"sv)->values()) {
        JsonObject resource;
        resource.set("name"sv, value.as_object().get("name"sv).value());
        resource.set("url"sv, value.as_object().get("url"sv).value());
        TRY(resources.append(move(resource)));
    }
    script.set("resources"sv, move(resources));
    script.set("excludeMatches"sv, metadata.get("excludeMatches"sv).value());
    auto grant_none = metadata.get_bool("grantNone"sv).value();
    JsonArray none;
    if (grant_none)
        TRY(none.append("none"sv));
    script.set("grant"sv, grant_none ? JsonValue { move(none) } : metadata.get("grants"sv).value());
    script.set("run-at"sv, metadata.get("runAt"sv).value());
    // Report the installed update policy, not untrusted downloaded metadata.
    auto source_url = installed.get_string("sourceURL"sv);
    auto base = source_url.has_value() ? URL::Parser::basic_parse(*source_url) : Optional<URL::URL> {};
    Optional<String> update, download;
    if (auto entries = metadata.get_array("entries"sv); entries.has_value()) {
        for (auto const& raw : entries->values()) {
            if (!raw.is_object()) continue;
            auto key = raw.as_object().get_string("key"sv);
            auto value = raw.as_object().get_string("value"sv);
            if (!value.has_value()) continue;
            if (key == "updateURL"sv && !update.has_value()) update = *value;
            if (key == "downloadURL"sv && !download.has_value()) download = *value;
        }
    }
    auto resolve = [&](Optional<String> const& declared) -> Optional<String> {
        if (!base.has_value() || declared == "none"sv) return {};
        auto url = declared.has_value() ? URL::Parser::basic_parse(*declared, *base) : base;
        if (!url.has_value() || !url->scheme().is_one_of("http"sv, "https"sv)
            || !url->username().is_empty() || !url->password().is_empty()) return {};
        url->set_fragment({});
        return url->serialize();
    };
    auto download_url = resolve(download);
    auto update_url = update.has_value() ? resolve(update) : download_url;
    bool checks_updates = installed.get_bool("checkUpdates"sv).value_or(false);
    bool will_update = checks_updates && update_url.has_value() && download_url.has_value();
    script.set("downloadURL"sv, download_url.has_value() ? JsonValue { *download_url } : JsonValue {});
    script.set("updateURL"sv, update_url.has_value() ? JsonValue { *update_url } : JsonValue {});
    JsonObject options;
    options.set("run_at"sv, metadata.get("runAt"sv).value());
    options.set("noframes"sv, metadata.get("noFrames"sv).value());
    options.set("check_for_updates"sv, checks_updates);
    script.set("options"sv, move(options));
    JsonObject info;
    info.set("script"sv, move(script));
    info.set("scriptHandler"sv, "DerpBird"sv);
    info.set("version"sv, "0.1.0"sv);
    info.set("scriptMetaStr"sv, move(header));
    info.set("scriptWillUpdate"sv, will_update);
    info.set("isIncognito"sv, installed.get_bool("derpbirdPrivateContext"sv).value_or(false));
    info.set("scriptUpdateURL"sv, update_url.has_value() ? JsonValue { *update_url } : JsonValue {});
    info.set("downloadMode"sv, "browser"sv);
    auto mode = "raw"sv;
    if (!grant_none) {
        if (auto entries = metadata.get_array("entries"sv); entries.has_value())
            for (auto const& entry : entries->values()) {
                if (!entry.is_object() || entry.as_object().get_string("key"sv) != "sandbox"sv) continue;
                auto requested = entry.as_object().get_string("value"sv);
                if (requested == "DOM"sv) mode = "dom"sv;
                if (requested == "JavaScript"sv) mode = "js"sv;
                if (requested == "raw"sv) mode = "raw"sv;
            }
    }
    info.set("sandboxMode"sv, mode);
    return info;
}

}
