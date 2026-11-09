#pragma once

#include <LibWeb/Page/DerpBirdScriptMatchPattern.h>
#include <AK/Utf16String.h>
#include <LibRegex/ECMAScriptRegex.h>

namespace Web {

// Tampermonkey's legacy glob/ECMAScript include syntax. Evaluation uses a
// native budget rather than page JavaScript or an unbounded regexp engine.
class DerpBirdScriptIncludePattern {
public:
    static ErrorOr<DerpBirdScriptIncludePattern> create(StringView pattern)
    {
        if (pattern.is_empty() || pattern.length() > 16384)
            return Error::from_string_literal("Invalid userscript include pattern size");
        StringBuilder expression;
        if (pattern.starts_with('/')) {
            if (pattern.length() < 2 || !pattern.ends_with('/'))
                return Error::from_string_literal("Unterminated userscript include regexp");
            auto body = pattern.substring_view(1, pattern.length() - 2);
            auto original = Utf16String::from_utf8(body);
            if (regex::ECMAScriptRegex::compile(original.utf16_view(), {}).is_error())
                return Error::from_string_literal("Invalid userscript include regexp");
            // Search anywhere without changing numbered groups/backreferences;
            // ^ and $ inside the original expression retain their semantics.
            expression.append("^[\\s\\S]*?(?:"sv);
            expression.append(body);
            expression.append(')');
        } else {
            expression.append('^');
            auto scheme_end = pattern.find("://"sv);
            Optional<size_t> host_end;
            if (scheme_end.has_value()) {
                auto after = *scheme_end + 3;
                auto slash = pattern.substring_view(after).find('/');
                if (slash.has_value())
                    host_end = after + *slash;
            }
            for (size_t i = 0; i < pattern.length(); ++i) {
                char ch = pattern[i];
                if (ch == '*') {
                    if (scheme_end.has_value() && i < *scheme_end)
                        expression.append("[^:]*"sv);
                    else if (scheme_end.has_value() && i >= *scheme_end + 3
                        && ((host_end.has_value() && i < *host_end) || (i == *scheme_end + 3 && i + 1 < pattern.length())))
                        expression.append("[^/]*"sv);
                    else
                        expression.append("[\\s\\S]*"sv);
                } else {
                    if ("\\.^$+?()[]{}|"sv.contains(ch))
                        expression.append('\\');
                    expression.append(ch);
                }
            }
            expression.append("(?![\\s\\S])"sv);
        }
        auto utf16 = Utf16String::from_utf8(expression.string_view());
        auto compiled = regex::ECMAScriptRegex::compile(utf16.utf16_view(), {});
        if (compiled.is_error())
            return Error::from_string_literal("Invalid userscript include expression");
        DerpBirdScriptIncludePattern result;
        result.m_regex = compiled.release_value();
        return result;
    }

    DerpBirdScriptMatch matches(URL::URL const& url, DerpBirdScriptMatchBudget& budget) const
    {
        if (!url.scheme().is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "file"sv, "ftp"sv))
            return DerpBirdScriptMatch::NoMatch;
        auto serialized = url.serialize(URL::ExcludeFragment::Yes);
        if (serialized.bytes().size() > 16384 || serialized.bytes().size() >= budget.remaining) {
            budget.remaining = 0;
            return DerpBirdScriptMatch::LimitExceeded;
        }
        budget.remaining -= serialized.bytes().size();
        auto input = Utf16String::from_utf8(serialized);
        auto allowance = min(budget.remaining, static_cast<u64>(20000));
        auto before = allowance;
        auto result = m_regex->test_anchored_budgeted(input.utf16_view(), 0, allowance);
        budget.remaining -= before - allowance;
        switch (result) {
        case regex::MatchResult::Match:
            return DerpBirdScriptMatch::Match;
        case regex::MatchResult::NoMatch:
            return DerpBirdScriptMatch::NoMatch;
        case regex::MatchResult::LimitExceeded:
            return DerpBirdScriptMatch::LimitExceeded;
        }
        VERIFY_NOT_REACHED();
    }

private:
    Optional<regex::ECMAScriptRegex> m_regex;
};

}
