#pragma once

#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/kmalloc.h>
#include <LibWeb/Page/DerpBirdStyleRule.h>
#include <LibURL/Parser.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace Ladybird {

// One instance per profile, shared by windows. Callers publish changes only
// after replace() succeeds; neither validation nor I/O failure changes state.
class DerpBirdStyleStore {
public:
    AK_ALLOC_WITH_KMALLOC;
    explicit DerpBirdStyleStore(QString path)
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
            return Error::from_string_literal("Cannot read user styles");
        if (file.size() > max_file_bytes)
            return Error::from_string_literal("User style file exceeds 8 MiB");
        auto bytes = file.read(max_file_bytes + 1);
        if (file.error() != QFile::NoError || bytes.size() > max_file_bytes)
            return Error::from_string_literal("Cannot read user style file");
        auto root = TRY(JsonValue::from_string(StringView(bytes.constData(), bytes.size())));
        if (!root.is_object() || root.as_object().get_integer<int>("version"sv) != 1)
            return Error::from_string_literal("Unsupported user style file version");
        auto items = root.as_object().get_array("items"sv);
        if (!items.has_value())
            return Error::from_string_literal("Missing user styles");
        TRY(validate(*items));
        m_items = *items;
        return {};
    }

    ErrorOr<void> replace(JsonArray items)
    {
        TRY(validate(items));
        JsonObject root;
        root.set("version"sv, 1);
        root.set("items"sv, items);
        auto data = JsonValue(move(root)).serialized();
        if (data.bytes().size() > max_file_bytes)
            return Error::from_string_literal("User style file exceeds 8 MiB");
        if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
            return Error::from_string_literal("Cannot create user style directory");
        QSaveFile file(m_path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return Error::from_string_literal("Cannot save user styles");
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
            return Error::from_string_literal("Cannot set user style file permissions");
        auto bytes = data.bytes_as_string_view();
        if (file.write(bytes.characters_without_null_termination(), bytes.length()) != static_cast<qint64>(bytes.length()))
            return Error::from_string_literal("Cannot write user styles");
        if (!file.commit())
            return Error::from_string_literal("Cannot commit user styles");
        m_items = move(items);
        ++m_revision;
        return {};
    }

    JsonArray enabled_sections() const
    {
        JsonArray sections;
        for (auto const& item : m_items.values()) {
            auto const& object = item.as_object();
            if (!object.get_bool("enabled"sv).value())
                continue;
            for (auto const& section : object.get_array("sections"sv)->values()) {
                auto identified = section.as_object();
                identified.set("sourceId"sv, object.get_string("id"sv).value());
                sections.must_append(move(identified));
            }
        }
        return sections;
    }

private:
    static ErrorOr<void> validate(JsonArray const& items)
    {
        if (items.size() > 256)
            return Error::from_string_literal("Too many user styles");
        HashTable<String> ids;
        JsonArray all_sections;
        size_t total_source_bytes = 0;
        for (auto const& item : items.values()) {
            if (!item.is_object())
                return Error::from_string_literal("Invalid user style item");
            auto const& object = item.as_object();
            auto id = object.get_string("id"sv);
            auto name = object.get_string("name"sv);
            auto source = object.get_string("source"sv);
            auto enabled = object.get_bool("enabled"sv);
            auto sections = object.get_array("sections"sv);
            if (!id.has_value() || id->is_empty() || id->bytes().size() > 128 || !name.has_value() || name->is_empty() || name->bytes().size() > 512
                || !source.has_value() || !enabled.has_value() || !sections.has_value())
                return Error::from_string_literal("Invalid user style metadata");
            if (auto usercss = object.get("usercss"sv); usercss.has_value()) {
                if (!usercss->is_object()) return Error::from_string_literal("Invalid UserCSS configuration");
                auto values = usercss->as_object().get_object("values"sv);
                if (!values.has_value()) return Error::from_string_literal("Missing UserCSS variable values");
                auto serialized_values = values->serialized();
                if (values->size() > 128 || serialized_values.bytes().size() > 65536)
                    return Error::from_string_literal("Invalid UserCSS variable values");
                bool valid = true;
                values->for_each_member([&](auto const& name, auto const& value) {
                    valid &= !name.is_empty() && name.bytes().size() <= 128 && (value.is_string() || value.is_number());
                    for (auto ch : name.bytes_as_string_view())
                        valid &= is_ascii_alphanumeric(ch) || ch == '_' || ch == '-';
                });
                if (!valid) return Error::from_string_literal("Invalid UserCSS variable value");
            }
            if (ids.set(*id) != AK::HashSetResult::InsertedNewEntry)
                return Error::from_string_literal("Duplicate user style ID");
            if (object.has("sourceBaseURL"sv)) {
                auto base = object.get_string("sourceBaseURL"sv);
                auto url = base.has_value() ? URL::Parser::basic_parse(*base) : Optional<URL::URL> {};
                if (!base.has_value() || base->bytes().size() > 8192 || !url.has_value() || url->scheme() != "https"sv
                    || url->serialized_host() != "derpbird-source.invalid"sv || !url->username().is_empty() || !url->password().is_empty())
                    return Error::from_string_literal("Invalid local style base");
            }
            if (object.has("dependencyRevision"sv)) {
                auto revision = object.get_string("dependencyRevision"sv);
                if (!revision.has_value() || revision->bytes().size() != 64)
                    return Error::from_string_literal("Invalid style dependency revision");
                for (auto ch : revision->bytes_as_string_view())
                    if (!is_ascii_digit(ch) && !(ch >= 'a' && ch <= 'f')) return Error::from_string_literal("Invalid style dependency revision");
            }
            total_source_bytes += source->bytes().size();
            if (object.has("imports"sv)) {
                auto imports = object.get_object("imports"sv);
                if (!imports.has_value() || imports->size() > 32)
                    return Error::from_string_literal("Invalid style imports");
                bool valid = true;
                auto valid_url = [](String const& text) {
                    auto url = URL::Parser::basic_parse(text);
                    return text.bytes().size() <= 8192 && url.has_value() && url->scheme().is_one_of("http"sv, "https"sv)
                        && url->username().is_empty() && url->password().is_empty();
                };
                imports->for_each_member([&](auto const& name, auto const& value) {
                    if (!value.is_object() || !valid_url(name)) { valid = false; return; }
                    auto source = value.as_object().get_string("source"sv);
                    auto url = value.as_object().get_string("url"sv);
                    if (!source.has_value() || !url.has_value() || !valid_url(*url) || source->bytes_as_string_view().contains('\0')) { valid = false; return; }
                    total_source_bytes += source->bytes().size();
                });
                if (!valid) return Error::from_string_literal("Invalid style import source");
            }
            if (total_source_bytes > 2 * 1024 * 1024)
                return Error::from_string_literal("User style source limit exceeded");
            for (auto const& section : sections->values())
                all_sections.must_append(section);
        }
        // Validate disabled entries too: enabling must not reveal malformed data.
        TRY(Web::DerpBirdStyleSection::decode(all_sections));
        return {};
    }

    static constexpr qint64 max_file_bytes = 8 * 1024 * 1024;
    QString m_path;
    JsonArray m_items;
    u64 m_revision { 0 };
};

}
