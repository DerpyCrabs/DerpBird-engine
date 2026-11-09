#pragma once
#include "DerpBirdSecretStore.h"
#include "DerpBirdTaskService.h"

namespace Ladybird {
enum class DerpBirdSecretOperation { Get, Put, Remove };
struct DerpBirdSecretRequest {
        DerpBirdSecretOperation operation;
        String profile;
        String credential;
        Optional<DerpBirdSecret> secret;
    };

class DerpBirdSecretService final : public DerpBirdTaskService<DerpBirdSecretRequest, Optional<DerpBirdSecret>> {
public:
    using Operation = DerpBirdSecretOperation;
    using Request = DerpBirdSecretRequest;
    using Base = DerpBirdTaskService<Request, Optional<DerpBirdSecret>>;
    explicit DerpBirdSecretService(Backend backend = {}, int timeout_ms = 30000)
        : Base(backend ? move(backend) : Backend { execute }, admit, {}, max_bytes, timeout_ms) { }
private:
    static ErrorOr<size_t> admit(Request const& request)
    {
        if (request.profile.is_empty() || request.profile.bytes().size() > 64
            || request.credential.is_empty() || request.credential.bytes().size() > 64
            || (request.operation == Operation::Put) != request.secret.has_value())
            return Error::from_string_literal("Invalid credential service request");
        if (request.operation != Operation::Get && request.operation != Operation::Put && request.operation != Operation::Remove)
            return Error::from_string_literal("Unknown credential operation");
        auto payload_bytes = request.secret.has_value() ? request.secret->bytes().size() : 0;
        if (payload_bytes > DerpBirdSecretStore::max_secret_bytes)
            return Error::from_string_literal("Credential exceeds limit");
        return 256 + payload_bytes + (request.operation == Operation::Get ? DerpBirdSecretStore::max_secret_bytes : 0);
    }
    static Result execute(Request const& request, GCancellable* cancel)
    {
        switch (request.operation) {
        case Operation::Get: return DerpBirdSecretStore::get(request.profile, request.credential, cancel);
        case Operation::Put: TRY(DerpBirdSecretStore::put(request.profile, request.credential, request.secret->bytes(), cancel)); break;
        case Operation::Remove: TRY(DerpBirdSecretStore::remove(request.profile, request.credential, cancel)); break;
        }
        return Optional<DerpBirdSecret> {};
    }
};
}
