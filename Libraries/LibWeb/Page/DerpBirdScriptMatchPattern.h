#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>

namespace Web {

// An exhausted match is indeterminate, not a negative result: callers must
// refuse script execution, especially when evaluating exclusion patterns.
enum class DerpBirdScriptMatch { NoMatch, Match, LimitExceeded };
struct DerpBirdScriptMatchBudget {
    u64 remaining { 100000 };
};

class DerpBirdScriptMatchPattern {
public:
    static ErrorOr<DerpBirdScriptMatchPattern> create(StringView pattern)
    {
        if (pattern.is_empty() || pattern.length() > 16384)
            return Error::from_string_literal("Invalid userscript match pattern size");
        DerpBirdScriptMatchPattern result;
        if (pattern == "<all_urls>"sv) {
            result.m_all = true;
            return result;
        }
        auto separator = pattern.find("://"sv);
        if (!separator.has_value())
            return Error::from_string_literal("Userscript match pattern requires scheme://host/path");
        auto scheme = pattern.substring_view(0, *separator);
        if (!scheme.is_one_of("*"sv, "http*"sv, "http"sv, "https"sv, "ws"sv, "wss"sv, "file"sv, "ftp"sv))
            return Error::from_string_literal("Unsupported userscript match scheme");
        result.m_scheme = TRY(String::from_utf8(scheme == "http*"sv ? "*"sv : scheme));
        auto rest = pattern.substring_view(*separator + 3);
        auto slash = rest.find('/');
        if (!slash.has_value())
            return Error::from_string_literal("Userscript match pattern requires a path");
        auto authority = rest.substring_view(0, *slash);
        auto path = rest.substring_view(*slash);
        if (path.contains('#'))
            return Error::from_string_literal("Userscript match paths cannot contain fragments");
        result.m_path = TRY(String::from_utf8(path));
        if (scheme == "file"sv) {
            if (!authority.is_empty())
                return Error::from_string_literal("File match patterns require an empty host");
            return result;
        }
        if (authority.is_empty() || authority.contains('@') || authority.contains('?') || authority.contains('#') || authority.contains('\\'))
            return Error::from_string_literal("Invalid userscript match host");
        for (auto character : authority) {
            if (static_cast<u8>(character) <= ' ' || character == 0x7f)
                return Error::from_string_literal("Invalid whitespace in userscript match host");
        }
        // Colons inside IPv6 brackets belong to the address, not the port.
        Optional<size_t> port_separator;
        if (authority.starts_with('[')) {
            auto close = authority.find(']');
            if (!close.has_value())
                return Error::from_string_literal("Invalid IPv6 match host");
            if (*close + 1 < authority.length()) {
                if (authority[*close + 1] != ':')
                    return Error::from_string_literal("Invalid IPv6 match port");
                port_separator = *close + 1;
            }
        } else {
            port_separator = authority.find(':');
        }
        if (port_separator.has_value()) {
            auto port = authority.substring_view(*port_separator + 1);
            if (port != "*"sv) {
                if (port.is_empty())
                    return Error::from_string_literal("Empty userscript match port");
                u32 number = 0;
                for (auto digit : port) {
                    if (digit < '0' || digit > '9' || number > 6553)
                        return Error::from_string_literal("Invalid userscript match port");
                    number = number * 10 + digit - '0';
                }
                if (number > 65535)
                    return Error::from_string_literal("Invalid userscript match port");
                result.m_port = static_cast<u16>(number);
            }
            authority = authority.substring_view(0, *port_separator);
        }
        if (authority == "*"sv) {
            result.m_any_host = true;
            return result;
        }
        if (authority.starts_with("*."sv)) {
            result.m_subdomains = true;
            authority = authority.substring_view(2);
        }
        if (authority.is_empty() || authority.contains('*') || authority.contains('%'))
            return Error::from_string_literal("Invalid wildcard userscript match host");
        auto parsed = URL::Parser::basic_parse(TRY(String::formatted("https://{}/", authority)));
        if (!parsed.has_value() || parsed->serialized_host().is_empty() || parsed->port().has_value())
            return Error::from_string_literal("Invalid userscript match host");
        result.m_host = TRY(String::from_utf8(parsed->serialized_host()));
        return result;
    }

    DerpBirdScriptMatch matches(URL::URL const& url, DerpBirdScriptMatchBudget& budget) const
    {
        auto scheme = url.scheme();
        if (!scheme.is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv, "file"sv, "ftp"sv))
            return DerpBirdScriptMatch::NoMatch;
        if (m_all)
            return DerpBirdScriptMatch::Match;
        if (m_scheme == "*"sv ? !scheme.is_one_of("http"sv, "https"sv) : m_scheme != scheme)
            return DerpBirdScriptMatch::NoMatch;
        if (m_port.has_value() && url.port_or_default() != *m_port)
            return DerpBirdScriptMatch::NoMatch;
        if (scheme != "file"sv && !m_any_host) {
            auto host = url.serialized_host();
            auto expected = m_host.bytes_as_string_view();
            if (host != expected && !(m_subdomains && host.length() > expected.length()
                    && host.ends_with(expected) && host[host.length() - expected.length() - 1] == '.'))
                return DerpBirdScriptMatch::NoMatch;
        }
        auto path = url.serialize_path();
        auto query = url.query();
        if (path.length() + (query.has_value() ? query->length() + 1 : 0) > 65536)
            return DerpBirdScriptMatch::LimitExceeded;
        StringBuilder builder;
        builder.append(path);
        if (query.has_value()) {
            builder.append('?');
            builder.append(*query);
        }
        auto input = builder.string_view();
        auto pattern = m_path.bytes_as_string_view();
        size_t i = 0, p = 0, retry = 0;
        Optional<size_t> star;
        while (i < input.length()) {
            if (!budget.remaining)
                return DerpBirdScriptMatch::LimitExceeded;
            --budget.remaining;
            if (p < pattern.length() && pattern[p] == '*') {
                star = p++;
                retry = i;
            } else if (p < pattern.length() && pattern[p] == input[i]) {
                ++p;
                ++i;
            } else if (star.has_value()) {
                p = *star + 1;
                i = ++retry;
            } else {
                return DerpBirdScriptMatch::NoMatch;
            }
        }
        while (p < pattern.length() && pattern[p] == '*') {
            if (!budget.remaining)
                return DerpBirdScriptMatch::LimitExceeded;
            --budget.remaining;
            ++p;
        }
        return p == pattern.length() ? DerpBirdScriptMatch::Match : DerpBirdScriptMatch::NoMatch;
    }

private:
    String m_scheme;
    String m_host;
    String m_path;
    Optional<u16> m_port;
    bool m_all { false };
    bool m_any_host { false };
    bool m_subdomains { false };
};

}
