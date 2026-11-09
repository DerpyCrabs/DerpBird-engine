#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <LibURL/Site.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebView/CookieJar.h>

namespace Ladybird {

// The caller supplies the native document URL, the session's own jar and an
// authority check. No script-supplied profile/session identifier is accepted.
class DerpBirdScriptCookies {
public:
    using Protocol = Web::DerpBirdValueProtocol;
    template<typename Authorize>
    static ErrorOr<ByteBuffer> dispatch(WebView::CookieJar& jar, URL::URL const& document_url,
        Protocol::Request const& request, Authorize authorize, bool is_private = false)
    {
        if (request.method != Protocol::Method::Cookie && request.method != Protocol::Method::CookieLegacy)
            return Error::from_string_literal("Invalid cookie method");
        TRY(Protocol::encode(request));
        auto input = TRY(JsonValue::from_string(StringView { request.updates[0].value->bytes() }));
        if (!input.is_object()) return Error::from_string_literal("Expected cookie details");
        auto const& details = input.as_object();
        for (auto field : { "url"sv, "domain"sv, "path"sv, "name"sv, "value"sv, "sameSite"sv, "storeId"sv, "firstPartyDomain"sv }) {
            if (details.has(field) && !details.get_string(field).has_value())
                return Error::from_string_literal("Invalid cookie string option");
        }
        for (auto field : { "secure"sv, "httpOnly"sv }) {
            if (details.has(field) && !details.get_bool(field).has_value())
                return Error::from_string_literal("Invalid cookie boolean option");
        }
        auto address = details.get_string("url"sv);
        auto url = address.has_value() ? URL::Parser::basic_parse(*address) : Optional<URL::URL> { document_url };
        if (!url.has_value()) return Error::from_string_literal("Invalid cookie URL");
        TRY(authorize(*url));
        auto host = HTTP::Cookie::canonicalize_domain(*url);
        if (!host.has_value()) return Error::from_string_literal("Invalid cookie host");
        auto store_id = is_private ? "1"sv : "0"sv;
        if (auto requested_store = details.get_string("storeId"sv); requested_store.has_value()
            && *requested_store != store_id && *requested_store != (is_private ? "firefox-private"sv : "firefox-default"sv))
            return Error::from_string_literal("Cookie store is outside this script's profile/session");
        Utf16String partition;
        bool all_partitions = false;
        if (details.has("partitionKey"sv)) {
            auto key = details.get_object("partitionKey"sv);
            if (!key.has_value()) return Error::from_string_literal("Invalid cookie partition key");
            if (key->is_empty() && request.key == "list"sv) all_partitions = true;
            else {
                auto site = key->get_string("topLevelSite"sv);
                auto parsed = site.has_value() ? URL::Parser::basic_parse(*site) : Optional<URL::URL> {};
                if (!parsed.has_value() || (parsed->scheme() != "http"sv && parsed->scheme() != "https"sv)
                    || !parsed->username().is_empty() || !parsed->password().is_empty())
                    return Error::from_string_literal("Invalid cookie top-level site");
                partition = URL::Site::serialize_for_partitioning(parsed->origin()).value_or(Utf16String {});
                if (partition.is_empty()) return Error::from_string_literal("Opaque cookie partition");
            }
        }
        if (auto first_party = details.get_string("firstPartyDomain"sv); first_party.has_value() && !first_party->is_empty()) {
            // DerpBird's native partition keys are schemeful sites. An explicit
            // partitionKey supplies the scheme; otherwise use this document's scheme.
            auto scheme = document_url.scheme().is_one_of("http"sv, "https"sv) ? document_url.scheme() : "https"sv;
            auto address = TRY(String::formatted("{}://{}/", scheme, *first_party));
            auto site = URL::Parser::basic_parse(address);
            if (!site.has_value() || !site->username().is_empty() || !site->password().is_empty()
                || site->serialized_host() != first_party->to_byte_string() || site->port().has_value())
                return Error::from_string_literal("Invalid first-party cookie domain");
            auto first_partition = URL::Site::serialize_for_partitioning(site->origin()).value_or(Utf16String {});
            if (first_partition.is_empty()) return Error::from_string_literal("Opaque first-party cookie domain");
            if (!partition.is_empty()) {
                auto existing = URL::Parser::basic_parse(partition.to_utf8());
                auto normalized_first = URL::Parser::basic_parse(first_partition.to_utf8());
                if (!existing.has_value() || !normalized_first.has_value() || existing->serialized_host() != normalized_first->serialized_host())
                    return Error::from_string_literal("Conflicting cookie partition options");
            } else { partition = move(first_partition); }
            all_partitions = false;
        }
        auto name = details.get_string("name"sv);
        Optional<String> domain;
        if (auto value = details.get_string("domain"sv); value.has_value()) domain = *value;
        if (domain.has_value()) {
            auto normalized = domain->bytes_as_string_view();
            if (normalized.starts_with('.')) normalized = normalized.substring_view(1);
            domain = normalized.to_ascii_lowercase_string();
            if (domain->is_empty() || !HTTP::Cookie::domain_matches(*host, *domain))
                return Error::from_string_literal("Cookie domain does not match URL");
        }
        auto path = details.get_string("path"sv);
        if (request.key == "set"sv) {
            auto value = details.get_string("value"sv);
            if (!name.has_value() || !value.has_value()) return Error::from_string_literal("Cookie name and value are required");
            HTTP::Cookie::Cookie cookie;
            cookie.name = *name;
            cookie.value = *value;
            cookie.domain = domain.value_or(*host);
            cookie.host_only = !domain.has_value();
            cookie.path = path.value_or(HTTP::Cookie::default_path(*url));
            cookie.partition_key = partition;
            cookie.secure = details.get_bool("secure"sv).value_or(false);
            cookie.http_only = details.get_bool("httpOnly"sv).value_or(false);
            auto same_site = details.get_string("sameSite"sv).value_or("unspecified"_string);
            if (same_site == "no_restriction"sv) cookie.same_site = HTTP::Cookie::SameSite::None;
            else if (same_site == "lax"sv) cookie.same_site = HTTP::Cookie::SameSite::Lax;
            else if (same_site == "strict"sv) cookie.same_site = HTTP::Cookie::SameSite::Strict;
            else if (same_site != "unspecified"sv) return Error::from_string_literal("Invalid cookie SameSite");
            if (cookie.secure && url->scheme() != "https"sv) return Error::from_string_literal("Secure cookies require HTTPS");
            cookie.expiry_time = UnixDateTime::latest();
            if (details.has("expirationDate"sv)) {
                auto expiry = details.get_double_with_precision_loss("expirationDate"sv);
                // Reject NaN/infinities as well as values outside the supported calendar.
                if (!expiry.has_value() || !(*expiry >= -62135596800.0 && *expiry <= 253402300799.0))
                    return Error::from_string_literal("Invalid cookie expirationDate");
                cookie.expiry_time = UnixDateTime::from_seconds_since_epoch(static_cast<i64>(*expiry));
                cookie.persistent = true;
            }
            TRY(jar.set_cookie_from_devtools(*url, {}, cookie));
            // The normal cookie policy may reject public-suffix domains or secure
            // shadowing. Report rejection instead of pretending the write succeeded.
            if (cookie.expiry_time > UnixDateTime::now()) {
                bool stored = false;
                for (auto const& candidate : jar.get_all_cookies()) {
                    if (candidate.name == cookie.name && candidate.domain == cookie.domain && candidate.path == cookie.path
                        && candidate.host_only == cookie.host_only && candidate.partition_key == cookie.partition_key
                        && candidate.value == cookie.value && candidate.secure == cookie.secure && candidate.http_only == cookie.http_only)
                        stored = true;
                }
                if (!stored) return Error::from_string_literal("Cookie rejected by browser policy");
            }
            return response(JsonValue {});
        }
        if (request.key == "delete"sv && !name.has_value()) return Error::from_string_literal("Cookie name is required");
        JsonArray result;
        size_t bytes = 0;
        for (auto const& cookie : jar.get_all_cookies()) {
            if ((!all_partitions && cookie.partition_key != partition)
                || !HTTP::Cookie::cookie_matches_url(cookie, *url, *host, HTTP::Cookie::Source::Http)
                || (name.has_value() && cookie.name != *name) || (domain.has_value() && cookie.domain != *domain)
                || (path.has_value() && cookie.path != *path)
                || (request.key == "list"sv && details.get_bool("secure"sv).has_value() && cookie.secure != *details.get_bool("secure"sv))
                || (request.key == "list"sv && details.get_bool("httpOnly"sv).has_value() && cookie.http_only != *details.get_bool("httpOnly"sv))) continue;
            if (request.key == "delete"sv) {
                jar.delete_cookie({ cookie.name, cookie.domain, cookie.host_only, cookie.path, cookie.partition_key });
                continue;
            }
            JsonObject item;
            item.set("name"sv, cookie.name);
            item.set("value"sv, cookie.value);
            item.set("domain"sv, cookie.domain);
            item.set("path"sv, cookie.path);
            item.set("secure"sv, cookie.secure);
            item.set("httpOnly"sv, cookie.http_only);
            item.set("hostOnly"sv, cookie.host_only);
            item.set("session"sv, !cookie.persistent);
            item.set("storeId"sv, store_id);
            item.set("firstPartyDomain"sv, ""sv);
            item.set("sameSite"sv, cookie.same_site == HTTP::Cookie::SameSite::None ? "no_restriction"sv
                : cookie.same_site == HTTP::Cookie::SameSite::Strict ? "strict"sv : cookie.same_site == HTTP::Cookie::SameSite::Lax ? "lax"sv : "unspecified"sv);
            if (cookie.persistent) item.set("expirationDate"sv, cookie.expiry_time.to_seconds_f64());
            if (!cookie.partition_key.is_empty()) {
                JsonObject key;
                auto site_text = TRY(cookie.partition_key.utf16_view().to_utf8());
                key.set("topLevelSite"sv, site_text);
                if (auto site = URL::Parser::basic_parse(site_text); site.has_value())
                    item.set("firstPartyDomain"sv, site->serialized_host());
                item.set("partitionKey"sv, move(key));
            }
            bytes += item.serialized().byte_count();
            if (bytes > 1024 * 1024 || result.size() >= 4096) return Error::from_string_literal("Cookie result exceeds quota");
            TRY(result.append(move(item)));
        }
        return request.key == "list"sv ? response(JsonValue { move(result) }) : response(JsonValue {});
    }
private:
    static ErrorOr<ByteBuffer> response(JsonValue value)
    {
        auto json = value.serialized();
        return Protocol::encode(Protocol::Response { Protocol::Kind::Network, {}, TRY(ByteBuffer::copy(json.bytes())), {}, 0 });
    }
};
}
