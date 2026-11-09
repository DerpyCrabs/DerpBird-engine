#pragma once

#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Page/DerpBirdValueKey.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web {

// Use Ladybird's stable LBSC storage envelope/version (not its IPC format).
// This retains undefined, UTF-16 code units, numeric edge cases and object graphs.
// The storage service treats these bytes as opaque and never evaluates them.
// ToString can execute caller code. A privileged caller must check its world and
// document again after this returns, before dispatching a storage operation.
inline WebIDL::ExceptionOr<String> encode_derpbird_value_key(JS::VM& vm, JS::Value key)
{
    auto string = TRY(key.to_utf16_string(vm));
    auto encoded = DerpBirdValueKey::encode(string.utf16_view());
    if (encoded.is_error() && encoded.error().is_errno() && encoded.error().code() == E2BIG)
        return WebIDL::QuotaExceededError::create("Userscript key exceeds its encoded storage quota"_utf16);
    return TRY_OR_THROW_OOM(vm, move(encoded));
}

inline constexpr size_t derpbird_max_value_bytes = 1024 * 1024;

inline WebIDL::ExceptionOr<ByteBuffer> encode_derpbird_value(JS::VM& vm, JS::Value value)
{
    auto record = TRY(HTML::structured_serialize_for_storage(vm, value));
    // This is a persisted-value quota, not a CPU or transient-allocation budget
    // for walking caller-supplied objects/getters in StructuredSerialize.
    if (record.data.size() > derpbird_max_value_bytes)
        return WebIDL::QuotaExceededError::create("Userscript value exceeds 1 MiB"_utf16);
    return move(record.data);
}

inline WebIDL::ExceptionOr<JS::Value> decode_derpbird_value(JS::VM& vm, ReadonlyBytes bytes, JS::Realm& target_realm)
{
    if (bytes.size() > derpbird_max_value_bytes)
        return WebIDL::QuotaExceededError::create("Stored userscript value exceeds 1 MiB"_utf16);
    auto copy = TRY_OR_THROW_OOM(vm, ByteBuffer::copy(bytes));
    HTML::StorageSerializationRecord record { move(copy) };
    // The storage reader validates the magic, format/version, tags and lengths.
    return HTML::structured_deserialize(vm, record, target_realm);
}

}
