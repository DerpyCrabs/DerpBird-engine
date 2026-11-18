/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibURL/Forward.h>

namespace HTTP {

struct Proxy {
    enum class Type : u8 {
        HTTP,
        HTTPS,
        SOCKS4,
        SOCKS4A,
        SOCKS5,
        SOCKS5Hostname,
    };

    static Optional<Proxy> parse(StringView);

    bool resolves_hostnames() const;

    // For CURLOPT_PROXY.
    ByteString to_curl_url() const;

    bool operator==(Proxy const&) const = default;

    Type type { Type::HTTP };
    ByteString host;
    u16 port { 0 };
    ByteString userinfo;
};

// Environment proxies with libcurl precedence and optional ZeroOmega routing.
class ProxyConfiguration {
public:
    using EnvironmentLookup = Function<Optional<StringView>(StringView)>;

    static ProxyConfiguration from_environment();
    static ProxyConfiguration from_environment(EnvironmentLookup const&);

    ProxyConfiguration() = default;
    ProxyConfiguration(Optional<Proxy> http_proxy, Optional<Proxy> https_proxy, Vector<ByteString> no_proxy)
        : m_http_proxy(move(http_proxy))
        , m_https_proxy(move(https_proxy))
        , m_no_proxy(move(no_proxy))
    {
    }

    Optional<Proxy const&> proxy_for(URL::URL const&) const;

    String profile_for(URL::URL const&) const;

    bool is_empty() const { return m_omega_options.size() == 0 && !m_http_proxy.has_value() && !m_https_proxy.has_value(); }

    ErrorOr<void> set_omega(JsonObject, String active_profile);
    JsonObject const& omega_options() const { return m_omega_options; }
    String const& active_profile() const { return m_active_profile; }

    Optional<Proxy> const& http_proxy() const { return m_http_proxy; }
    Optional<Proxy> const& https_proxy() const { return m_https_proxy; }
    Vector<ByteString> const& no_proxy() const { return m_no_proxy; }

private:
    bool is_excluded(StringView host) const;
    Optional<Proxy const&> omega_proxy_for(URL::URL const&, String const&, size_t depth = 0) const;
    JsonObject m_omega_options;
    String m_active_profile;
    HashMap<String, Proxy> m_omega_endpoints;

    Optional<Proxy> m_http_proxy;
    Optional<Proxy> m_https_proxy;
    Vector<ByteString> m_no_proxy;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, HTTP::Proxy const&);

template<>
ErrorOr<HTTP::Proxy> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, HTTP::ProxyConfiguration const&);

template<>
ErrorOr<HTTP::ProxyConfiguration> decode(Decoder&);

}
