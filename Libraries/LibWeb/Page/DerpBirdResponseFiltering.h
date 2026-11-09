#pragma once
#include <AK/RefCounted.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>
#include <LibWeb/HTML/Parser/HTMLEncodingDetection.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/DerpBirdResponseReplacement.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web {
class DerpBirdResponseFiltering final : public RefCounted<DerpBirdResponseFiltering> {
public:
    static void wrap(JsonArray operations, GC::Root<ResourceLoader::OnHeadersReceived>& headers, GC::Root<ResourceLoader::OnDataReceived>& data,
        GC::Root<ResourceLoader::OnCachedBodyAvailable>& cached, GC::Root<ResourceLoader::OnComplete>& complete, Function<void(JsonArray const&)> applied = {}, Function<JsonArray(Fetch::Engine::NetworkTransportHandle*)> select_operations = {})
    {
        if (operations.is_empty() && !select_operations) return;
        auto state = adopt_ref(*new DerpBirdResponseFiltering(move(operations), headers, data, cached, complete, move(applied)));
        state->m_select_operations = move(select_operations);
        headers = GC::create_function(GC::Heap::the(), [state](Fetch::Engine::NetworkTransportHandle* transport, HTTP::HeaderList const& headers, Optional<u32> status, Optional<String> const& reason, Requests::CacheState cache) {
            if (state->m_finished) return;
            if (state->m_select_operations) state->m_operations = state->m_select_operations(transport);
            auto content_type = headers.get("Content-Type"sv);
            state->m_mime = content_type.has_value() ? MimeSniff::MimeType::parse(*content_type) : Optional<MimeSniff::MimeType> {};
            state->m_buffering = !state->m_operations.is_empty() && status == 200u && state->m_mime.has_value()
                && ((state->m_mime->type() == "text"sv && state->m_mime->subtype() != "event-stream"sv) || state->m_mime->is_javascript() || state->m_mime->is_json() || state->m_mime->essence().is_one_of("application/x-ndjson"sv, "application/jsonl"sv) || state->m_mime->is_xml());
            if (!state->m_buffering) { state->m_headers->function()(transport, headers, status, reason, cache); return; }
            state->m_transport = transport;
            state->m_saved_headers = HTTP::HeaderList::create(headers.headers());
            state->m_status = status;
            state->m_reason = reason;
            // Fetch cannot grant body credit before receiving headers; this bounded
            // transform owns delivery until it can release a complete local body.
            if (transport) transport->resume_body_delivery_up_to(maximum_body + 1);
        });
        data = GC::create_function(GC::Heap::the(), [state](Requests::ResponseData data) {
            if (state->m_finished) return;
            if (!state->m_buffering) { state->m_data->function()(move(data)); return; }
            if (state->m_failed) return;
            if (data.bytes().size() > maximum_body - state->m_body.size()) {
                state->fail("Response replacement body limit exceeded"sv);
                return;
            }
            if (state->m_body.try_append(data.bytes()).is_error())
                state->fail("Cannot buffer response replacement"sv);
        });
        cached = GC::create_function(GC::Heap::the(), [state](Core::ImmutableBytes data) {
            if (state->m_finished) return;
            // The following data callbacks carry slices of this cached payload.
            if (!state->m_buffering) state->m_cached->function()(move(data));
        });
        complete = GC::create_function(GC::Heap::the(), [state](bool success, Requests::RequestTimingInfo const& timing, Optional<StringView> error) {
            if (state->m_finished) return;
            state->m_finished = true;
            state->m_transport = nullptr;
            if (!state->m_buffering || !success) {
                state->m_body.clear();
                state->m_complete->function()(success, timing, error);
                return;
            }
            auto output = state->transform();
            state->m_body.clear();
            if (output.is_error()) { state->m_complete->function()(false, timing, "Response replacement failed"sv); return; }
            auto body = output.release_value();
            if (state->m_applied) state->m_applied(state->m_operations);
            auto& headers = *state->m_saved_headers;
            state->m_mime->set_parameter("charset"_string, "utf-8"_string);
            auto content_type = state->m_mime->serialized();
            auto length = ByteString::number(body.bytes().size());
            headers.set({ "Content-Type"sv, content_type.bytes_as_string_view() });
            headers.set({ "Content-Length"sv, length });
            headers.set({ "Cache-Control"sv, "no-store"sv });
            headers.delete_("Content-Encoding"sv);
            headers.delete_("ETag"sv);
            headers.delete_("Content-MD5"sv);
            state->m_headers->function()(nullptr, headers, state->m_status, state->m_reason, Requests::CacheState::NotCached);
            state->m_data->function()(Requests::ResponseData::from_bytes(body.bytes()));
            state->m_complete->function()(true, timing, {});
        });
    }
private:
    void fail(StringView reason)
    {
        if (m_finished) return;
        m_failed = true;
        m_finished = true;
        m_body.clear();
        // Completion may synchronously tear down Fetch or deliver cancellation.
        // Detach first and retain only the transport we still need to stop.
        auto transport = move(m_transport);
        m_complete->function()(false, {}, reason);
        if (transport) transport->stop();
    }
    static constexpr size_t maximum_body = 16 * MiB;
    DerpBirdResponseFiltering(JsonArray operations, GC::Root<ResourceLoader::OnHeadersReceived> headers, GC::Root<ResourceLoader::OnDataReceived> data,
        GC::Root<ResourceLoader::OnCachedBodyAvailable> cached, GC::Root<ResourceLoader::OnComplete> complete, Function<void(JsonArray const&)> applied)
        : m_operations(move(operations)), m_headers(move(headers)), m_data(move(data)), m_cached(move(cached)), m_complete(move(complete)), m_applied(move(applied)) { }
    ErrorOr<String> transform()
    {
        if (m_failed) return Error::from_string_literal("Response exceeds replacement limit");
        String encoding = m_mime->parameters().get("charset"sv).value_or("UTF-8"_string);
        if (m_mime->is_html() && !m_mime->parameters().contains("charset"sv)) {
            HTML::EncodingDeclarationPrescan prescan(true);
            if (prescan.scan(m_body.bytes(), true) == HTML::EncodingDeclarationPrescan::Result::Found) encoding = TRY(String::from_utf8(prescan.encoding()));
            else if (String::from_utf8(StringView { m_body.bytes() }).is_error()) encoding = "windows-1252"_string;
        }
        auto decoder = TextCodec::decoder_for(encoding);
        if (!decoder.has_value()) return Error::from_string_literal("Unknown response encoding");
        auto decoded = TRY(TextCodec::decode_to_utf16(m_body.bytes(), *decoder));
        auto transformed = TRY(replace_derpbird_response(move(decoded), m_operations));
        return transformed.to_utf8();
    }
    JsonArray m_operations;
    GC::Root<ResourceLoader::OnHeadersReceived> m_headers;
    GC::Root<ResourceLoader::OnDataReceived> m_data;
    GC::Root<ResourceLoader::OnCachedBodyAvailable> m_cached;
    GC::Root<ResourceLoader::OnComplete> m_complete;
    Function<void(JsonArray const&)> m_applied;
    Function<JsonArray(Fetch::Engine::NetworkTransportHandle*)> m_select_operations;
    RefPtr<Fetch::Engine::NetworkTransportHandle> m_transport;
    RefPtr<HTTP::HeaderList> m_saved_headers;
    Optional<MimeSniff::MimeType> m_mime;
    Optional<u32> m_status;
    Optional<String> m_reason;
    ByteBuffer m_body;
    bool m_buffering { false }, m_failed { false }, m_finished { false };
};
}
