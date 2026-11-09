#pragma once

#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/kmalloc.h>
#include <LibWeb/Page/DerpBirdScriptRegistry.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace Ladybird {

// One instance per profile, shared by windows. Callers publish changes only
// after replace() succeeds; neither validation nor I/O failure changes state.
class DerpBirdScriptStore {
public:
    AK_ALLOC_WITH_KMALLOC;
    explicit DerpBirdScriptStore(QString path)
        : m_path(move(path))
    {
    }

    JsonArray const& items() const { return m_items; }
    u64 revision() const { return m_revision; }

    ErrorOr<void> load()
    {
        QFile file(m_path);
        if (!file.exists())
            return {};
        if (!file.open(QIODevice::ReadOnly))
            return Error::from_string_literal("Cannot read userscripts");
        if (file.size() > max_file_bytes)
            return Error::from_string_literal("Userscript file exceeds 16 MiB");
        auto bytes = file.read(max_file_bytes + 1);
        if (file.error() != QFile::NoError || bytes.size() > max_file_bytes)
            return Error::from_string_literal("Cannot read userscript file");
        auto root = TRY(JsonValue::from_string(StringView(bytes.constData(), bytes.size())));
        if (!root.is_object() || root.as_object().get_integer<int>("version"sv) != 1)
            return Error::from_string_literal("Unsupported userscript file version");
        auto items = root.as_object().get_array("items"sv);
        if (!items.has_value())
            return Error::from_string_literal("Missing userscripts");
        TRY(validate(*items));
        m_items = *items;
        ++m_revision;
        return {};
    }

    // Ordinary editing cannot authorize local-file access. Only the separate
    // permission operation grants it; code/dependency changes revoke it.
    ErrorOr<JsonArray> prepare_from_editor(JsonArray items, u64 expected_revision)
    {
        if (expected_revision != m_revision)
            return Error::from_string_literal("Userscript registry changed; reload before saving");
        // Dependency bodies are native-owned. Never accept executable cache data
        // supplied by the editor; restore it only for unchanged installed code.
        for (auto& item : items.values()) {
            if (!item.is_object())
                return Error::from_string_literal("Invalid userscript record");
            item.as_object().remove("requiredSources"sv);
            item.as_object().remove("cachedResources"sv);
        }
        TRY(validate(items));
        for (auto& item : items.values()) {
            if (!item.is_object())
                return Error::from_string_literal("Invalid userscript record");
            auto& record = item.as_object();
            bool granted = false;
            for (auto const& previous : m_items.values()) {
                auto const& old = previous.as_object();
                if (old.get_string("id"sv) == record.get_string("id"sv)
                    && old.get_string("source"sv) == record.get_string("source"sv)
                    && old.get_string("dependencyRevision"sv) == record.get_string("dependencyRevision"sv)
                    && old.get("metadata"sv)->serialized() == record.get("metadata"sv).value_or(JsonValue {}).serialized()) {
                    granted = old.get_bool("fileAccessGranted"sv).value_or(false);
                    if (auto cache = old.get("requiredSources"sv); cache.has_value())
                        record.set("requiredSources"sv, *cache);
                    if (auto cache = old.get("cachedResources"sv); cache.has_value())
                        record.set("cachedResources"sv, *cache);
                    break;
                }
            }
            record.set("fileAccessGranted"sv, granted);
        }
        return items;
    }

    ErrorOr<void> set_file_access(StringView id, bool granted, u64 expected_revision)
    {
        if (expected_revision != m_revision)
            return Error::from_string_literal("Userscript registry changed; reload before changing permissions");
        auto items = m_items;
        for (auto& item : items.values()) {
            auto& record = item.as_object();
            if (record.get_string("id"sv) != id) continue;
            if (record.get_bool("fileAccessGranted"sv) == granted) return {};
            record.set("fileAccessGranted"sv, granted);
            return replace(move(items), expected_revision);
        }
        return Error::from_string_literal("Userscript no longer exists");
    }

    ErrorOr<void> replace_from_editor(JsonArray items, u64 expected_revision)
    {
        return replace(TRY(prepare_from_editor(move(items), expected_revision)), expected_revision);
    }

    ErrorOr<void> replace(JsonArray items, u64 expected_revision)
    {
        if (expected_revision != m_revision)
            return Error::from_string_literal("Userscript registry changed; reload before saving");
        TRY(validate(items));
        JsonObject root;
        root.set("version"sv, 1);
        root.set("items"sv, items);
        auto data = JsonValue(move(root)).serialized();
        if (data.bytes().size() > max_file_bytes)
            return Error::from_string_literal("Userscript file exceeds 16 MiB");
        if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
            return Error::from_string_literal("Cannot create userscript directory");
        QSaveFile file(m_path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return Error::from_string_literal("Cannot save userscripts");
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
            return Error::from_string_literal("Cannot set userscript file permissions");
        auto bytes = data.bytes_as_string_view();
        if (file.write(bytes.characters_without_null_termination(), bytes.length()) != static_cast<qint64>(bytes.length()))
            return Error::from_string_literal("Cannot write userscripts");
        if (!file.commit())
            return Error::from_string_literal("Cannot commit userscripts");
        m_items = move(items);
        ++m_revision;
        return {};
    }

private:
    static ErrorOr<void> validate(JsonArray const& items)
    {
        Web::DerpBirdScriptRegistry registry;
        return registry.replace(JsonValue { items });
    }

    static constexpr qint64 max_file_bytes = 16 * 1024 * 1024;
    QString m_path;
    JsonArray m_items;
    u64 m_revision { 0 };
};

}
