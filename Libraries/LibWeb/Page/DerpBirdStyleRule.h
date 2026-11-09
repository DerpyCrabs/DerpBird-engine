#pragma once

#include <AK/Optional.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <LibURL/Parser.h>

namespace Web {

inline bool allows_derpbird_user_styles(URL::URL const& url)
{
    return url.scheme().is_one_of("http"sv, "https"sv, "file"sv);
}

// Shared across all regular-expression conditions evaluated for one document.
// Domain/exact/prefix rules still work if an expensive regexp exhausts it.
struct DerpBirdStyleMatchBudget {
    u64 remaining { 100000 };
    bool exceeded { false };
    u32 skipped_conditions { 0 };
};

// Compiled once on installation/update, owned by the WebContent registry.
// This deliberately uses Ladybird's bounded ECMAScript VM rather than running
// regexes in the privileged shell or introducing a different regex dialect.
class DerpBirdStyleCondition {
public:
    static ErrorOr<DerpBirdStyleCondition> create(StringView type, String value)
    {
        if (value.bytes().size() > 16384)
            return Error::from_string_literal("User style condition exceeds 16 KiB");
        DerpBirdStyleCondition condition;
        condition.m_value = move(value);
        if (type == "url"sv)
            condition.m_type = Type::URL;
        else if (type == "url-prefix"sv)
            condition.m_type = Type::Prefix;
        else if (type == "domain"sv) {
            condition.m_type = Type::Domain;
            auto input = condition.m_value.bytes_as_string_view();
            if (input.is_empty())
                return Error::from_string_literal("Invalid user style domain");
            for (auto character : input) {
                if ("/:*?#@\\ \t\r\n"sv.contains(character))
                    return Error::from_string_literal("Invalid user style domain");
            }
            auto parsed = URL::Parser::basic_parse(TRY(String::formatted("https://{}/", input)));
            if (!parsed.has_value() || parsed->serialized_host().is_empty())
                return Error::from_string_literal("Invalid user style domain");
            condition.m_value = TRY(String::from_utf8(parsed->serialized_host()));
        } else if (type == "regexp"sv) {
            condition.m_type = Type::RegExp;
            // Validate before adding anchors: an unterminated character class
            // could otherwise consume the wrapper's closing bracket.
            auto original = Utf16String::from_utf8(condition.m_value);
            if (regex::ECMAScriptRegex::compile(original.utf16_view(), {}).is_error())
                return Error::from_string_literal("Invalid user style regular expression");
            // A noncapturing wrapper preserves backreference numbering. A
            // negative lookahead enforces the absolute end, including newlines.
            auto pattern = Utf16String::from_utf8(TRY(String::formatted("^(?:{})(?![\\s\\S])", condition.m_value)));
            auto compiled = regex::ECMAScriptRegex::compile(pattern.utf16_view(), {});
            if (compiled.is_error())
                return Error::from_string_literal("Invalid user style regular expression");
            condition.m_regex = compiled.release_value();
        } else {
            return Error::from_string_literal("Unknown user style condition");
        }
        return condition;
    }

    bool matches(URL::URL const& url) const
    {
        DerpBirdStyleMatchBudget budget;
        return matches(url, budget);
    }

    bool matches(URL::URL const& url, DerpBirdStyleMatchBudget& budget) const
    {
        if (!allows_derpbird_user_styles(url))
            return false;
        auto value = m_value.bytes_as_string_view();
        switch (m_type) {
        case Type::URL:
            // A target without a fragment also matches anchors on that page.
            return url.serialize() == m_value || (!value.contains('#') && url.serialize(URL::ExcludeFragment::Yes) == m_value);
        case Type::Prefix:
            return url.serialize().bytes_as_string_view().starts_with(value);
        case Type::Domain: {
            auto host = url.serialized_host();
            return host == value || (host.length() > value.length() && host.ends_with(value) && host[host.length() - value.length() - 1] == '.');
        }
        case Type::RegExp: {
            if (budget.remaining == 0) {
                budget.exceeded = true;
                ++budget.skipped_conditions;
                return false;
            }
            auto serialized = url.serialize();
            // Bound conversion and input scans as well as VM dispatches.
            if (serialized.bytes().size() > 16384 || serialized.bytes().size() >= budget.remaining) {
                budget.remaining = 0;
                budget.exceeded = true;
                ++budget.skipped_conditions;
                return false;
            }
            budget.remaining -= serialized.bytes().size();
            auto input = Utf16String::from_utf8(serialized);
            // A per-pattern cap leaves room for other styles in the registry.
            u64 allowance = min(budget.remaining, static_cast<u64>(20000));
            auto before = allowance;
            auto result = m_regex->test_anchored_budgeted(input.utf16_view(), 0, allowance);
            budget.remaining -= before - allowance;
            budget.exceeded |= result == regex::MatchResult::LimitExceeded;
            if (result == regex::MatchResult::LimitExceeded) ++budget.skipped_conditions;
            return result == regex::MatchResult::Match;
        }
        }
        VERIFY_NOT_REACHED();
    }

private:
    enum class Type { URL, Prefix, Domain, RegExp };
    Type m_type { Type::URL };
    String m_value;
    Optional<regex::ECMAScriptRegex> m_regex;
};

struct DerpBirdStyleSection {
    struct Wrapper {
        u32 id;
        Utf16String open;
    };
    Optional<Vector<Wrapper>> wrappers;
    Vector<DerpBirdStyleCondition> conditions;
    Utf16String css;
    String source_id;
    Vector<Vector<DerpBirdStyleCondition>> condition_groups;

    static ErrorOr<Vector<DerpBirdStyleSection>> decode(JsonValue const& value)
    {
        if (!value.is_array() || value.as_array().size() > 1024)
            return Error::from_string_literal("Invalid user style sections");
        Vector<DerpBirdStyleSection> sections;
        size_t total_source_bytes = 0;
        size_t total_conditions = 0;
        for (auto const& item : value.as_array().values()) {
            if (!item.is_object())
                return Error::from_string_literal("Invalid user style section");
            auto css = item.as_object().get_string("css"sv);
            auto conditions = item.as_object().get_array("conditions"sv);
            // A missing or malformed restriction must never become global.
            if (!css.has_value() || !conditions.has_value())
                return Error::from_string_literal("Missing user style CSS or conditions");
            total_source_bytes += css->bytes().size();
            total_conditions += conditions->size();
            if (total_source_bytes > 2 * 1024 * 1024 || total_conditions > 1024)
                return Error::from_string_literal("User style registry limit exceeded");
            DerpBirdStyleSection section;
            section.css = Utf16String::from_utf8(*css);
            if (auto wrappers = item.as_object().get("wrappers"sv); wrappers.has_value()) {
                if (!wrappers->is_array() || wrappers->as_array().size() > 32)
                    return Error::from_string_literal("Invalid style ancestors");
                section.wrappers = Vector<Wrapper> {};
                for (auto const& value : wrappers->as_array().values()) {
                    if (!value.is_object()) return Error::from_string_literal("Invalid style ancestor");
                    auto id = value.as_object().get_u32("id"sv);
                    auto open = value.as_object().get_string("open"sv);
                    if (!id.has_value() || *id == 0 || *id > 1024 || !open.has_value())
                        return Error::from_string_literal("Invalid style ancestor identity");
                    total_source_bytes += open->bytes().size();
                    if (total_source_bytes > 2 * 1024 * 1024)
                        return Error::from_string_literal("User style registry limit exceeded");
                    section.wrappers->append({ *id, Utf16String::from_utf8(*open) });
                }
            }
            if (auto id = item.as_object().get_string("sourceId"sv); id.has_value()) {
                if (id->bytes().size() > 128) return Error::from_string_literal("Invalid style source identifier");
                section.source_id = *id;
            }
            auto decode_conditions = [&](JsonArray const& entries, Vector<DerpBirdStyleCondition>& destination) -> ErrorOr<void> {
                for (auto const& entry : entries.values()) {
                    if (!entry.is_object())
                        return Error::from_string_literal("Invalid user style condition");
                    auto type = entry.as_object().get_string("type"sv);
                    auto pattern = entry.as_object().get_string("value"sv);
                    if (!type.has_value() || !pattern.has_value())
                        return Error::from_string_literal("Missing user style condition type or value");
                    destination.append(TRY(DerpBirdStyleCondition::create(type->bytes_as_string_view(), *pattern)));
                }
                return {};
            };
            TRY(decode_conditions(*conditions, section.conditions));
            if (auto groups = item.as_object().get("conditionGroups"sv); groups.has_value()) {
                if (!groups->is_array() || groups->as_array().size() > 31)
                    return Error::from_string_literal("Invalid nested user style conditions");
                for (auto const& group : groups->as_array().values()) {
                    if (!group.is_array() || group.as_array().is_empty())
                        return Error::from_string_literal("Invalid nested user style condition group");
                    total_conditions += group.as_array().size();
                    if (total_conditions > 1024)
                        return Error::from_string_literal("User style registry limit exceeded");
                    Vector<DerpBirdStyleCondition> decoded;
                    TRY(decode_conditions(group.as_array(), decoded));
                    section.condition_groups.append(move(decoded));
                }
            }
            sections.append(move(section));
        }
        return sections;
    }

    bool matches(URL::URL const& url) const
    {
        DerpBirdStyleMatchBudget budget;
        return matches(url, budget);
    }

    bool matches(URL::URL const& url, DerpBirdStyleMatchBudget& budget) const
    {
        if (!allows_derpbird_user_styles(url))
            return false;
        auto matches_group = [&](Vector<DerpBirdStyleCondition> const& group) {
            if (group.is_empty()) return true;
            for (auto const& condition : group) {
                if (condition.matches(url, budget)) return true;
            }
            return false;
        };
        if (!matches_group(conditions)) return false;
        for (auto const& group : condition_groups) {
            if (!matches_group(group)) return false;
        }
        return true;
    }
};

}
