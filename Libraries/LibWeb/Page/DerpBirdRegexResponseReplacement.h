#pragma once
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Utf16StringBuilder.h>
#include <LibRegex/ECMAScriptRegex.h>

namespace Web {
inline ErrorOr<Utf16String> replace_derpbird_regex_response(Utf16String input, JsonArray const& operations, u64* shared_budget = nullptr)
{
    constexpr size_t maximum_output = 16 * MiB;
    u64 local_budget = 4'000'000;
    auto& remaining = shared_budget ? *shared_budget : local_budget;
    for (auto const& raw : operations.values()) {
        if (!raw.is_object()) return Error::from_string_literal("Invalid response replacement");
        auto const& operation = raw.as_object();
        auto pattern = operation.get_string("pattern"sv);
        auto replacement = operation.get_string("replacement"sv);
        auto flags = operation.get_string("flags"sv);
        if (!pattern.has_value() || !replacement.has_value() || !flags.has_value() || pattern->bytes().size() > 4096 || replacement->bytes().size() > 8192)
            return Error::from_string_literal("Invalid response replacement");
        u8 seen_flags = 0;
        for (auto flag : flags->bytes()) {
            auto position = "gimsu"sv.find(static_cast<char>(flag));
            if (!position.has_value() || (seen_flags & (1u << *position))) return Error::from_string_literal("Invalid response regex flags");
            seen_flags |= 1u << *position;
        }
        // Limit nested/count repetition expansion before the compiler allocates bytecode.
        auto bytes = pattern->bytes();
        size_t expansion = max(bytes.size(), static_cast<size_t>(1));
        size_t depth = 0;
        bool character_class = false;
        for (size_t i = 0; i < bytes.size(); ++i) {
            auto ch = bytes[i];
            if (ch == '\\') { ++i; continue; }
            if (ch == '[') character_class = true;
            if (ch == ']') character_class = false;
            if (character_class) continue;
            if (ch == '(' && ++depth > 64) return Error::from_string_literal("Response regex nesting limit");
            if (ch == ')' && depth) --depth;
            if (ch != '{') continue;
            size_t count = 0, largest = 1;
            for (size_t j = i + 1; j < bytes.size(); ++j) {
                if (bytes[j] >= '0' && bytes[j] <= '9') { count = count * 10 + bytes[j] - '0'; if (count > 16384) return Error::from_string_literal("Response regex repetition limit"); largest = max(largest, count); }
                else if (bytes[j] == ',') count = 0;
                else break;
            }
            if (expansion > 16384 / largest) return Error::from_string_literal("Response regex compilation limit");
            expansion *= largest;
        }
        auto source = Utf16String::from_utf8(*pattern);
        auto compiled = regex::ECMAScriptRegex::compile(source.utf16_view(), {
            .ignore_case = flags->contains('i'), .multiline = flags->contains('m'), .dot_all = flags->contains('s'), .unicode = flags->contains('u') });
        if (compiled.is_error()) return Error::from_string_literal("Invalid response regex");
        auto regex = compiled.release_value();
        auto substitute = Utf16String::from_utf8(*replacement);
        auto text = input.utf16_view();
        auto format = substitute.utf16_view();
        Utf16StringBuilder output;
        size_t consumed = 0;
        for (size_t offset = 0; offset <= text.length_in_code_units(); ++offset) {
            if (remaining == 0) return Error::from_string_literal("Response regex execution limit");
            --remaining;
            auto result = regex.test_anchored_budgeted(text, offset, remaining);
            if (result == regex::MatchResult::LimitExceeded) return Error::from_string_literal("Response regex execution limit");
            if (result != regex::MatchResult::Match) continue;
            auto begin = regex.capture_slot(0), end = regex.capture_slot(1);
            if (begin < 0 || end < begin || static_cast<size_t>(end) > text.length_in_code_units()) return Error::from_string_literal("Invalid regex capture");
            output.append(text.substring_view(consumed, begin - consumed));
            auto append_capture = [&](unsigned index) {
                auto start = regex.capture_slot(index * 2), finish = regex.capture_slot(index * 2 + 1);
                if (start >= 0 && finish >= start && static_cast<size_t>(finish) <= text.length_in_code_units()) output.append(text.substring_view(start, finish - start));
            };
            for (size_t i = 0; i < format.length_in_code_units(); ++i) {
                auto ch = format.code_unit_at(i);
                if (ch != '$' || i + 1 == format.length_in_code_units()) { output.append_code_unit(ch); continue; }
                auto next = format.code_unit_at(i + 1);
                if (next == '$') { output.append_code_unit('$'); ++i; }
                else if (next == '&') { append_capture(0); ++i; }
                else if (next == '`') { output.append(text.substring_view(0, begin)); ++i; }
                else if (next == '\'') { output.append(text.substring_view(end)); ++i; }
                else if (next >= '0' && next <= '9') {
                    unsigned group = next - '0';
                    size_t digits = 1;
                    if (i + 2 < format.length_in_code_units()) {
                        auto second = format.code_unit_at(i + 2);
                        if (second >= '0' && second <= '9' && group * 10 + second - '0' <= regex.capture_count()) { group = group * 10 + second - '0'; digits = 2; }
                    }
                    if (group > 0 && group <= regex.capture_count()) { append_capture(group); i += digits; }
                    else output.append_code_unit('$');
                } else if (next == '<' && !regex.named_groups().is_empty()) {
                    size_t end_name = i + 2;
                    while (end_name < format.length_in_code_units() && format.code_unit_at(end_name) != '>') ++end_name;
                    if (end_name == format.length_in_code_units()) output.append_code_unit('$');
                    else {
                        auto name = format.substring_view(i + 2, end_name - i - 2);
                        for (auto const& group : regex.named_groups()) if (group.name == name) { append_capture(group.index); break; }
                        i = end_name;
                    }
                } else output.append_code_unit('$');
                if (output.length_in_code_units() > maximum_output) return Error::from_string_literal("Response replacement output limit");
            }
            if (output.length_in_code_units() > maximum_output) return Error::from_string_literal("Response replacement output limit");
            consumed = end;
            if (!flags->contains('g')) break;
            if (static_cast<size_t>(end) > offset) offset = end - 1;
            else if (flags->contains('u') && offset + 1 < text.length_in_code_units() && text.code_unit_at(offset) >= 0xd800 && text.code_unit_at(offset) <= 0xdbff
                && text.code_unit_at(offset + 1) >= 0xdc00 && text.code_unit_at(offset + 1) <= 0xdfff) ++offset;
        }
        output.append(text.substring_view(consumed));
        if (output.length_in_code_units() > maximum_output) return Error::from_string_literal("Response replacement output limit");
        input = output.to_string();
    }
    return input;
}

}
