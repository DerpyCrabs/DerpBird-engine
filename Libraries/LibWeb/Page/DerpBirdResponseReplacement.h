#pragma once
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/Page/DerpBirdRegexResponseReplacement.h>
#include <AK/ScopeGuard.h>
#include <LibJS/Script.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Page/DerpBirdJSONResponseRuntime.h>

namespace Web {
inline ErrorOr<Utf16String> replace_derpbird_json_response(Utf16String, String const&, String const&, u64&);

inline ErrorOr<Utf16String> replace_derpbird_response(Utf16String input, JsonArray const& operations, u64* shared_budget = nullptr)
{
    u64 local_budget = 4'000'000;
    auto& remaining = shared_budget ? *shared_budget : local_budget;
    for (auto const& raw : operations.values()) {
        if (!raw.is_object()) return Error::from_string_literal("Invalid response replacement");
        auto const& operation = raw.as_object();
        if (auto kind = operation.get_string("json_kind"sv); kind.has_value()) {
            auto query = operation.get_string("query"sv);
            if (!query.has_value() || query->bytes().size() > 8192 || !kind->is_one_of("json"sv, "jsonl"sv))
                return Error::from_string_literal("Invalid JSON response replacement");
            input = TRY(replace_derpbird_json_response(move(input), *kind, *query, remaining));
        } else {
            JsonArray single;
            single.must_append(raw);
            input = TRY(replace_derpbird_regex_response(move(input), single, &remaining));
        }
    }
    return input;
}

// A fresh bare realm exposes no page objects, network or browser bridge. Only the
// pinned JSONPath implementation runs here; query and body are string properties.
inline ErrorOr<Utf16String> replace_derpbird_json_response(Utf16String input, String const& kind, String const& query, u64& budget)
{
    auto& vm = JS::VM::the();
    auto context_result = JS::Realm::initialize_host_defined_realm(vm, nullptr, nullptr);
    if (context_result.is_error()) return Error::from_string_literal("Unable to create JSON filtering realm");
    auto context = context_result.release_value();
    ScopeGuard pop_context = [&] { vm.pop_execution_context(); };
    auto& realm = *context->realm;
    auto& global = realm.global_object();
    global.define_direct_property("__input"_utf16_fly_string, JS::PrimitiveString::create(vm, move(input)), JS::default_attributes);
    global.define_direct_property("__kind"_utf16_fly_string, JS::PrimitiveString::create(vm, Utf16String::from_utf8(kind)), JS::default_attributes);
    global.define_direct_property("__query"_utf16_fly_string, JS::PrimitiveString::create(vm, Utf16String::from_utf8(query)), JS::default_attributes);
    auto native_regex = JS::NativeFunction::create(realm, [&budget](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        for (size_t i = 0; i < 4; ++i) {
            if (!vm.argument(i).is_string()) return vm.throw_completion<JS::TypeError>("Invalid JSON regex input"_utf16);
        }
        auto input = vm.argument(0).as_string().utf16_string();
        auto pattern = vm.argument(1).as_string().utf16_string().to_utf8();
        auto flags = vm.argument(2).as_string().utf16_string().to_utf8();
        auto substitute = vm.argument(3).as_string().utf16_string().to_utf8();
        auto test_only = vm.argument(4).is_boolean() && vm.argument(4).as_bool();
        JsonObject operation;
        operation.set("pattern"sv, move(pattern));
        operation.set("flags"sv, move(flags));
        // Adding a NUL before the match guarantees a longer result, even for an empty match.
        operation.set("replacement"sv, test_only ? MUST(String::from_utf8("\0$&"sv)) : move(substitute));
        JsonArray operations; operations.must_append(move(operation));
        auto input_length = input.length_in_code_units();
        auto result = replace_derpbird_response(move(input), operations, &budget);
        if (result.is_error()) return vm.throw_completion<JS::TypeError>("JSON regex limit or invalid pattern"_utf16);
        if (test_only) return JS::Value(result.value().length_in_code_units() > input_length);
        return JS::PrimitiveString::create(vm, result.release_value());
    }, 5, "__nativeRegex"_utf16_fly_string, GC::Ptr<JS::Realm> { &realm });
    global.define_direct_property("__nativeRegex"_utf16_fly_string, native_regex, JS::default_attributes);
    auto source = Utf16String::from_utf8(derpbird_json_response_runtime);
    auto script = JS::Script::parse(source.utf16_view(), realm);
    if (script.is_error()) return Error::from_string_literal("Invalid JSON filtering runtime");
    auto result = vm.run(script.value());
    if (result.is_error() || !result.value().is_string()) return Error::from_string_literal("JSON response filtering failed");
    auto output = result.value().as_string().utf16_string();
    if (output.length_in_code_units() > 16 * MiB) return Error::from_string_literal("JSON response output limit");
    return output;
}
}
