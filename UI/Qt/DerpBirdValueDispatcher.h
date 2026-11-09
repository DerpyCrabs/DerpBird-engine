#pragma once

#include "DerpBirdValueAuthority.h"

namespace Ladybird {

// Adapter for Application's opaque IPC payload callback. Its caller supplies the
// authenticated connection/page/document and canonical URL, selects the correct
// profile/private authority, and rechecks document liveness before delivering.
class DerpBirdValueDispatcher {
public:
    using Protocol = Web::DerpBirdValueProtocol;
    using Completion = Function<void(ErrorOr<ByteBuffer>)>;
    using EventSink = Function<void(ByteBuffer)>;
    using ReserveEventSink = Function<ErrorOr<EventSink>(u64, size_t)>;
    static ErrorOr<void> dispatch(DerpBirdValueAuthority& authority, DerpBirdValueAuthority::Context context,
        URL::URL const& url, bool top_level, ReadonlyBytes payload, Completion completion, EventSink events = {}, ReserveEventSink reserve_events = {})
    {
        if (!completion) return Error::from_string_literal("Missing userscript storage completion");
        auto request = TRY(Protocol::decode_request(payload));
        if (request.method == Protocol::Method::Open) {
            auto token = TRY(authority.issue(context, request.identity, url, top_level));
            auto utf8 = token.toUtf8();
            Protocol::Response response { Protocol::Kind::Token, TRY(String::from_utf8(StringView(utf8.constData(), utf8.size()))), {}, {}, 0 };
            completion(Protocol::encode(response));
            return {};
        }
        auto identity = request.identity.bytes();
        auto token = QString::fromUtf8(reinterpret_cast<char const*>(identity.data()), identity.size());
        if (request.method == Protocol::Method::Subscribe) {
            if (!events && !reserve_events) return Error::from_string_literal("Userscript event transport is unavailable");
            DerpBirdValueAuthority::ReserveNotification reserve;
            if (reserve_events) {
                reserve = [identity = request.identity, reserve_events = move(reserve_events)](u64 id, size_t bytes) -> ErrorOr<DerpBirdValueAuthority::Notification> {
                    auto sink = TRY(reserve_events(id, bytes));
                    if (!sink) return Error::from_string_literal("Missing reserved userscript event sink");
                    return notification(identity, move(sink));
                };
            }
            auto id = TRY(authority.subscribe(context, token, move(request.key), request.asynchronous,
                events ? notification(request.identity, move(events)) : DerpBirdValueAuthority::Notification {}, move(reserve)));
            auto id_text = String::formatted("{}", id);
            if (id_text.is_error()) {
                authority.cancel_subscription(context, token, id);
                completion(id_text.release_error());
                return {};
            }
            auto reply = Protocol::encode(Protocol::Response { Protocol::Kind::Subscription, id_text.release_value(), {}, {}, 0 });
            if (reply.is_error()) authority.cancel_subscription(context, token, id);
            completion(move(reply));
            return {};
        }
        if (request.method == Protocol::Method::Unsubscribe) {
            auto id = Protocol::subscription_id(request.key);
            VERIFY(id.has_value());
            TRY(authority.unsubscribe(context, token, *id, request.asynchronous));
            completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
            return {};
        }
        Vector<DerpBirdValueService::Store::Update> updates;
        for (auto& entry : request.updates)
            TRY(updates.try_append({ move(entry.key), move(entry.value) }));
        return authority.submit(context, token, { request.method, request.asynchronous, move(request.key), move(updates) },
            [completion = move(completion)](ErrorOr<DerpBirdValueService::Response> result) mutable {
                if (result.is_error()) { completion(result.release_error()); return; }
                completion(encode_result(result.release_value()));
            });
    }

private:
    static DerpBirdValueAuthority::Notification notification(String identity, EventSink events)
    {
        return [identity = move(identity), events = move(events)](u64 id, i64 revision, auto const& change, bool remote) {
            // Validated fields; allocation failure must not silently lose an event.
            events(MUST(Protocol::encode_event(identity, id, revision, change.key, remote,
                change.old_value.has_value() ? Optional<ReadonlyBytes> { change.old_value->bytes() } : Optional<ReadonlyBytes> {},
                change.new_value.has_value() ? Optional<ReadonlyBytes> { change.new_value->bytes() } : Optional<ReadonlyBytes> {})));
        };
    }
    static ErrorOr<ByteBuffer> encode_result(DerpBirdValueService::Response result)
    {
        using Store = DerpBirdValueService::Store;
        static_assert(Store::max_value_bytes == Protocol::max_value);
        static_assert(Store::max_scope_bytes == Protocol::max_values);
        Protocol::Response response { Protocol::Kind::Revision, {}, {}, {}, 0 };
        if (result.has<Store::Snapshot>()) {
            auto& snapshot = result.get<Store::Snapshot>();
            response.kind = Protocol::Kind::Snapshot;
            response.revision = snapshot.revision;
            for (auto& entry : snapshot.values)
                TRY(response.entries.try_append({ move(entry.key), move(entry.value) }));
        } else if (result.has<Optional<ByteBuffer>>()) {
            response.kind = Protocol::Kind::Value;
            response.value = move(result.get<Optional<ByteBuffer>>());
        } else if (result.has<Vector<String>>()) {
            response.kind = Protocol::Kind::Keys;
            for (auto& key : result.get<Vector<String>>())
                TRY(response.entries.try_append({ move(key), {} }));
        } else {
            response.revision = result.get<i64>();
        }
        return Protocol::encode(response);
    }
};

}
