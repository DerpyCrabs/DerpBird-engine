/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/JsonArray.h>
#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/kmalloc.h>
#include <LibURL/URL.h>
#include <LibHTTP/HeaderList.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web {

class WEB_API ContentBlocker {
    AK_MAKE_NONCOPYABLE(ContentBlocker);
    AK_MAKE_NONMOVABLE(ContentBlocker);

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class ResourceType : u8 {
        Document,
        Popup,
        Popunder,
        Font,
        Image,
        Media,
        Object,
        Other,
        Ping,
        Script,
        Stylesheet,
        Subdocument,
        WebSocket,
        XMLHttpRequest,
    };

    struct SubstituteResource {
        ByteBuffer body;
        String mime_type;
        bool click_to_load { false };
    };

    struct Decision {
        bool trusted_url_skip { false };
        bool blocked { false };
        String matched_rule {};
        Optional<URL::URL> rewritten_url {};
        Optional<SubstituteResource> substitute {};
    };

    Decision check(URL::URL const&, URL::URL const& source_url, ResourceType, StringView method = {}, Optional<URL::URL> const& top_url = {}, StringView ip_addresses = {}) const;
    static ContentBlocker& the();

    bool has_rules() const { return m_engine.load(AK::MemoryOrder::memory_order_relaxed) != nullptr; }
    bool has_cosmetic_rules() const { return m_has_cosmetic_rules.load(AK::MemoryOrder::memory_order_relaxed); }

    bool is_filtered(URL::URL const&) const;
    bool is_filtered(URL::URL const&, URL::URL const& source_url, ResourceType, StringView method = {}) const;
    bool is_filtered(URL::URL const&, URL::URL const& source_url, Optional<Fetch::Infrastructure::Request::Destination> const&, Optional<Fetch::Infrastructure::Request::InitiatorType> const&, Fetch::Infrastructure::Request::Mode, StringView method = {}) const;
    ErrorOr<void> set_patterns(ReadonlySpan<String>);
    ErrorOr<void> set_rules_from_bytes(ReadonlyBytes);

    Utf16String html_filter_rules(URL::URL const&) const;
    static Utf16String html_filter_runtime();
    bool canonical_url_exempt(URL::URL const&, URL::URL const& source, ResourceType, StringView method, Optional<URL::URL> const& top_url = {}, StringView ip_addresses = {}) const;
    Decision check_canonical_hosts(URL::URL const&, URL::URL const& source, ResourceType, StringView method, StringView names, Optional<URL::URL> const& top_url = {}, StringView ip_addresses = {}) const;
    bool has_header_filters() const;
    bool has_request_header_filters() const;
    bool has_ip_address_filters() const;
    Decision check_ip_address(URL::URL const&, URL::URL const& source, ResourceType, StringView method, StringView address, Optional<URL::URL> const& top_url = {}) const;
    Decision check_request_headers(URL::URL const&, URL::URL const& source, ResourceType, StringView method, HTTP::HeaderList const&, Optional<URL::URL> const& top_url = {}, StringView ip_addresses = {}) const;
    Decision check_response(URL::URL const&, URL::URL const& source, ResourceType, StringView method, HTTP::HeaderList const&, Optional<URL::URL> const& top_url = {}, StringView ip_addresses = {}) const;
    bool should_block_response(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, HTTP::HeaderList const& headers) const { return check_response(url, source, type, method, headers).blocked; }
    Vector<String> action_rules(URL::URL const&, URL::URL const& source, ResourceType, StringView method, Optional<URL::URL> const& top_url, u8 action, StringView ip_addresses = {}) const;
    u8 response_header_removals(URL::URL const&, ResourceType) const;
    String response_modifiers(URL::URL const&, URL::URL const& source, ResourceType, StringView method, bool permissions, Optional<URL::URL> const& top_url = {}, StringView ip_addresses = {}) const;
    JsonArray response_replacements(URL::URL const&, URL::URL const& source, ResourceType, StringView method, Optional<URL::URL> const& top_url = {}, StringView ip_addresses = {}) const;
    String csp_for_response(URL::URL const&, URL::URL const& source_url, ResourceType, StringView method = {}, Optional<URL::URL> const& top_url = {}, StringView ip_addresses = {}) const;
    Utf16String scriptlets_for_url(URL::URL const&, bool isolated = false) const;
    Utf16String cosmetic_style_sheet_for_url(URL::URL const&) const;
    Utf16String cosmetic_style_sheet_for_url(URL::URL const&, ReadonlySpan<Utf16FlyString> classes, ReadonlySpan<Utf16FlyString> ids) const;
    bool has_generic_cosmetic_selectors_for_url(URL::URL const&, ReadonlySpan<Utf16FlyString> classes, ReadonlySpan<Utf16FlyString> ids) const;

    static ResourceType resource_type_from_fetch_metadata(Optional<Fetch::Infrastructure::Request::Destination> const&, Optional<Fetch::Infrastructure::Request::InitiatorType> const&, Fetch::Infrastructure::Request::Mode);
    static URL::URL source_url_for_matching(URL::URL const&);

private:
    ContentBlocker();
    ~ContentBlocker();

    // Agents on any thread ask, and the engine is not one that several threads can use at once. The atomics let a check
    // for no rules at all skip the lock.
    mutable Mutex m_mutex;
    Atomic<bool> m_has_cosmetic_rules { false };
    Atomic<void*> m_engine { nullptr };
};

}
