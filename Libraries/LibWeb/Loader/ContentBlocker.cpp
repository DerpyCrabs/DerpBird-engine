#include <LibWeb/Page/DerpBirdFilterPreprocessor.h>
#include <LibWeb/Page/DerpBirdResponseReplacement.h>
#include <AK/JsonValue.h>
#include <AK/JsonObject.h>
#include <AK/JsonArray.h>
/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/Utf16String.h>
#include <AK/NeverDestroyed.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <mutex>
#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>
#include <LibTextCodec/Decoder.h>
#include <LibURL/Parser.h>
#include <LibWeb/ContentBlockerRustFFI.h>
#include <LibWeb/Loader/ContentBlocker.h>
#include <LibWebCommon/Fetch/Infrastructure/DataURL.h>

namespace Web {

ContentBlocker& ContentBlocker::the()
{
    static ContentBlocker& blocker = *new ContentBlocker;
    return blocker;
}

// A bounded cache shared by all engines; compilation and VM execution are serialized.
// Fast linear Rust regexes never cross this bridge.
static i8 evaluate_host_regex(u8 const* source, size_t source_length, bool ignore_case, u8 const* subject, size_t subject_length, u8 mode, u64* remaining, u8* output, size_t output_capacity, size_t* output_length)
{
    if (source_length > 4096 || subject_length > 16384)
        return -2;
    // Conservatively cap compilation expansion as well as matching. Nested
    // counted repetitions otherwise allocate bytecode before the VM budget applies.
    size_t expansion = max(source_length, static_cast<size_t>(1));
    size_t depth = 0;
    bool in_class = false;
    for (size_t i = 0; i < source_length; ++i) {
        auto ch = source[i];
        if (ch == '\\') { ++i; continue; }
        if (ch == '[') in_class = true;
        if (ch == ']') in_class = false;
        if (in_class) continue;
        if (ch == '(' && ++depth > 64) return -2;
        if (ch == ')' && depth) --depth;
        if (ch != '{') continue;
        size_t count = 0;
        size_t largest = 1;
        for (size_t j = i + 1; j < source_length; ++j) {
            if (source[j] >= '0' && source[j] <= '9') {
                count = count * 10 + source[j] - '0';
                if (count > 16384) return -2;
                largest = max(largest, count);
            } else if (source[j] == ',') {
                count = 0;
            } else {
                break;
            }
        }
        if (expansion > 16384 / largest) return -2;
        expansion *= largest;
    }
    struct Entry { String pattern; bool ignore_case; Optional<regex::ECMAScriptRegex> compiled; };
    static std::mutex mutex;
    static NeverDestroyed<Vector<Entry>> storage;
    auto& cache = *storage;
    std::lock_guard lock(mutex);
    auto pattern = String::from_utf8(StringView { source, source_length });
    if (pattern.is_error())
        return -2;
    Entry* entry = nullptr;
    for (auto& candidate : cache) {
        if (candidate.pattern == pattern.value() && candidate.ignore_case == ignore_case) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) {
        if (cache.size() >= 64)
            cache.remove(0);
        auto utf16 = Utf16String::from_utf8(pattern.value());
        auto compiled = regex::ECMAScriptRegex::compile(utf16.utf16_view(), { .ignore_case = ignore_case });
        Optional<regex::ECMAScriptRegex> value;
        if (!compiled.is_error())
            value = compiled.release_value();
        cache.append(Entry { pattern.release_value(), ignore_case, move(value) });
        entry = &cache.last();
    }
    if (!entry->compiled.has_value())
        return -2;
    if (mode == 1)
        return 1;
    auto input = String::from_utf8(StringView { subject, subject_length });
    if (input.is_error())
        return -2;
    auto utf16 = Utf16String::from_utf8(input.value());
    auto view = utf16.utf16_view();
    // Keep the real subject and offsets: wrapping/slicing changes anchors and lookbehind.
    for (size_t offset = 0; offset <= view.length_in_code_units(); ++offset) {
        if (*remaining == 0)
            return -1;
        --*remaining;
        switch (entry->compiled->test_anchored_budgeted(view, offset, *remaining)) {
        case regex::MatchResult::Match:
            // Modes: 0 test, 1 validate, 2 first capture, 3 full-match byte range.
            if (mode == 3) {
                if (!output || !output_length || output_capacity < 16) return -2;
                auto start = entry->compiled->capture_slot(0);
                auto end = entry->compiled->capture_slot(1);
                if (start < 0 || end < start || static_cast<size_t>(end) > view.length_in_code_units()) return -2;
                auto prefix = view.substring_view(0, start).to_utf8();
                auto matched = view.substring_view(start, end - start).to_utf8();
                if (prefix.is_error() || matched.is_error()) return -2;
                u64 first = prefix.value().bytes().size();
                u64 last = first + matched.value().bytes().size();
                for (size_t i = 0; i < 8; ++i) { output[i] = (first >> (8 * i)) & 255; output[8 + i] = (last >> (8 * i)) & 255; }
                *output_length = 16;
                return 1;
            }
            if (output) {
                if (!output_length || entry->compiled->capture_count() < 1) return -2;
                auto start = entry->compiled->capture_slot(2);
                auto end = entry->compiled->capture_slot(3);
                if (start < 0 || end < start || static_cast<size_t>(end) > view.length_in_code_units()) return -2;
                auto capture = view.substring_view(start, end - start).to_utf8();
                if (capture.is_error() || capture.value().bytes().size() > output_capacity) return -2;
                *output_length = capture.value().bytes().size();
                memcpy(output, capture.value().bytes().data(), *output_length);
            }
            return 1;
        case regex::MatchResult::LimitExceeded: return -1;
        case regex::MatchResult::NoMatch: break;
        }
    }
    return 0;
}

ContentBlocker::ContentBlocker()
{
    ContentBlocking::FFI::rust_content_blocker_install_regex(evaluate_host_regex);
}

ContentBlocker::~ContentBlocker()
{
    ContentBlocking::FFI::rust_content_blocker_free(m_engine);
}

static StringView resource_type_to_adblock_request_type(ContentBlocker::ResourceType type)
{
    switch (type) {
    case ContentBlocker::ResourceType::Document:
        return "document"sv;
    case ContentBlocker::ResourceType::Popunder:
        return "popunder"sv;
    case ContentBlocker::ResourceType::Popup:
        return "popup"sv;
    case ContentBlocker::ResourceType::Font:
        return "font"sv;
    case ContentBlocker::ResourceType::Image:
        return "image"sv;
    case ContentBlocker::ResourceType::Media:
        return "media"sv;
    case ContentBlocker::ResourceType::Object:
        return "object"sv;
    case ContentBlocker::ResourceType::Other:
        return "other"sv;
    case ContentBlocker::ResourceType::Ping:
        return "ping"sv;
    case ContentBlocker::ResourceType::Script:
        return "script"sv;
    case ContentBlocker::ResourceType::Stylesheet:
        return "stylesheet"sv;
    case ContentBlocker::ResourceType::Subdocument:
        return "subdocument"sv;
    case ContentBlocker::ResourceType::WebSocket:
        return "websocket"sv;
    case ContentBlocker::ResourceType::XMLHttpRequest:
        return "xmlhttprequest"sv;
    }
    VERIFY_NOT_REACHED();
}

static ByteString serialized_url(URL::URL const& url)
{
    return url.serialize().to_byte_string();
}

static ByteString serialized_url_for_matching(URL::URL const& url)
{
    if (url.scheme() == "file"sv)
        return ByteString::formatted("http://local-file.invalid/{}", serialized_url(url));
    return serialized_url(url);
}

static Utf16String take_rust_utf16_string(ContentBlocking::FFI::ContentBlockerString rust_string)
{
    if (!rust_string.data)
        return {};

    ArmedScopeGuard free_string = [&] {
        ContentBlocking::FFI::rust_content_blocker_free_string(rust_string.data, rust_string.length);
    };

    auto maybe_string = Utf16String::try_from_utf8({ reinterpret_cast<char const*>(rust_string.data), rust_string.length });
    if (maybe_string.is_error())
        return {};
    return maybe_string.release_value();
}

static ErrorOr<String> join_lines(ReadonlySpan<Utf16FlyString> lines)
{
    StringBuilder builder;
    for (auto const& line : lines) {
        builder.append(TRY(line.view().to_utf8()));
        builder.append('\n');
    }
    return builder.to_string();
}

static bool line_looks_like_supported_cosmetic_rule(StringView line)
{
    auto trimmed_line = line.trim_whitespace();
    if (trimmed_line.is_empty() || trimmed_line.starts_with('!') || trimmed_line.starts_with('['))
        return false;

    auto sharp_index = trimmed_line.find('#');
    if (!sharp_index.has_value())
        return false;

    auto after_sharp_index = *sharp_index + 1;
    if (after_sharp_index >= trimmed_line.length())
        return false;

    auto second_sharp_index = trimmed_line.find('#', after_sharp_index);
    if (!second_sharp_index.has_value())
        return false;

    auto between_sharps = trimmed_line.substring_view(after_sharp_index, *second_sharp_index - after_sharp_index);
    if (between_sharps.starts_with('@')) {
        if (*sharp_index == 0)
            return false;
        between_sharps = between_sharps.substring_view(1);
    }
    if (between_sharps.starts_with('?'))
        between_sharps = between_sharps.substring_view(1);

    return between_sharps.is_empty();
}

static bool rules_contain_cosmetic_rules(ReadonlyBytes rules_bytes)
{
    if (StringView { rules_bytes }.trim_whitespace().starts_with('{')) {
        auto bundle = JsonValue::from_string(StringView { rules_bytes });
        if (bundle.is_error() || !bundle.value().is_object()) return false;
        auto lists = bundle.value().as_object().get_array("lists"sv);
        if (!lists.has_value()) return false;
        for (auto const& list : lists->values()) {
            if (!list.is_object()) continue;
            auto text = list.as_object().get_string("text"sv);
            if (!text.has_value()) continue;
            bool found = false;
            text->bytes_as_string_view().for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView line) {
                if (line_looks_like_supported_cosmetic_rule(line)) found = true;
            });
            if (found) return true;
        }
        return false;
    }
    bool has_cosmetic_rules = false;
    StringView { rules_bytes }.for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView line) {
        if (line_looks_like_supported_cosmetic_rule(line))
            has_cosmetic_rules = true;
    });
    return has_cosmetic_rules;
}

ErrorOr<void> ContentBlocker::set_patterns(ReadonlySpan<String> patterns)
{
    StringBuilder builder;
    for (auto const& pattern : patterns) {
        if (pattern.is_empty())
            continue;
        builder.append(pattern);
        builder.append('\n');
    }

    auto patterns_string = TRY(builder.to_string());
    auto patterns_bytes = patterns_string.bytes_as_string_view().bytes();
    return set_rules_from_bytes(patterns_bytes);
}

ErrorOr<void> ContentBlocker::set_rules_from_bytes(ReadonlyBytes rules_bytes)
{
    // OPTIMIZATION: Building an engine costs the same tokenizing and flatbuffer work whether or not there are any
    //               rules to put in it, and every caller already treats a missing engine as "nothing is blocked".
    //               Drop the engine instead of rebuilding an empty one.
    if (rules_bytes.is_empty()) {
        ContentBlocking::FFI::rust_content_blocker_free(m_engine);
        m_engine = nullptr;
        m_has_cosmetic_rules = false;
        return {};
    }

    String prepared;
    auto input = StringView { rules_bytes };
    if (input.trim_whitespace().starts_with('{')) {
        auto bundle = TRY(JsonValue::from_string(input));
        if (!bundle.is_object()) return Error::from_string_literal("Invalid filter bundle");
        auto lists = bundle.as_object().get_array("lists"sv);
        if (!lists.has_value()) return Error::from_string_literal("Missing filter lists");
        JsonArray processed;
        for (auto const& value : lists->values()) {
            if (!value.is_object()) return Error::from_string_literal("Invalid filter source");
            auto item = value.as_object();
            auto text = item.get_string("text"sv);
            if (!text.has_value()) return Error::from_string_literal("Missing filter text");
            item.set("text"sv, TRY(preprocess_derpbird_filter_list(text->bytes_as_string_view())));
            TRY(processed.append(move(item)));
        }
        bundle.as_object().set("lists"sv, move(processed));
        prepared = bundle.serialized();
    } else {
        prepared = TRY(preprocess_derpbird_filter_list(input));
    }
    rules_bytes = prepared.bytes();
    auto* engine = ContentBlocking::FFI::rust_content_blocker_create(rules_bytes);
    if (!engine)
        return Error::from_string_literal("Failed to create content blocker");

    auto has_cosmetic_rules = rules_contain_cosmetic_rules(rules_bytes);

    ContentBlocking::FFI::rust_content_blocker_free(m_engine);
    m_engine = engine;
    m_has_cosmetic_rules = has_cosmetic_rules;
    return {};
}

bool ContentBlocker::is_filtered(URL::URL const& url) const
{
    return is_filtered(url, url, ResourceType::Other);
}

bool ContentBlocker::is_filtered(URL::URL const& url, URL::URL const& source_url, Optional<Fetch::Infrastructure::Request::Destination> const& destination, Optional<Fetch::Infrastructure::Request::InitiatorType> const& initiator_type, Fetch::Infrastructure::Request::Mode mode, StringView method) const
{
    return is_filtered(url, source_url_for_matching(source_url), resource_type_from_fetch_metadata(destination, initiator_type, mode), method);
}

bool ContentBlocker::is_filtered(URL::URL const& url, URL::URL const& source_url, ResourceType resource_type, StringView method) const
{
    return check(url, source_url, resource_type, method).blocked;
}

static ByteString serialized_top_context(URL::URL const& url, URL::URL const& source, ContentBlocker::ResourceType type, Optional<URL::URL> const& top)
{
    // A top-level navigation establishes its own context, including redirects.
    auto context = type == ContentBlocker::ResourceType::Document ? url : top.value_or(source);
    return serialized_url_for_matching(ContentBlocker::source_url_for_matching(context));
}

ContentBlocker::Decision ContentBlocker::check(URL::URL const& url, URL::URL const& source_url, ResourceType resource_type, StringView method, Optional<URL::URL> const& top_url, StringView ip_addresses) const
{
    if (!m_engine)
        return {};

    if (url.scheme() == "data"sv)
        return {};

    auto url_string = serialized_url_for_matching(url);
    auto normalized_source_url = source_url_for_matching(source_url);
    auto source_url_string = serialized_url_for_matching(normalized_source_url);
    auto request_type = resource_type_to_adblock_request_type(resource_type);

    auto top_context = serialized_top_context(url, source_url, resource_type, top_url);
    auto result = ContentBlocking::FFI::rust_content_blocker_check(
        m_engine,
        url_string.bytes(),
        source_url_string.bytes(),
        request_type.bytes(),
        method.bytes(), top_context.bytes(), ip_addresses.bytes());
    ScopeGuard free_url = [&] {
        ContentBlocking::FFI::rust_content_blocker_free_string(result.matched_rule.data, result.matched_rule.length);
        ContentBlocking::FFI::rust_content_blocker_free_string(result.rewritten_url.data, result.rewritten_url.length);
        ContentBlocking::FFI::rust_content_blocker_free_string(result.redirect_data_url.data, result.redirect_data_url.length);
    };
    Decision decision { .blocked = result.blocked };
    if (result.matched_rule.length)
        decision.matched_rule = MUST(String::from_utf8({ result.matched_rule.data, result.matched_rule.length }));
    if (result.blocked && result.redirect_data_url.length && url.scheme().is_one_of("http"sv, "https"sv)) {
        auto data_url = URL::Parser::basic_parse(StringView { result.redirect_data_url.data, result.redirect_data_url.length });
        if (data_url.has_value() && data_url->scheme() == "data"sv) {
            auto data = Fetch::Infrastructure::process_data_url(*data_url);
            if (!data.is_error()) {
                auto mime = data.value().mime_type.serialized();
                bool click_to_load = mime == "application/x-derpbird-click-to-load"sv;
                if (!click_to_load || resource_type == ResourceType::Subdocument)
                    decision.substitute = SubstituteResource { move(data.value().body), click_to_load ? "text/html"_string : move(mime), click_to_load };
            }
        }
    }
    if (!result.blocked && url.scheme().is_one_of("http"sv, "https"sv)) {
        auto transforms = ContentBlocking::FFI::rust_content_blocker_uri_transforms(m_engine, url_string.bytes(), source_url_string.bytes(), request_type.bytes(), method.bytes(), top_context.bytes(), ip_addresses.bytes());
        ScopeGuard free_transforms = [&] { ContentBlocking::FFI::rust_content_blocker_free_string(transforms.data, transforms.length); };
        if (transforms.length && transforms.length <= 512 * KiB) {
            auto parsed = JsonValue::from_string(StringView { transforms.data, transforms.length });
            if (!parsed.is_error() && parsed.value().is_array()) {
                for (auto const& operation : parsed.value().as_array().values()) {
                    JsonArray single;
                    single.must_append(operation);
                    auto transformed = replace_derpbird_response(Utf16String::from_utf8(url.serialize()), single);
                    if (transformed.is_error()) continue;
                    auto text = transformed.value().to_utf8();
                    if (text.bytes().size() > 16384) continue;
                    auto target = URL::Parser::basic_parse(text);
                    if (!target.has_value() || target == url || target->includes_credentials() || !target->scheme().is_one_of("http"sv, "https"sv)) continue;
                    decision.rewritten_url = target.release_value();
                    decision.trusted_url_skip = true;
                    decision.matched_rule = operation.as_object().get_string("matched_rule"sv).value_or("uritransform"_string);
                    return decision;
                }
            }
        }
    }
    if (result.blocked || !result.rewritten_url.length || !url.scheme().is_one_of("http"sv, "https"sv))
        return decision;
    auto rewritten = URL::Parser::basic_parse(StringView { result.rewritten_url.data, result.rewritten_url.length });
    if (!rewritten.has_value() || rewritten == url)
        return decision;
    if (result.trusted_url_skip && rewritten->scheme().is_one_of("http"sv, "https"sv)) {
        decision.trusted_url_skip = true;
        decision.rewritten_url = rewritten.release_value();
        return decision;
    }
    // removeparam may change only the query. Never accept a different origin,
    // path, credentials or fragment from the filtering engine.
    auto query_only = url;
    query_only.set_query(rewritten->query());
    if (query_only == *rewritten)
        decision.rewritten_url = rewritten.release_value();
    return decision;
}

ContentBlocker::Decision ContentBlocker::check_canonical_hosts(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, StringView names, Optional<URL::URL> const& top_url, StringView ip_addresses) const
{
    if (canonical_url_exempt(url, source, type, method, top_url, ip_addresses)) return {};
    size_t count = 0;
    Decision combined;
    auto effective_url = url;
    for (auto name : names.split_view('\n')) {
        if (name.is_empty()) continue;
        auto parsed = URL::Parser::basic_parse(MUST(String::formatted("https://{}/", name)));
        if (++count > 16 || names.length() > 4096 || !parsed.has_value() || !parsed->host().has_value()
            || parsed->includes_credentials() || !parsed->serialized_host().equals_ignoring_ascii_case(name)) {
            Decision rejected;
            rejected.blocked = true;
            rejected.matched_rule = "Invalid cached canonical provenance"_string;
            return rejected;
        }
        auto alias = effective_url;
        alias.set_host(*parsed->host());
        auto decision = check(alias, source, type, method, top_url, ip_addresses);
        if (decision.blocked) {
            decision.matched_rule = MUST(String::formatted("{}\n! CNAME: {}", decision.matched_rule, name));
            return decision;
        }
        if (decision.rewritten_url.has_value()) {
            // DNS aliases only select rules; never navigate to the alias host.
            if (decision.trusted_url_skip) {
                decision.matched_rule = MUST(String::formatted("{}\n! CNAME: {}", decision.matched_rule, name));
                return decision;
            }
            effective_url.set_query(decision.rewritten_url->query());
            combined.rewritten_url = effective_url;
            combined.matched_rule = MUST(String::formatted("{}\n! CNAME: {}", decision.matched_rule, name));
        }
    }
    return combined;
}

bool ContentBlocker::canonical_url_exempt(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, Optional<URL::URL> const& top_url, StringView ip_addresses) const
{
    if (!m_engine) return true;
    auto address = serialized_url_for_matching(url);
    auto source_address = serialized_url_for_matching(source_url_for_matching(source));
    auto request_type = resource_type_to_adblock_request_type(type);
    auto top_context = serialized_top_context(url, source, type, top_url);
    return ContentBlocking::FFI::rust_content_blocker_canonical_url_exempt(m_engine, address.bytes(), source_address.bytes(), request_type.bytes(), method.bytes(), top_context.bytes(), ip_addresses.bytes());
}

Utf16String ContentBlocker::html_filter_rules(URL::URL const& url) const
{
    if (!m_engine || !url.scheme().is_one_of("http"sv, "https"sv)) return {};
    auto address = serialized_url_for_matching(url);
    return take_rust_utf16_string(ContentBlocking::FFI::rust_content_blocker_html_rules(m_engine, address.bytes()));
}

Utf16String ContentBlocker::html_filter_runtime()
{
    return take_rust_utf16_string(ContentBlocking::FFI::rust_content_blocker_html_runtime());
}

bool ContentBlocker::has_request_header_filters() const
{
    return m_engine && ContentBlocking::FFI::rust_content_blocker_has_request_header_filters(m_engine);
}

bool ContentBlocker::has_header_filters() const
{
    return m_engine && ContentBlocking::FFI::rust_content_blocker_has_header_filters(m_engine);
}

ContentBlocker::Decision ContentBlocker::check_response(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, HTTP::HeaderList const& headers, Optional<URL::URL> const& top_url, StringView ip_addresses) const
{
    if (!has_header_filters()) return {};
    JsonArray pairs;
    for (auto const& header : headers) {
        JsonArray pair;
        pair.must_append(TextCodec::isomorphic_decode(header.name));
        pair.must_append(TextCodec::isomorphic_decode(header.value));
        pairs.must_append(move(pair));
    }
    auto serialized = pairs.serialized();
    auto address = serialized_url_for_matching(url);
    auto origin = serialized_url_for_matching(source_url_for_matching(source));
    auto top_context = serialized_top_context(url, source, type, top_url);
    auto result = ContentBlocking::FFI::rust_content_blocker_block_response(m_engine, address.bytes(), origin.bytes(), resource_type_to_adblock_request_type(type).bytes(), method.bytes(), serialized.bytes(), top_context.bytes(), ip_addresses.bytes());
    ScopeGuard release = [&] {
        ContentBlocking::FFI::rust_content_blocker_free_string(result.matched_rule.data, result.matched_rule.length);
        ContentBlocking::FFI::rust_content_blocker_free_string(result.rewritten_url.data, result.rewritten_url.length);
        ContentBlocking::FFI::rust_content_blocker_free_string(result.redirect_data_url.data, result.redirect_data_url.length);
    };
    Decision decision { .blocked = result.blocked };
    if (result.matched_rule.length)
        decision.matched_rule = MUST(String::from_utf8({ result.matched_rule.data, result.matched_rule.length }));
    return decision;
}

ContentBlocker::Decision ContentBlocker::check_request_headers(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, HTTP::HeaderList const& headers, Optional<URL::URL> const& top_url, StringView ip_addresses) const
{
    if (!m_engine || !ContentBlocking::FFI::rust_content_blocker_has_request_header_filters(m_engine)) return {};
    JsonArray pairs;
    for (auto const& header : headers) {
        JsonArray pair;
        pair.must_append(TextCodec::isomorphic_decode(header.name));
        pair.must_append(TextCodec::isomorphic_decode(header.value));
        pairs.must_append(move(pair));
    }
    auto serialized = pairs.serialized();
    auto address = serialized_url_for_matching(url);
    auto origin = serialized_url_for_matching(source_url_for_matching(source));
    auto top_context = serialized_top_context(url, source, type, top_url);
    auto result = ContentBlocking::FFI::rust_content_blocker_block_request_headers(m_engine, address.bytes(), origin.bytes(), resource_type_to_adblock_request_type(type).bytes(), method.bytes(), serialized.bytes(), top_context.bytes(), ip_addresses.bytes());
    ScopeGuard release = [&] {
        ContentBlocking::FFI::rust_content_blocker_free_string(result.matched_rule.data, result.matched_rule.length);
        ContentBlocking::FFI::rust_content_blocker_free_string(result.rewritten_url.data, result.rewritten_url.length);
        ContentBlocking::FFI::rust_content_blocker_free_string(result.redirect_data_url.data, result.redirect_data_url.length);
    };
    Decision decision { .blocked = result.blocked };
    if (result.matched_rule.length)
        decision.matched_rule = MUST(String::from_utf8({ result.matched_rule.data, result.matched_rule.length }));
    return decision;
}

bool ContentBlocker::has_ip_address_filters() const
{
    return m_engine && ContentBlocking::FFI::rust_content_blocker_has_ip_address_filters(m_engine);
}

ContentBlocker::Decision ContentBlocker::check_ip_address(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, StringView ip_address, Optional<URL::URL> const& top_url) const
{
    if (!m_engine || !ContentBlocking::FFI::rust_content_blocker_has_ip_address_filters(m_engine)) return {};
    auto address = serialized_url_for_matching(url);
    auto origin = serialized_url_for_matching(source_url_for_matching(source));
    auto top_context = serialized_top_context(url, source, type, top_url);
    auto result = ContentBlocking::FFI::rust_content_blocker_check_ip_address(m_engine, address.bytes(), origin.bytes(), resource_type_to_adblock_request_type(type).bytes(), method.bytes(), ip_address.bytes(), top_context.bytes());
    ScopeGuard release = [&] {
        ContentBlocking::FFI::rust_content_blocker_free_string(result.matched_rule.data, result.matched_rule.length);
        ContentBlocking::FFI::rust_content_blocker_free_string(result.rewritten_url.data, result.rewritten_url.length);
        ContentBlocking::FFI::rust_content_blocker_free_string(result.redirect_data_url.data, result.redirect_data_url.length);
    };
    Decision decision { .blocked = result.blocked };
    if (result.matched_rule.length)
        decision.matched_rule = MUST(String::from_utf8({ result.matched_rule.data, result.matched_rule.length }));
    return decision;
}

Vector<String> ContentBlocker::action_rules(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, Optional<URL::URL> const& top_url, u8 action, StringView ip_addresses) const
{
    Vector<String> rules;
    if (!m_engine || !url.scheme().is_one_of("http"sv, "https"sv)) return rules;
    auto address = serialized_url_for_matching(url);
    auto initiator = serialized_url_for_matching(source_url_for_matching(source));
    auto kind = resource_type_to_adblock_request_type(type);
    auto top = serialized_top_context(url, source, type, top_url);
    auto result = ContentBlocking::FFI::rust_content_blocker_action_rules(m_engine, address.bytes(), initiator.bytes(), kind.bytes(), method.bytes(), top.bytes(), action, ip_addresses.bytes());
    ScopeGuard release = [&] { ContentBlocking::FFI::rust_content_blocker_free_string(result.data, result.length); };
    if (!result.length || result.length > 1 * MiB) return rules;
    auto value = JsonValue::from_string(StringView { result.data, result.length });
    if (value.is_error() || !value.value().is_array()) return rules;
    for (auto const& rule : value.value().as_array().values()) {
        if (rule.is_string()) rules.append(rule.as_string());
    }
    return rules;
}

u8 ContentBlocker::response_header_removals(URL::URL const& url, ResourceType type) const
{
    if (!m_engine || !url.scheme().is_one_of("http"sv, "https"sv)
        || (type != ResourceType::Document && type != ResourceType::Subdocument)) return 0;
    auto address = serialized_url_for_matching(url);
    return ContentBlocking::FFI::rust_content_blocker_response_headers(m_engine, address.bytes()) & 15;
}

String ContentBlocker::response_modifiers(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, bool permissions, Optional<URL::URL> const& top_url, StringView ip_addresses) const
{
    if (!m_engine || !url.scheme().is_one_of("http"sv, "https"sv)) return {};
    auto address = serialized_url_for_matching(url);
    auto initiator = serialized_url_for_matching(source_url_for_matching(source));
    auto kind = resource_type_to_adblock_request_type(type);
    auto top_context = serialized_top_context(url, source, type, top_url);
    auto result = ContentBlocking::FFI::rust_content_blocker_response_modifiers(m_engine, address.bytes(), initiator.bytes(), kind.bytes(), method.bytes(), permissions, top_context.bytes(), ip_addresses.bytes());
    ScopeGuard release = [&] { ContentBlocking::FFI::rust_content_blocker_free_string(result.data, result.length); };
    if (!result.length || result.length > 512 * KiB) return {};
    auto text = String::from_utf8(StringView { result.data, result.length });
    if (text.is_error() || (permissions && !HTTP::is_header_value(text.value()))) return {};
    return text.release_value();
}

JsonArray ContentBlocker::response_replacements(URL::URL const& url, URL::URL const& source, ResourceType type, StringView method, Optional<URL::URL> const& top_url, StringView ip_addresses) const
{
    auto text = response_modifiers(url, source, type, method, false, top_url, ip_addresses);
    if (text.is_empty()) return {};
    auto value = JsonValue::from_string(text);
    if (value.is_error() || !value.value().is_array()) return {};
    return value.value().as_array();
}

String ContentBlocker::csp_for_response(URL::URL const& url, URL::URL const& source_url, ResourceType type, StringView method, Optional<URL::URL> const& top_url, StringView ip_addresses) const
{
    if (!m_engine || !url.scheme().is_one_of("http"sv, "https"sv)
        || (type != ResourceType::Document && type != ResourceType::Subdocument))
        return {};
    auto address = serialized_url_for_matching(url);
    auto source = serialized_url_for_matching(source_url_for_matching(source_url));
    auto request_type = resource_type_to_adblock_request_type(type);
    auto top_context = serialized_top_context(url, source_url, type, top_url);
    auto result = ContentBlocking::FFI::rust_content_blocker_csp(m_engine, address.bytes(), source.bytes(), request_type.bytes(), method.bytes(), top_context.bytes(), ip_addresses.bytes());
    ScopeGuard release = [&] { ContentBlocking::FFI::rust_content_blocker_free_string(result.data, result.length); };
    if (!result.length || result.length > 64 * KiB)
        return {};
    StringView value { result.data, result.length };
    if (!HTTP::is_header_value(value))
        return {};
    auto text = String::from_utf8(value);
    if (text.is_error())
        return {};
    return text.release_value();
}

Utf16String ContentBlocker::scriptlets_for_url(URL::URL const& url, bool isolated) const
{
    if (!m_engine || !url.scheme().is_one_of("http"sv, "https"sv))
        return {};
    auto address = serialized_url(url);
    return take_rust_utf16_string(ContentBlocking::FFI::rust_content_blocker_scriptlets(m_engine, address.bytes(), isolated));
}

Utf16String ContentBlocker::cosmetic_style_sheet_for_url(URL::URL const& url) const
{
    return cosmetic_style_sheet_for_url(url, {}, {});
}

Utf16String ContentBlocker::cosmetic_style_sheet_for_url(URL::URL const& url, ReadonlySpan<Utf16FlyString> classes, ReadonlySpan<Utf16FlyString> ids) const
{
    if (!m_engine || !m_has_cosmetic_rules)
        return {};

    auto url_string = serialized_url(url);
    auto classes_string = join_lines(classes);
    if (classes_string.is_error())
        return {};

    auto ids_string = join_lines(ids);
    if (ids_string.is_error())
        return {};

    auto classes_bytes = classes_string.value().bytes_as_string_view();
    auto ids_bytes = ids_string.value().bytes_as_string_view();

    return take_rust_utf16_string(ContentBlocking::FFI::rust_content_blocker_cosmetic_css(
        m_engine,
        url_string.bytes(),
        classes_bytes.bytes(),
        ids_bytes.bytes()));
}

bool ContentBlocker::has_generic_cosmetic_selectors_for_url(URL::URL const& url, ReadonlySpan<Utf16FlyString> classes, ReadonlySpan<Utf16FlyString> ids) const
{
    if (!m_engine || !m_has_cosmetic_rules)
        return false;

    auto url_string = serialized_url(url);
    auto classes_string = join_lines(classes);
    if (classes_string.is_error())
        return false;

    auto ids_string = join_lines(ids);
    if (ids_string.is_error())
        return false;

    auto classes_bytes = classes_string.value().bytes_as_string_view();
    auto ids_bytes = ids_string.value().bytes_as_string_view();

    return ContentBlocking::FFI::rust_content_blocker_has_generic_cosmetic_selectors(
        m_engine,
        url_string.bytes(),
        classes_bytes.bytes(),
        ids_bytes.bytes());
}

ContentBlocker::ResourceType ContentBlocker::resource_type_from_fetch_metadata(Optional<Fetch::Infrastructure::Request::Destination> const& destination, Optional<Fetch::Infrastructure::Request::InitiatorType> const& initiator_type, Fetch::Infrastructure::Request::Mode mode)
{
    using Fetch::Infrastructure::Request;

    if (mode == Request::Mode::WebSocket)
        return ResourceType::WebSocket;

    if (destination.has_value()) {
        switch (*destination) {
        case Request::Destination::Audio:
        case Request::Destination::Track:
        case Request::Destination::Video:
            return ResourceType::Media;
        case Request::Destination::Document:
            return ResourceType::Document;
        case Request::Destination::Embed:
        case Request::Destination::Object:
            return ResourceType::Object;
        case Request::Destination::Font:
            return ResourceType::Font;
        case Request::Destination::Frame:
        case Request::Destination::IFrame:
            return ResourceType::Subdocument;
        case Request::Destination::Image:
            return ResourceType::Image;
        case Request::Destination::AudioWorklet:
        case Request::Destination::PaintWorklet:
        case Request::Destination::Script:
        case Request::Destination::ServiceWorker:
        case Request::Destination::SharedWorker:
        case Request::Destination::Worker:
            return ResourceType::Script;
        case Request::Destination::Style:
            return ResourceType::Stylesheet;
        case Request::Destination::Text:
            return ResourceType::Other;
        case Request::Destination::JSON:
        case Request::Destination::Manifest:
        case Request::Destination::Report:
        case Request::Destination::WebIdentity:
        case Request::Destination::XSLT:
            return ResourceType::Other;
        }
        VERIFY_NOT_REACHED();
    }

    if (initiator_type.has_value()) {
        switch (*initiator_type) {
        case Request::InitiatorType::Audio:
        case Request::InitiatorType::Video:
        case Request::InitiatorType::Track:
            return ResourceType::Media;
        case Request::InitiatorType::Beacon:
        case Request::InitiatorType::Ping:
            return ResourceType::Ping;
        case Request::InitiatorType::Embed:
        case Request::InitiatorType::Object:
            return ResourceType::Object;
        case Request::InitiatorType::Fetch:
        case Request::InitiatorType::XMLHttpRequest:
            return ResourceType::XMLHttpRequest;
        case Request::InitiatorType::Font:
            return ResourceType::Font;
        case Request::InitiatorType::Frame:
        case Request::InitiatorType::IFrame:
            return ResourceType::Subdocument;
        case Request::InitiatorType::Image:
        case Request::InitiatorType::IMG:
            return ResourceType::Image;
        case Request::InitiatorType::Script:
            return ResourceType::Script;
        case Request::InitiatorType::CSS:
            return ResourceType::Stylesheet;
        case Request::InitiatorType::EarlyHint:
        case Request::InitiatorType::Body:
        case Request::InitiatorType::Input:
        case Request::InitiatorType::Link:
        case Request::InitiatorType::Other:
            return ResourceType::Other;
        }
        VERIFY_NOT_REACHED();
    }

    return ResourceType::Other;
}

URL::URL ContentBlocker::source_url_for_matching(URL::URL const& source_url)
{
    if (source_url.scheme() != "blob"sv)
        return source_url;

    auto parsed_url = URL::Parser::basic_parse(source_url.serialize_path());
    if (!parsed_url.has_value())
        return source_url;

    return parsed_url.release_value();
}

}
