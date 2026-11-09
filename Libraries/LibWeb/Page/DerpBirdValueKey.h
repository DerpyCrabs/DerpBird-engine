#pragma once

#include <AK/Base64.h>
#include <errno.h>
#include <AK/Utf16StringBuilder.h>

namespace Web {

// SQLite/protocol keys are strict UTF-8, while JS strings may contain unpaired
// UTF-16 surrogates. Prefix both representations so literal strings cannot alias
// encoded ones. Never normalize Unicode or replace invalid UTF-16 code units.
class DerpBirdValueKey {
public:
    static constexpr size_t max_bytes = 1024;
    static ErrorOr<String> encode(Utf16View key)
    {
        // Every supported representation needs at least one byte per code unit.
        // Reject before converting or allocating based on an arbitrarily large key.
        if (key.length_in_code_units() > max_bytes - 3)
            return Error::from_errno(E2BIG);
        String result;
        if (key.validate()) {
            result = TRY(String::formatted("u8:{}", TRY(key.to_utf8(AllowLonelySurrogates::No))));
        } else {
            // The fallback is explicit UTF-16LE, independent of host endianness.
            // Four prefix bytes plus base64 output must fit the stored-key limit.
            auto byte_count = key.length_in_code_units() * 2;
            if (4 + ((byte_count + 2) / 3) * 4 > max_bytes)
                return Error::from_errno(E2BIG);
            auto bytes = TRY(ByteBuffer::create_uninitialized(byte_count));
            for (size_t i = 0; i < key.length_in_code_units(); ++i) {
                auto unit = key.code_unit_at(i);
                bytes[2 * i] = static_cast<u8>(unit);
                bytes[2 * i + 1] = static_cast<u8>(unit >> 8);
            }
            result = TRY(String::formatted("u16:{}", TRY(encode_base64(bytes.bytes()))));
        }
        if (result.bytes().size() > max_bytes)
            return Error::from_errno(E2BIG);
        return result;
    }
    static ErrorOr<Utf16String> decode(String const& stored)
    {
        auto bytes = stored.bytes_as_string_view();
        if (bytes.length() > max_bytes)
            return Error::from_string_literal("Stored userscript key exceeds quota");
        Utf16String result;
        if (bytes.starts_with("u8:"sv)) {
            auto string = TRY(String::from_utf8(bytes.substring_view(3)));
            result = Utf16String::from_utf8(string);
        } else if (bytes.starts_with("u16:"sv)) {
            auto decoded = TRY(decode_base64(bytes.substring_view(4), AK::LastChunkHandling::Strict));
            if (decoded.size() % 2)
                return Error::from_string_literal("Invalid UTF-16 userscript key");
            Utf16StringBuilder builder;
            for (size_t i = 0; i < decoded.size(); i += 2)
                builder.append_code_unit(static_cast<char16_t>(decoded[i] | (static_cast<u16>(decoded[i + 1]) << 8)));
            result = builder.to_string();
        } else {
            return Error::from_string_literal("Unknown userscript key encoding");
        }
        // Reject whitespace, noncanonical padding and a UTF-16 fallback for valid
        // Unicode: there is exactly one persisted representation per JS string.
        if (TRY(encode(result.utf16_view())) != stored)
            return Error::from_string_literal("Noncanonical userscript key encoding");
        return result;
    }
};

}
