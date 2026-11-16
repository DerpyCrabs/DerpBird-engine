#pragma once

// Included by Proxy.cpp: ZeroOmega schema 2 routing shared by all RequestServers.
// Keep the original options intact so export preserves extension metadata.
namespace HTTP {

static bool omega_glob(StringView text, StringView pattern)
{
    size_t t = 0, p = 0, star = 0, retry = 0;
    bool has_star = false;
    while (t < text.length()) {
        if (p < pattern.length() && (pattern[p] == '?' || pattern[p] == text[t])) { ++p; ++t; }
        else if (p < pattern.length() && pattern[p] == '*') { has_star = true; star = ++p; retry = t; }
        else if (has_star) { p = star; t = ++retry; }
        else return false;
    }
    while (p < pattern.length() && pattern[p] == '*') ++p;
    return p == pattern.length();
}

static bool omega_condition(JsonObject const& condition, URL::URL const& url)
{
    auto type = condition.get_string("conditionType"sv).value_or(String {});
    if (type == "TrueCondition"sv) return true;
    if (type == "FalseCondition"sv) return false;
    auto pattern_string = condition.get_string("pattern"sv).value_or(String {});
    auto pattern = pattern_string.bytes_as_string_view();
    auto host_string = url.serialized_host().to_byte_string().to_lowercase();
    StringView host = host_string;
    if (host.starts_with('[') && host.ends_with(']')) host = host.substring_view(1, host.length() - 2);
    auto address = url.serialize();
    if (type == "KeywordCondition"sv) return url.scheme() == "http"sv && address.bytes_as_string_view().contains(pattern);
    if (type == "HostRegexCondition"sv || type == "UrlRegexCondition"sv) {
        auto compiled = regex::ECMAScriptRegex::compile(Utf16String::from_utf8(pattern), {});
        return !compiled.is_error() && compiled.value().test(Utf16String::from_utf8(type == "HostRegexCondition"sv ? host : address.bytes_as_string_view()), 0) == regex::MatchResult::Match;
    }
    if (type == "IpCondition"sv) {
        auto ip = condition.get_string("ip"sv).value_or(String {});
        auto prefix = condition.get_u32("prefixLength"sv).value_or(0);
        auto network = ByteString::formatted("{}/{}", ip, prefix);
        if (auto v4 = IPv4Address::from_string(host); v4.has_value()) return address_matches(*v4, network);
        if (auto v6 = IPv6Address::from_string(host); v6.has_value()) return address_matches(*v6, network);
        return false;
    }
    if (type == "BypassCondition"sv) {
        if (pattern == "<local>"sv) return !host.contains('.') || host == "127.0.0.1"sv;
        if (auto scheme = pattern.find("://"sv); scheme.has_value()) {
            if (url.scheme() != pattern.substring_view(0, *scheme)) return false;
            pattern = pattern.substring_view(*scheme + 3);
        }
        if (pattern.contains('/')) {
            if (auto v4 = IPv4Address::from_string(host); v4.has_value()) return address_matches(*v4, pattern);
            if (auto v6 = IPv6Address::from_string(host); v6.has_value()) return address_matches(*v6, pattern);
            return false;
        }
        if (!IPv6Address::from_string(pattern).has_value()) {
            auto colon = pattern.find_last(':');
            auto bracket = pattern.find(']');
            if (colon.has_value() && (!bracket.has_value() || *colon > *bracket)) {
                auto port = pattern.substring_view(*colon + 1).to_number<u16>();
                auto actual = url.port().value_or(url.scheme().is_one_of("https"sv, "wss"sv) ? 443 : 80);
                if (!port.has_value() || *port != actual) return false;
                pattern = pattern.substring_view(0, *colon);
            }
        }
        if (pattern.starts_with('[') && pattern.ends_with(']')) pattern = pattern.substring_view(1, pattern.length() - 2);
        if (pattern.starts_with('.')) return host.ends_with(pattern);
        return omega_glob(host, pattern);
    }
    for (auto alternative : pattern.split_view('|')) {
        if (type == "UrlWildcardCondition"sv) {
            if (omega_glob(address.bytes_as_string_view(), alternative)) return true;
        } else {
            if (alternative.starts_with("**."sv)) {
                if (omega_glob(host, alternative.substring_view(1))) return true;
            } else if (alternative.starts_with("*."sv) || alternative.starts_with('.')) {
                auto suffix = alternative.substring_view(alternative.starts_with('.') ? 1 : 2);
                if (omega_glob(host, suffix)) return true;
                for (size_t i = 0; i < host.length(); ++i)
                    if (host[i] == '.' && omega_glob(host.substring_view(i + 1), suffix)) return true;
            } else if (omega_glob(host, alternative)) return true;
        }
    }
    return false;
}

static ErrorOr<void> validate_omega_condition(JsonValue const& value)
{
    if (!value.is_object()) return Error::from_string_literal("Invalid proxy condition");
    auto const& condition = value.as_object();
    auto type = condition.get_string("conditionType"sv).value_or(String {});
    if (type.is_one_of("TrueCondition"sv, "FalseCondition"sv)) return {};
    if (type == "IpCondition"sv) {
        auto ip = condition.get_string("ip"sv);
        auto prefix = condition.get_u32("prefixLength"sv);
        if (!ip.has_value() || !prefix.has_value()) return Error::from_string_literal("Invalid IP condition");
        if (IPv4Address::from_string(*ip).has_value() && *prefix <= 32) return {};
        if (IPv6Address::from_string(*ip).has_value() && *prefix <= 128) return {};
        return Error::from_string_literal("Invalid IP condition");
    }
    if (!type.is_one_of("HostWildcardCondition"sv, "UrlWildcardCondition"sv, "HostRegexCondition"sv, "UrlRegexCondition"sv, "KeywordCondition"sv, "BypassCondition"sv))
        return Error::from_string_literal("Unsupported ZeroOmega condition type");
    auto pattern = condition.get_string("pattern"sv);
    if (!pattern.has_value() || pattern->is_empty() || pattern->bytes().size() > 4096)
        return Error::from_string_literal("Invalid proxy condition pattern");
    if (type.is_one_of("HostRegexCondition"sv, "UrlRegexCondition"sv)) {
        auto compiled = regex::ECMAScriptRegex::compile(Utf16String::from_utf8(*pattern), {});
        if (compiled.is_error()) return Error::from_string_literal("Invalid proxy regular expression");
    }
    return {};
}

ErrorOr<void> ProxyConfiguration::set_omega(JsonObject options, String active)
{
    if (options.get_u32("schemaVersion"sv) != 2u) return Error::from_string_literal("Expected a ZeroOmega schema 2 backup");
    HashMap<String, Proxy> endpoints;
    HashTable<String> names;
    Optional<Error> error;
    options.for_each_member([&](auto const& key, auto const& value) {
        if (!key.starts_with('+') || error.has_value()) return;
        auto validate = [&]() -> ErrorOr<void> {
            if (!value.is_object()) return Error::from_string_literal("Invalid proxy profile");
            auto const& profile = value.as_object();
            auto name = profile.get_string("name"sv);
            auto type = profile.get_string("profileType"sv);
            if (!name.has_value() || name->is_empty() || name->bytes().size() > 256 || key != TRY(String::formatted("+{}", *name)) || name->is_one_of("direct"sv, "system"sv))
                return Error::from_string_literal("Invalid or reserved proxy profile name");
            if (!type.has_value() || !type->is_one_of("FixedProfile"sv, "SwitchProfile"sv, "VirtualProfile"sv, "DirectProfile"sv, "SystemProfile"sv))
                return Error::from_string_literal("Unsupported ZeroOmega profile: PAC scripts and subscription lists are not supported");
            names.set(*name);
            if (names.size() > 128) return Error::from_string_literal("Too many proxy profiles");
            if (type == "SwitchProfile"sv || type == "VirtualProfile"sv) {
                auto rules = profile.get_array("rules"sv);
                if (type == "SwitchProfile"sv && !rules.has_value()) return Error::from_string_literal("Missing proxy rules");
                if (rules.has_value()) {
                    if (rules->size() > 20000) return Error::from_string_literal("Too many proxy rules");
                    for (auto const& rule : rules->values()) {
                        if (!rule.is_object() || !rule.as_object().has("condition"sv)) return Error::from_string_literal("Invalid proxy rule");
                        TRY(validate_omega_condition(rule.as_object().get("condition"sv).value()));
                    }
                }
            }
            if (type != "FixedProfile"sv) return {};
            if (profile.has("bypassList"sv)) {
                auto bypass = profile.get_array("bypassList"sv);
                if (!bypass.has_value() || bypass->size() > 20000) return Error::from_string_literal("Invalid proxy bypass list");
                for (auto const& condition : bypass->values()) TRY(validate_omega_condition(condition));
            }
            for (auto field : { "fallbackProxy"sv, "proxyForHttp"sv, "proxyForHttps"sv, "proxyForFtp"sv }) {
                if (!profile.has(field)) continue;
                auto endpoint = profile.get_object(field);
                if (!endpoint.has_value()) return Error::from_string_literal("Invalid proxy endpoint");
                auto scheme = endpoint->get_string("scheme"sv);
                if (scheme == "direct"sv) continue;
                auto host = endpoint->get_string("host"sv);
                auto port = endpoint->get_u32("port"sv);
                if (!scheme.has_value() || !scheme->is_one_of("http"sv, "https"sv, "socks4"sv, "socks5"sv) || !host.has_value() || host->is_empty() || !port.has_value() || *port == 0 || *port > 65535)
                    return Error::from_string_literal("Invalid proxy server, scheme or port");
                auto host_view = host->bytes_as_string_view();
                for (auto ch : host_view) if (is_ascii_space(ch) || ch == '/' || ch == '@' || ch == '?' || ch == '#' || ch == '\\') return Error::from_string_literal("Invalid proxy hostname");
                auto normalized_host = host_view.contains(':') && !host_view.starts_with('[') ? TRY(String::formatted("[{}]", *host)) : *host;
                auto server = TRY(String::formatted("{}://{}", scheme == "socks5"sv ? "socks5h"sv : scheme->bytes_as_string_view(), normalized_host));
                auto proxy = Proxy::parse(TRY(String::formatted("{}:{}", server, *port)));
                if (!proxy.has_value()) return Error::from_string_literal("Invalid proxy server");
                if (auto auth = profile.get_object("auth"sv); auth.has_value()) {
                    auto credentials = auth->get_object(field);
                    if (!credentials.has_value()) credentials = auth->get_object("all"sv);
                    if (credentials.has_value()) {
                        auto user = credentials->get_string("username"sv);
                        auto password = credentials->get_string("password"sv);
                        if (!user.has_value() || !password.has_value()) return Error::from_string_literal("Invalid proxy credentials");
                        // Percent-encode userinfo to preserve delimiters and non-ASCII bytes.
                        StringBuilder encoded;
                        auto append = [&](String const& text) { for (auto byte : text.bytes()) { if (is_ascii_alphanumeric(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') encoded.append(static_cast<char>(byte)); else encoded.appendff("%{:02X}", byte); } };
                        append(*user); encoded.append(':'); append(*password);
                        proxy->userinfo = encoded.to_byte_string();
                    }
                }
                endpoints.set(TRY(String::formatted("{}:{}", *name, field)), proxy.release_value());
            }
            return {};
        };
        auto result = validate();
        if (result.is_error()) error = result.release_error();
    });
    if (error.has_value()) return error.release_value();
    auto exists = [&](String const& name) { return name.is_one_of("direct"sv, "system"sv) || names.contains(name); };
    if (!exists(active)) return Error::from_string_literal("Unknown active proxy profile");
    HashTable<String> visiting, visited;
    Function<ErrorOr<void>(String const&, size_t)> visit = [&](String const& name, size_t depth) -> ErrorOr<void> {
        if (!exists(name)) return Error::from_string_literal("Proxy rule references an unknown profile");
        if (visiting.contains(name) || depth > 128) return Error::from_string_literal("Proxy profiles contain a cycle");
        if (name.is_one_of("direct"sv, "system"sv) || visited.contains(name)) return {};
        visiting.set(name);
        auto const& profile = options.get_object(TRY(String::formatted("+{}", name))).value();
        auto type = profile.get_string("profileType"sv).value();
        if (type.is_one_of("SwitchProfile"sv, "VirtualProfile"sv)) {
            auto fallback = profile.get_string("defaultProfileName"sv);
            if (!fallback.has_value()) return Error::from_string_literal("Missing default proxy profile");
            TRY(visit(*fallback, depth + 1));
            if (auto rules = profile.get_array("rules"sv); rules.has_value()) for (auto const& rule : rules->values()) {
                auto target = rule.as_object().get_string("profileName"sv);
                if (!target.has_value()) return Error::from_string_literal("Missing rule target");
                TRY(visit(*target, depth + 1));
            }
        }
        visiting.remove(name); visited.set(name);
        return {};
    };
    for (auto const& name : names) TRY(visit(name, 0));
    if (options.has("-derpbirdDomainRules"sv)) {
        auto rules = options.get_object("-derpbirdDomainRules"sv);
        if (!rules.has_value() || rules->size() > 20000) return Error::from_string_literal("Invalid domain proxy rules");
        Optional<Error> rule_error;
        rules->for_each_member([&](auto const& host, auto const& target) {
            auto address = MUST(String::formatted("https://{}/", host));
            auto parsed = URL::Parser::basic_parse(address);
            if (host.bytes_as_string_view().contains('*') || host.bytes_as_string_view().contains('|') || host.bytes().size() > 253 || !parsed.has_value() || parsed->serialized_host() != host || !target.is_string() || !exists(target.as_string()))
                rule_error = Error::from_string_literal("Invalid domain proxy rule");
        });
        if (rule_error.has_value()) return rule_error.release_value();
    }
    m_omega_options = move(options);
    m_active_profile = move(active);
    m_omega_endpoints = move(endpoints);
    return {};
}

Optional<Proxy const&> ProxyConfiguration::omega_proxy_for(URL::URL const& url, String const& name, size_t depth) const
{
    if (depth > 128 || name == "direct"sv || !url.scheme().is_one_of("http"sv, "https"sv, "ws"sv, "wss"sv)) return {};
    if (name == "system"sv) {
        if (is_excluded(url.serialized_host())) return {};
        auto const& proxy = url.scheme().is_one_of("https"sv, "wss"sv) ? m_https_proxy : m_http_proxy;
        if (proxy.has_value()) return *proxy;
        return {};
    }
    auto profile = m_omega_options.get_object(MUST(String::formatted("+{}", name)));
    if (!profile.has_value()) return {};
    auto type = profile->get_string("profileType"sv).value_or(String {});
    if (type == "DirectProfile"sv) return {};
    if (type == "SystemProfile"sv) return omega_proxy_for(url, "system"_string, depth + 1);
    if (type.is_one_of("SwitchProfile"sv, "VirtualProfile"sv)) {
        if (auto rules = profile->get_array("rules"sv); rules.has_value()) for (auto const& rule : rules->values()) {
            if (omega_condition(rule.as_object().get_object("condition"sv).value(), url))
                return omega_proxy_for(url, rule.as_object().get_string("profileName"sv).value(), depth + 1);
        }
        return omega_proxy_for(url, profile->get_string("defaultProfileName"sv).value(), depth + 1);
    }
    if (auto bypass = profile->get_array("bypassList"sv); bypass.has_value()) for (auto const& condition : bypass->values())
        if (omega_condition(condition.as_object(), url)) return {};
    auto field = url.scheme().is_one_of("https"sv, "wss"sv) ? "proxyForHttps"sv : "proxyForHttp"sv;
    if (!profile->has(field)) field = "fallbackProxy"sv;
    return m_omega_endpoints.get(MUST(String::formatted("{}:{}", name, field)));
}

String ProxyConfiguration::profile_for(URL::URL const& url) const
{
    if (!proxy_for(url).has_value()) return "direct"_string;
    auto name = m_active_profile.is_empty() ? "system"_string : m_active_profile;
    if (auto rules = m_omega_options.get_object("-derpbirdDomainRules"sv); rules.has_value())
        name = rules->get_string(url.serialized_host()).value_or(name);
    for (size_t depth = 0; depth < 129; ++depth) {
        if (name.is_one_of("direct"sv, "system"sv)) return name;
        auto profile = m_omega_options.get_object(MUST(String::formatted("+{}", name)));
        if (!profile.has_value()) return "direct"_string;
        auto type = profile->get_string("profileType"sv).value_or(String {});
        if (type == "SystemProfile"sv) return "system"_string;
        if (!type.is_one_of("SwitchProfile"sv, "VirtualProfile"sv)) return name;
        auto next = profile->get_string("defaultProfileName"sv).value_or("direct"_string);
        if (auto rules = profile->get_array("rules"sv); rules.has_value()) {
            for (auto const& rule : rules->values()) {
                if (omega_condition(rule.as_object().get_object("condition"sv).value(), url)) {
                    next = rule.as_object().get_string("profileName"sv).value();
                    break;
                }
            }
        }
        name = move(next);
    }
    return "direct"_string;
}

}
