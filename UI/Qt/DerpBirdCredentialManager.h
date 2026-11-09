#pragma once

#include "DerpBirdCredentialService.h"
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>

namespace Ladybird {

// Called only by the dedicated trusted shell, never by a content renderer.
// Form capture/fill needs its own document-bound authority and cannot use this
// management API. In particular, a caller cannot select a keyring profile or
// supply the opaque secret IDs held in the native catalog.
class DerpBirdCredentialManager {
public:
    AK_ALLOC_WITH_KMALLOC;
    using Completion = Function<void(ErrorOr<JsonValue>)>;
    DerpBirdCredentialManager(QString directory, String profile)
        : m_service(move(directory), move(profile)) { }

    ErrorOr<void> dispatch(JsonValue const& payload, bool is_private, Completion& completion)
    {
        if (is_private) return Error::from_string_literal("Manage passwords from a regular browser window");
        if (!payload.is_object()) return Error::from_string_literal("Invalid credential request");
        auto const& object = payload.as_object();
        auto type = object.get_string("type"sv);
        using Service = DerpBirdCredentialService;
        Service::Request request { Service::Operation::ListAll, {}, {}, {}, {}, 0 };
        if (type == "credentials.for-origin"sv) request.operation = Service::Operation::List;
        if (type == "credentials.exceptions"sv) request.operation = Service::Operation::ListExceptions;
        if (type == "credentials.import-csv"sv) {
            auto path = object.get_string("path"sv);
            if (!path.has_value()) return Error::from_string_literal("Missing CSV path");
            request.operation = Service::Operation::ImportCsv;
            request.import_path = QString::fromUtf8(path->bytes_as_string_view().characters_without_null_termination(), path->bytes().size());
        }
        if (type != "credentials.list"sv && type != "credentials.exceptions"sv && type != "credentials.import-csv"sv) {
            auto origin = object.get_string("origin"sv);
            auto id = object.get_string("id"sv);
            auto revision = object.get_string("revision"sv);
            if (!origin.has_value() || !id.has_value() || !revision.has_value() || revision->bytes().size() > 19)
                return Error::from_string_literal("Missing credential identity");
            auto parsed_revision = revision->to_number<i64>();
            if (!parsed_revision.has_value() || *parsed_revision < 0 || String::number(*parsed_revision) != *revision)
                return Error::from_string_literal("Invalid credential revision");
            request.origin = *origin;
            request.id = *id;
            request.revision = *parsed_revision;
            if (type == "credentials.save"sv || type == "credentials.matches"sv) {
                auto username = object.get_string("username"sv);
                auto password = object.get_string("password"sv);
                if (!username.has_value() || !password.has_value() || password->bytes().size() > DerpBirdSecretStore::max_secret_bytes)
                    return Error::from_string_literal("Invalid credential fields");
                request.operation = type == "credentials.matches"sv ? Service::Operation::Matches : Service::Operation::Save;
                request.username = *username;
                request.secret = DerpBirdSecret { secret_value_new(password->is_empty() ? "" : password->bytes_as_string_view().characters_without_null_termination(), password->bytes().size(), "application/octet-stream") };
            } else if (type == "credentials.for-origin"sv) {
                request.operation = Service::Operation::List;
            } else if (type == "credentials.block-origin"sv || type == "credentials.allow-origin"sv) {
                request.operation = type == "credentials.block-origin"sv ? Service::Operation::BlockOrigin : Service::Operation::AllowOrigin;
            } else if (type == "credentials.for-fill"sv) {
                request.operation = Service::Operation::Fill;
            } else if (type == "credentials.reveal"sv) {
                request.operation = Service::Operation::Reveal;
            } else if (type == "credentials.remove"sv) {
                request.operation = Service::Operation::Remove;
            } else {
                return Error::from_string_literal("Unknown credential management operation");
            }
        }
        // Preserve completion on admission failure so the caller can return the
        // immediate error. Async failures go through the same exactly-once path.
        bool cleanup = request.operation != Service::Operation::Matches && request.operation != Service::Operation::ListExceptions
            && request.operation != Service::Operation::BlockOrigin && request.operation != Service::Operation::AllowOrigin;
        auto done = std::make_shared<Completion>(move(completion));
        auto submitted = m_service.submit(move(request), [this, done, cleanup](auto result) {
            if (result.is_error()) { (*done)(result.release_error()); return; }
            if (cleanup) schedule_cleanup();
            auto response = result.release_value();
            if (auto* report = response.template get_pointer<JsonObject>()) {
                (*done)(JsonValue { move(*report) });
            } else if (auto* entries = response.template get_pointer<Vector<Service::Catalog::Entry>>()) {
                JsonArray items;
                for (auto const& entry : *entries) items.must_append(metadata(entry));
                JsonObject value;
                value.set("items"sv, move(items));
                (*done)(JsonValue { move(value) });
            } else if (auto* entry = response.template get_pointer<Service::Catalog::Entry>()) {
                (*done)(metadata(*entry));
            } else if (auto* origins = response.template get_pointer<Vector<String>>()) {
                JsonArray list;
                for (auto const& origin : *origins) list.must_append(origin);
                JsonObject value; value.set("origins"sv, move(list));
                (*done)(JsonValue { move(value) });
            } else if (auto* match = response.template get_pointer<Service::MatchValue>()) {
                JsonObject value;
                value.set("matches"sv, match->matches);
                value.set("blocked"sv, match->blocked);
                (*done)(JsonValue { move(value) });
            } else if (auto* fill = response.template get_pointer<Service::FillValue>()) {
                auto text = String::from_utf8(fill->secret.bytes());
                if (text.is_error()) { (*done)(text.release_error()); return; }
                auto value = metadata(fill->entry);
                value.as_object().set("password"sv, text.release_value());
                (*done)(move(value));
            } else if (auto* secret = response.template get_pointer<Optional<DerpBirdSecret>>()) {
                if (!secret->has_value()) { (*done)(Error::from_string_literal("Saved credential is unavailable")); return; }
                auto text = String::from_utf8((*secret)->bytes());
                if (text.is_error()) { (*done)(Error::from_string_literal("Saved credential is not UTF-8 text")); return; }
                JsonObject value;
                value.set("password"sv, text.release_value());
                (*done)(JsonValue { move(value) });
            } else {
                (*done)(JsonValue {});
            }
        });
        if (submitted.is_error()) {
            completion = move(*done);
            return submitted.release_error();
        }
        return {};
    }
    ErrorOr<void> close(Function<void()> completion) { return m_service.close(move(completion)); }

private:
    void schedule_cleanup()
    {
        if (m_cleanup_pending) return;
        m_cleanup_pending = true;
        auto queued = m_service.submit({ DerpBirdCredentialService::Operation::Cleanup, {}, {}, {}, {}, 0 },
            [this](auto) { m_cleanup_pending = false; });
        // The journal survives admission/provider failure. A later management
        // operation retries a bounded batch; UI metadata replies do not wait.
        if (queued.is_error()) m_cleanup_pending = false;
    }
    static JsonValue metadata(DerpBirdCredentialService::Catalog::Entry const& entry)
    {
        JsonObject result;
        result.set("id"sv, entry.id);
        result.set("origin"sv, entry.origin);
        result.set("username"sv, entry.username);
        // SQLite revisions are i64; JSON numbers would lose CAS precision.
        result.set("revision"sv, String::number(entry.revision));
        return result;
    }
    DerpBirdCredentialService m_service;
    bool m_cleanup_pending { false };
};

}
