#pragma once
#include <AK/Optional.h>
#include <AK/CharacterTypes.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>

namespace Web {

class DerpBirdFilterCondition {
public:
    static Optional<bool> evaluate(StringView input)
    {
        if (input.length() > 4096) return {};
        DerpBirdFilterCondition parser { input };
        auto result = parser.expression(0);
        parser.space();
        return parser.m_offset == input.length() ? result : Optional<bool> {};
    }
private:
    explicit DerpBirdFilterCondition(StringView input) : m_input(input) { }
    void space() { while (m_offset < m_input.length() && is_ascii_space(m_input[m_offset])) ++m_offset; }
    Optional<bool> atom(size_t depth)
    {
        space();
        if (depth >= 64 || m_offset == m_input.length()) return {};
        if (m_input[m_offset] == '!') {
            ++m_offset;
            auto value = atom(depth + 1);
            return value.has_value() ? Optional<bool> { !*value } : Optional<bool> {};
        }
        if (m_input[m_offset] == '(') {
            ++m_offset;
            auto value = expression(depth + 1);
            space();
            if (m_offset == m_input.length() || m_input[m_offset++] != ')') return {};
            return value;
        }
        auto start = m_offset;
        while (m_offset < m_input.length() && (is_ascii_alphanumeric(m_input[m_offset]) || m_input[m_offset] == '_')) ++m_offset;
        if (start == m_offset) return {};
        auto token = m_input.substring_view(start, m_offset - start);
        if (token.is_one_of("ext_ublock"sv, "cap_html_filtering"sv, "cap_user_stylesheet"sv, "cap_urlskip"sv, "cap_ipaddress"sv)) return true;
        if (token.starts_with("cap_"sv) || token.is_one_of("false"sv, "ext_abp"sv, "ext_ubol"sv, "ext_devbuild"sv,
            "env_brave"sv, "env_chromium"sv, "env_edge"sv, "env_firefox"sv, "env_legacy"sv, "env_mobile"sv, "env_mv3"sv, "env_safari"sv,
            "adguard"sv, "adguard_app_android"sv, "adguard_app_cli"sv, "adguard_app_ios"sv, "adguard_app_mac"sv, "adguard_app_windows"sv,
            "adguard_ext_android_cb"sv, "adguard_ext_chromium"sv, "adguard_ext_chromium_mv3"sv, "adguard_ext_edge"sv,
            "adguard_ext_firefox"sv, "adguard_ext_opera"sv, "adguard_ext_safari"sv)) return false;
        // Unknown conditions are ignored, as in uBO: neither branch is discarded.
        return {};
    }
    Optional<bool> expression(size_t depth)
    {
        auto value = atom(depth);
        for (;;) {
            space();
            if (m_offset == m_input.length() || m_input[m_offset] == ')') return value;
            auto tail = m_input.substring_view(m_offset);
            bool is_and = tail.starts_with("&&"sv);
            if (!is_and && !tail.starts_with("||"sv)) return {};
            m_offset += 2;
            auto rhs = atom(depth);
            if (!value.has_value() || !rhs.has_value()) value.clear();
            else value = is_and ? *value && *rhs : *value || *rhs;
        }
    }
    StringView m_input;
    size_t m_offset { 0 };
};

inline ErrorOr<String> preprocess_derpbird_filter_list(StringView input)
{
    struct Branch { bool parent; Optional<bool> condition; bool alternate { false }; };
    Vector<Branch> stack;
    StringBuilder output;
    bool active = true;
    // Preserve physical line numbers so source attribution still points to the file.
    for (auto line : input.split_view('\n', SplitBehavior::KeepEmpty)) {
        auto trimmed = line.trim_whitespace();
        if (trimmed.starts_with("!#if "sv) || trimmed.starts_with("!#if\t"sv)) {
            if (stack.size() >= 64) return Error::from_string_literal("Filter condition nesting exceeds 64");
            auto condition = DerpBirdFilterCondition::evaluate(trimmed.substring_view(5).trim_whitespace());
            stack.append({ active, condition });
            active = active && condition.value_or(true);
        } else if (trimmed == "!#else"sv) {
            if (stack.is_empty() || stack.last().alternate) return Error::from_string_literal("Unmatched filter else directive");
            auto& branch = stack.last();
            branch.alternate = true;
            active = branch.parent && (!branch.condition.has_value() || !*branch.condition);
        } else if (trimmed == "!#endif"sv) {
            if (stack.is_empty()) return Error::from_string_literal("Unmatched filter endif directive");
            active = stack.take_last().parent;
        } else if (active) {
            output.append(line);
        }
        output.append('\n');
    }
    if (!stack.is_empty()) return Error::from_string_literal("Unterminated filter condition");
    return output.to_string();
}

}
