#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashTable.h>
#include <AK/NumericLimits.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace Web {

// Versioned binary payload inside the authenticated IPC envelope. No JSON/base64
// conversion of stored values, and no page URL or storage scope in an operation.
class DerpBirdValueProtocol {
public:
    static constexpr size_t max_message = 16 * 1024 * 1024;
    static constexpr size_t max_value = 1024 * 1024;
    static constexpr size_t max_values = 10 * 1024 * 1024;
    enum class Method : u8 { Get, Set, Delete, List, GetMany, SetMany, DeleteMany, Open, Subscribe, Unsubscribe, Network, NetworkLegacy, NetworkAbort, Clipboard, ClipboardLegacy, TabOpen, TabOpenLegacy, TabClose, TabWatch, MenuRegister, MenuRegisterLegacy, MenuRemove, MenuRemoveLegacy, MenuWait, TabGet, TabGetLegacy, TabSave, TabSaveLegacy, TabsGet, TabsGetLegacy, Download, DownloadLegacy, DownloadAbort, DownloadWatch, DownloadChunk, DownloadFinish, Notify, NotifyLegacy, NotifyWait, NotifyActivate, Cookie, CookieLegacy, NetworkRead, WindowClose, WindowFocus, Audio, AudioLegacy };
    enum class Kind : u8 { Token, Value, Keys, Snapshot, Revision, Subscription, Network, TabSnapshot };
    struct Entry { String key; Optional<ByteBuffer> value; };
    struct Request {
        Method method;
        bool asynchronous;
        String identity; // Installed script ID for Open; opaque capability otherwise.
        String key;
        Vector<Entry> updates;
    };
    struct Response {
        Kind kind;
        String token;
        Optional<ByteBuffer> value;
        Vector<Entry> entries;
        i64 revision { 0 };
    };

    struct Event {
        String identity;
        u64 subscription;
        i64 revision;
        String key;
        bool remote;
        Optional<ByteBuffer> old_value;
        Optional<ByteBuffer> new_value;
    };
    static Optional<u64> subscription_id(String const& text)
    {
        auto view = text.bytes_as_string_view();
        if (view.is_empty() || view[0] == '0') return {};
        for (auto ch : view) if (ch < '0' || ch > '9') return {};
        return view.to_number<u64>(TrimWhitespace::No);
    }
    static ErrorOr<ByteBuffer> encode_event(String const& identity, u64 subscription, i64 revision,
        String const& key, bool remote, Optional<ReadonlyBytes> old_value, Optional<ReadonlyBytes> new_value)
    {
        TRY(validate_event(identity, subscription, revision, key, old_value, new_value));
        Writer out;
        TRY(out.bytes("DVE1"sv.bytes()));
        TRY(out.string(identity));
        TRY(out.wide(subscription));
        TRY(out.wide(static_cast<u64>(revision)));
        TRY(out.string(key));
        TRY(out.byte(remote ? 1 : 0));
        for (auto const& value : { old_value, new_value }) {
            TRY(out.byte(value.has_value() ? 1 : 0));
            if (value.has_value()) TRY(out.blob(*value));
        }
        return move(out.data);
    }
    static ErrorOr<ByteBuffer> encode(Event const& event)
    {
        return encode_event(event.identity, event.subscription, event.revision, event.key, event.remote,
            event.old_value.has_value() ? Optional<ReadonlyBytes> { event.old_value->bytes() } : Optional<ReadonlyBytes> {},
            event.new_value.has_value() ? Optional<ReadonlyBytes> { event.new_value->bytes() } : Optional<ReadonlyBytes> {});
    }
    static ErrorOr<Event> decode_event(ReadonlyBytes bytes)
    {
        Reader in { bytes };
        TRY(in.magic("DVE1"sv));
        auto identity = TRY(in.string(128));
        auto subscription = TRY(in.wide());
        auto revision = TRY(in.wide());
        if (revision > static_cast<u64>(NumericLimits<i64>::max())) return invalid();
        auto key = TRY(in.string(1024));
        auto remote = TRY(in.byte());
        if (remote > 1) return invalid();
        Event event { move(identity), subscription, static_cast<i64>(revision), move(key), remote == 1,
            TRY(in.optional_value()), TRY(in.optional_value()) };
        if (in.offset != bytes.size()) return invalid();
        TRY(validate_event(event.identity, event.subscription, event.revision, event.key,
            event.old_value.has_value() ? Optional<ReadonlyBytes> { event.old_value->bytes() } : Optional<ReadonlyBytes> {},
            event.new_value.has_value() ? Optional<ReadonlyBytes> { event.new_value->bytes() } : Optional<ReadonlyBytes> {}));
        return event;
    }

    static ErrorOr<ByteBuffer> encode(Request const& request)
    {
        TRY(validate(request));
        Writer out;
        TRY(out.bytes("DVQ1"sv.bytes()));
        TRY(out.byte(static_cast<u8>(request.method)));
        TRY(out.byte(request.asynchronous ? 1 : 0));
        TRY(out.string(request.identity));
        TRY(out.string(request.key));
        TRY(out.entries(request.updates));
        return move(out.data);
    }
    static ErrorOr<Request> decode_request(ReadonlyBytes bytes)
    {
        Reader in { bytes };
        TRY(in.magic("DVQ1"sv));
        auto method = TRY(in.byte());
        auto asynchronous = TRY(in.byte());
        if (method > static_cast<u8>(Method::AudioLegacy) || asynchronous > 1)
            return invalid();
        Request request { static_cast<Method>(method), asynchronous == 1,
            TRY(in.string(128)), TRY(in.string(1024)), TRY(in.entries(256)) };
        if (in.offset != bytes.size()) return invalid();
        TRY(validate(request));
        return request;
    }
    static ErrorOr<ByteBuffer> encode(Response const& response)
    {
        TRY(validate(response));
        Writer out;
        TRY(out.bytes("DVR1"sv.bytes()));
        TRY(out.byte(static_cast<u8>(response.kind)));
        TRY(out.string(response.token));
        TRY(out.optional_value(response.value));
        TRY(out.entries(response.entries));
        auto revision = static_cast<u64>(response.revision);
        for (int i = 0; i < 8; ++i) TRY(out.byte(static_cast<u8>(revision >> (i * 8))));
        return move(out.data);
    }
    static ErrorOr<Response> decode_response(ReadonlyBytes bytes)
    {
        Reader in { bytes };
        TRY(in.magic("DVR1"sv));
        auto kind = TRY(in.byte());
        if (kind > static_cast<u8>(Kind::TabSnapshot)) return invalid();
        Response response { static_cast<Kind>(kind), TRY(in.string(128)), TRY(in.optional_value(kind == static_cast<u8>(Kind::Network) ? max_values : max_value)), TRY(in.entries(4096)), 0 };
        u64 revision = 0;
        for (int i = 0; i < 8; ++i) revision |= static_cast<u64>(TRY(in.byte())) << (i * 8);
        if (revision > static_cast<u64>(NumericLimits<i64>::max()) || in.offset != bytes.size()) return invalid();
        response.revision = static_cast<i64>(revision);
        TRY(validate(response));
        return response;
    }

private:
    static ErrorOr<void> validate_event(String const& identity, u64 subscription, i64 revision, String const& key,
        Optional<ReadonlyBytes> old_value, Optional<ReadonlyBytes> new_value)
    {
        if (identity.is_empty() || identity.bytes().size() > 128 || !subscription || revision <= 0 || key.bytes().size() > 1024
            || (!old_value.has_value() && !new_value.has_value())
            || (old_value.has_value() && old_value->size() > max_value) || (new_value.has_value() && new_value->size() > max_value)) return invalid();
        return {};
    }
    static Error invalid() { return Error::from_string_literal("Invalid userscript storage message"); }
    static ErrorOr<void> validate_entries(Vector<Entry> const& entries, size_t maximum)
    {
        if (entries.size() > maximum) return invalid();
        size_t total = 0;
        HashTable<String> keys;
        for (auto const& entry : entries) {
            if (entry.key.bytes().size() > 1024 || keys.contains(entry.key)) return invalid();
            keys.set(entry.key);
            total += entry.key.bytes().size();
            if (entry.value.has_value()) {
                if (entry.value->size() > max_value) return invalid();
                total += entry.value->size();
            }
            if (total > max_values) return invalid();
        }
        return {};
    }
    static ErrorOr<void> validate(Request const& request)
    {
        if (request.identity.is_empty() || request.identity.bytes().size() > 128 || request.key.bytes().size() > 1024)
            return invalid();
        TRY(validate_entries(request.updates, 256));
        switch (request.method) {
        case Method::TabGet: case Method::TabGetLegacy: case Method::TabsGet: case Method::TabsGetLegacy:
            if (!request.asynchronous || !request.key.is_empty() || !request.updates.is_empty()) return invalid();
            break;
        case Method::TabSave: case Method::TabSaveLegacy:
            if (!request.asynchronous || !request.key.is_empty() || request.updates.size()!=1
                || request.updates[0].key!="tab-data"sv || !request.updates[0].value.has_value()) return invalid();
            break;
        case Method::MenuRegister: case Method::MenuRegisterLegacy:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || request.updates.size()!=1
                || request.updates[0].key!="menu"sv || !request.updates[0].value.has_value() || request.updates[0].value->size()>4096) return invalid();
            break;
        case Method::MenuRemove: case Method::MenuRemoveLegacy: case Method::MenuWait:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || !request.updates.is_empty()) return invalid();
            break;
        case Method::Download: case Method::DownloadLegacy:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || request.updates.size()!=1
                || request.updates[0].key!="download"sv || !request.updates[0].value.has_value() || request.updates[0].value->size()>65536) return invalid();
            break;
        case Method::Audio: case Method::AudioLegacy:
            if (!request.asynchronous || !request.key.is_one_of("get"sv, "set"sv, "watch"sv, "cancel"sv)
                || request.updates.size() != 1 || request.updates[0].key != "audio"sv
                || !request.updates[0].value.has_value() || request.updates[0].value->size() > 1024) return invalid();
            break;
        case Method::Cookie: case Method::CookieLegacy:
            if (!request.asynchronous || (request.key != "list"sv && request.key != "set"sv && request.key != "delete"sv)
                || request.updates.size() != 1 || request.updates[0].key != "cookie"sv
                || !request.updates[0].value.has_value() || request.updates[0].value->size() > 16384) return invalid();
            break;
        case Method::Notify: case Method::NotifyLegacy:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || request.updates.size()!=1
                || request.updates[0].key!="notification"sv || !request.updates[0].value.has_value() || request.updates[0].value->size()>65536) return invalid();
            break;
        case Method::NotifyActivate:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || request.updates.size()!=1
                || request.updates[0].key!="allow"sv || !request.updates[0].value.has_value() || request.updates[0].value->size()!=1
                || request.updates[0].value->data()[0]>1) return invalid();
            break;
        case Method::NotifyWait:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || !request.updates.is_empty()) return invalid();
            break;
        case Method::DownloadChunk:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || request.updates.size()!=1
                || request.updates[0].key!="chunk"sv || !request.updates[0].value.has_value() || request.updates[0].value->size()>131072) return invalid();
            break;
        case Method::DownloadFinish: case Method::DownloadAbort: case Method::DownloadWatch:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || !request.updates.is_empty()) return invalid();
            break;
        case Method::TabOpen: case Method::TabOpenLegacy:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || request.updates.size()!=1
                || request.updates[0].key!="tab"sv || !request.updates[0].value.has_value() || request.updates[0].value->size()>65536) return invalid();
            break;
        case Method::WindowClose: case Method::WindowFocus:
            if (!request.asynchronous || !request.key.is_empty() || !request.updates.is_empty()) return invalid();
            break;
        case Method::TabClose: case Method::TabWatch:
            if (!request.asynchronous || !subscription_id(request.key).has_value() || !request.updates.is_empty()) return invalid();
            break;
        case Method::Clipboard: case Method::ClipboardLegacy:
            if (!request.asynchronous || (request.key != "text/plain"sv && request.key != "text/html"sv)
                || request.updates.size() != 1 || request.updates[0].key != "clipboard"sv || !request.updates[0].value.has_value()) return invalid();
            break;
        case Method::Network: case Method::NetworkLegacy:
            if (!subscription_id(request.key).has_value() || request.updates.size() != 1 || !request.updates[0].value.has_value()) return invalid();
            break;
        case Method::NetworkRead:
            if (!request.asynchronous) return invalid();
            [[fallthrough]];
        case Method::NetworkAbort:
            if (!subscription_id(request.key).has_value() || !request.updates.is_empty()) return invalid();
            break;
        case Method::Get: case Method::Subscribe:
            if (!request.updates.is_empty()) return invalid();
            break;
        case Method::Unsubscribe:
            if (!request.updates.is_empty() || !subscription_id(request.key).has_value()) return invalid();
            break;
        case Method::GetMany:
            if (!request.key.is_empty()) return invalid();
            for (auto const& entry : request.updates) if (entry.value.has_value()) return invalid();
            break;
        case Method::List: case Method::Open:
            if (!request.key.is_empty() || !request.updates.is_empty()) return invalid();
            break;
        case Method::Set: case Method::Delete: case Method::SetMany: case Method::DeleteMany: {
            if (!request.key.is_empty()) return invalid();
            if ((request.method == Method::Set || request.method == Method::Delete) && request.updates.size() != 1) return invalid();
            bool deleting = request.method == Method::Delete || request.method == Method::DeleteMany;
            for (auto const& update : request.updates) {
                if (update.value.has_value() == deleting) return invalid();
            }
            break;
        }
        default: return invalid();
        }
        return {};
    }
    static ErrorOr<void> validate(Response const& response)
    {
        if (response.revision < 0 || response.token.bytes().size() > 128
            || (response.value.has_value() && response.value->size() > (response.kind == Kind::Network ? max_values : max_value))) return invalid();
        TRY(validate_entries(response.entries, 4096));
        if (response.kind != Kind::Token && response.kind != Kind::Subscription && !response.token.is_empty()) return invalid();
        if (response.kind != Kind::Value && response.kind != Kind::Network && response.value.has_value()) return invalid();
        if (response.kind != Kind::Keys && response.kind != Kind::Snapshot && response.kind != Kind::TabSnapshot && !response.entries.is_empty()) return invalid();
        if (response.kind != Kind::Revision && response.kind != Kind::Snapshot && response.revision != 0) return invalid();
        switch (response.kind) {
        case Kind::Token: if (response.token.is_empty()) return invalid(); break;
        case Kind::Subscription: if (!subscription_id(response.token).has_value()) return invalid(); break;
        case Kind::Network: if (!response.value.has_value()) return invalid(); break;
        case Kind::Value: case Kind::Revision: case Kind::TabSnapshot: break;
        case Kind::Keys: case Kind::Snapshot:
            for (auto const& entry : response.entries) {
                if (entry.value.has_value() != (response.kind == Kind::Snapshot)) return invalid();
            }
            break;
        default: return invalid();
        }
        return {};
    }
    struct Writer {
        ByteBuffer data;
        ErrorOr<void> bytes(ReadonlyBytes bytes)
        {
            if (bytes.size() > max_message - data.size()) return invalid();
            return data.try_append(bytes);
        }
        ErrorOr<void> byte(u8 value) { return bytes({ &value, 1 }); }
        ErrorOr<void> wide(u64 value)
        {
            for (int i = 0; i < 8; ++i) TRY(byte(static_cast<u8>(value >> (i * 8))));
            return {};
        }
        ErrorOr<void> number(u32 value)
        {
            for (int i = 0; i < 4; ++i) TRY(byte(static_cast<u8>(value >> (i * 8))));
            return {};
        }
        ErrorOr<void> blob(ReadonlyBytes value) { TRY(number(value.size())); return bytes(value); }
        ErrorOr<void> string(String const& value) { return blob(value.bytes()); }
        ErrorOr<void> optional_value(Optional<ByteBuffer> const& value)
        {
            TRY(byte(value.has_value() ? 1 : 0));
            if (value.has_value()) TRY(blob(value->bytes()));
            return {};
        }
        ErrorOr<void> entries(Vector<Entry> const& entries)
        {
            TRY(number(entries.size()));
            for (auto const& entry : entries) { TRY(string(entry.key)); TRY(optional_value(entry.value)); }
            return {};
        }
    };
    struct Reader {
        ReadonlyBytes data;
        size_t offset { 0 };
        ErrorOr<ReadonlyBytes> bytes(size_t length)
        {
            if (data.size() > max_message || length > data.size() - offset) return invalid();
            auto result = data.slice(offset, length); offset += length; return result;
        }
        ErrorOr<void> magic(StringView expected)
        {
            if (StringView(TRY(bytes(4))) != expected) return invalid();
            return {};
        }
        ErrorOr<u8> byte() { return TRY(bytes(1))[0]; }
        ErrorOr<u64> wide()
        {
            u64 value = 0;
            for (int i = 0; i < 8; ++i) value |= static_cast<u64>(TRY(byte())) << (i * 8);
            return value;
        }
        ErrorOr<u32> number()
        {
            u32 value = 0;
            for (int i = 0; i < 4; ++i) value |= static_cast<u32>(TRY(byte())) << (i * 8);
            return value;
        }
        ErrorOr<ReadonlyBytes> blob(size_t maximum)
        {
            auto size = TRY(number());
            if (size > maximum) return invalid();
            return bytes(size);
        }
        ErrorOr<String> string(size_t maximum) { return String::from_utf8(TRY(blob(maximum))); }
        ErrorOr<Optional<ByteBuffer>> optional_value(size_t maximum = max_value)
        {
            auto present = TRY(byte());
            if (present > 1) return invalid();
            if (!present) return Optional<ByteBuffer> {};
            return Optional<ByteBuffer> { TRY(ByteBuffer::copy(TRY(blob(maximum)))) };
        }
        ErrorOr<Vector<Entry>> entries(size_t maximum)
        {
            auto count = TRY(number());
            if (count > maximum) return invalid();
            Vector<Entry> entries;
            size_t total = 0;
            for (u32 i = 0; i < count; ++i) {
                auto key = TRY(string(1024));
                auto value = TRY(optional_value());
                total += key.bytes().size() + (value.has_value() ? value->size() : 0);
                if (total > max_values) return invalid();
                TRY(entries.try_append({ move(key), move(value) }));
            }
            return entries;
        }
    };
};

}
