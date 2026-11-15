#pragma once
#include <AK/JsonObject.h>
#include <UI/Qt/HubConnection.h>
namespace Ladybird {
class DerpBirdProxy {
public:
    static ErrorOr<JsonObject> state()
    {
        if (s_state.has_value()) return *s_state;
        auto result = TRY(HubConnection::the().request_derpbird_sync(0, "proxy.state"_string));
        if (!result.is_object()) return Error::from_string_literal("Invalid proxy state");
        s_state = result.as_object();
        return *s_state;
    }
    static ErrorOr<JsonValue> replace(JsonObject const& request)
    {
        auto result = TRY(HubConnection::the().request_derpbird_sync(0, "proxy.replace"_string, request));
        if (result.is_object()) s_state = result.as_object();
        return result;
    }
private:
    static inline Optional<JsonObject> s_state;
};
}
