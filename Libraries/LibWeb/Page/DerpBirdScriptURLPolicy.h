#pragma once

#include <AK/Vector.h>
#include <LibWeb/Page/DerpBirdScriptIncludePattern.h>

namespace Web {

enum class DerpBirdScriptEligibility {
    Allowed,
    NoMatch,
    Excluded,
    FrameDenied,
    FileAccessDenied,
    LimitExceeded,
};

// Compiled positive selectors are ORed; either exclusion family vetoes execution.
class DerpBirdScriptURLPolicy {
public:
    static ErrorOr<DerpBirdScriptURLPolicy> create(Vector<String> const& matches, Vector<String> const& exclude_matches, bool no_frames, Vector<String> const& includes = {}, Vector<String> const& excludes = {}, bool context_menu = false)
    {
        if (matches.size() + exclude_matches.size() + includes.size() + excludes.size() > 1024)
            return Error::from_string_literal("Too many userscript match patterns");
        size_t bytes = 0;
        DerpBirdScriptURLPolicy policy;
        policy.m_no_frames = no_frames;
        policy.m_context_menu = context_menu;
        auto compile = [&](Vector<String> const& inputs, auto& outputs, auto factory) -> ErrorOr<void> {
            for (auto const& input : inputs) {
                bytes += input.bytes().size();
                if (bytes > 65536)
                    return Error::from_string_literal("Userscript match metadata exceeds 64 KiB");
                outputs.append(TRY(factory(input)));
            }
            return {};
        };
        TRY(compile(matches, policy.m_matches, DerpBirdScriptMatchPattern::create));
        TRY(compile(exclude_matches, policy.m_exclusions, DerpBirdScriptMatchPattern::create));
        // Manual context-menu scripts ignore legacy include/exclude directives.
        // Keep explicit match/exclude-match restrictions and native file/frame
        // authority identical in the menu, renderer and capability provider.
        if (!context_menu) {
            TRY(compile(includes, policy.m_includes, DerpBirdScriptIncludePattern::create));
            TRY(compile(excludes, policy.m_excludes, DerpBirdScriptIncludePattern::create));
        }
        return policy;
    }

    DerpBirdScriptEligibility evaluate(URL::URL const& url, bool is_top_level_document, bool file_access_granted, DerpBirdScriptMatchBudget& budget) const
    {
        if (!url.scheme().is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "file"sv, "ftp"sv))
            return DerpBirdScriptEligibility::NoMatch;
        if (m_no_frames && !is_top_level_document)
            return DerpBirdScriptEligibility::FrameDenied;
        if (url.scheme() == "file"sv && !file_access_granted)
            return DerpBirdScriptEligibility::FileAccessDenied;
        auto any = [&](auto const& patterns) {
            for (auto const& pattern : patterns) {
                if (!budget.remaining)
                    return DerpBirdScriptMatch::LimitExceeded;
                --budget.remaining;
                auto result = pattern.matches(url, budget);
                if (result != DerpBirdScriptMatch::NoMatch)
                    return result;
            }
            return DerpBirdScriptMatch::NoMatch;
        };
        auto positive = m_context_menu && m_matches.is_empty() ? DerpBirdScriptMatch::Match : any(m_matches);
        if (positive == DerpBirdScriptMatch::NoMatch)
            positive = any(m_includes);
        if (positive == DerpBirdScriptMatch::LimitExceeded)
            return DerpBirdScriptEligibility::LimitExceeded;
        if (positive == DerpBirdScriptMatch::NoMatch)
            return DerpBirdScriptEligibility::NoMatch;
        auto excluded = any(m_exclusions);
        if (excluded == DerpBirdScriptMatch::NoMatch)
            excluded = any(m_excludes);
        if (excluded == DerpBirdScriptMatch::LimitExceeded)
            return DerpBirdScriptEligibility::LimitExceeded;
        if (excluded == DerpBirdScriptMatch::Match)
            return DerpBirdScriptEligibility::Excluded;
        return DerpBirdScriptEligibility::Allowed;
    }

private:
    bool m_context_menu { false };
    Vector<DerpBirdScriptMatchPattern> m_matches;
    Vector<DerpBirdScriptMatchPattern> m_exclusions;
    Vector<DerpBirdScriptIncludePattern> m_includes;
    Vector<DerpBirdScriptIncludePattern> m_excludes;
    bool m_no_frames { false };
};

}
