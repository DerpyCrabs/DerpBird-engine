#pragma once

#include "DerpBirdCredentialVault.h"
#include "ChromeImportData.h"
#include <AK/JsonObject.h>
#include "DerpBirdTaskService.h"
#include <AK/OwnPtr.h>
#include <AK/Memory.h>
#include <AK/Variant.h>
#include <QDir>
#include <QFile>
#include <QLockFile>

namespace Ladybird {

class DerpBirdCredentialService final {
public:
    using Catalog = WebView::DerpBirdCredentialStore;
    enum class Operation { ImportCsv, ListAll, List, Save, Load, Reveal, Fill, Matches, ListExceptions, BlockOrigin, AllowOrigin, Remove, Cleanup };
    struct Request {
        Operation operation;
        String origin;
        String id;
        String username;
        Optional<DerpBirdSecret> secret;
        i64 revision { 0 };
        QString import_path {};
    };
    struct FillValue { Catalog::Entry entry; DerpBirdSecret secret; };
    struct MatchValue { bool matches; bool blocked { false }; };
    using Response = Variant<JsonObject,Vector<Catalog::Entry>, Catalog::Entry, Optional<DerpBirdSecret>, FillValue, MatchValue, Vector<String>, size_t>;
    using Worker = DerpBirdTaskService<Request, Response>;
    using Completion = Worker::Completion;
    static constexpr size_t max_bytes = 64 * 1024 * 1024;

    // Persistent normal-profile service. Renderer callers must not choose profile,
    // origin or secret IDs. The future host supplies canonical document authority
    // and private-mode save policy; no renderer-facing entry point is installed here.
    DerpBirdCredentialService(QString directory, String profile_id, int timeout_ms = 300000)
        : m_state(std::make_shared<State>(move(directory), move(profile_id)))
        , m_worker([state = m_state](Request const& request, GCancellable* cancel) { return state->execute(request, cancel); },
              admit, [state = m_state] { state->vault = nullptr; state->catalog = nullptr; state->lock.reset(); }, max_bytes, timeout_ms) { }
    ErrorOr<u64> submit(Request request, Completion completion) { return m_worker.submit(move(request), move(completion)); }
    bool cancel(u64 id) { return m_worker.cancel(id); }
    ErrorOr<void> close(Function<void()> completion) { return m_worker.close(move(completion)); }
    size_t pending() const { return m_worker.pending(); }
private:
    static ErrorOr<size_t> admit(Request const& request)
    {
        if (request.origin.bytes().size() > 2048 || request.id.bytes().size() > 64 || request.username.bytes().size() > 1024 || request.revision < 0)
            return Error::from_string_literal("Credential request exceeds limits");
        bool carries_secret = request.operation == Operation::Save || request.operation == Operation::Matches;
        if (carries_secret != request.secret.has_value()
            || (!carries_secret && !request.username.is_empty()))
            return Error::from_string_literal("Invalid credential request payload");
        auto bytes = request.secret.has_value() ? request.secret->bytes().size() : 0;
        if (bytes > DerpBirdSecretStore::max_secret_bytes) return Error::from_string_literal("Credential secret exceeds limit");
        switch (request.operation) {
        case Operation::ImportCsv:
            if (request.import_path.isEmpty()) return Error::from_string_literal("No CSV selected");
            return max_bytes;
        case Operation::ListExceptions:
        case Operation::ListAll:
            if (!request.origin.is_empty()) return Error::from_string_literal("Invalid credential listing");
            [[fallthrough]];
        case Operation::List:
            if (!request.id.is_empty() || request.revision) return Error::from_string_literal("Invalid credential listing");
            return Catalog::max_entries * (2048 + 1024 + 128 + 256);
        case Operation::BlockOrigin:
        case Operation::AllowOrigin:
            if (!request.id.is_empty() || request.revision) return Error::from_string_literal("Invalid credential save exception");
            return Catalog::max_entries * (2048 + 128);
        case Operation::Matches:
            if (!request.id.is_empty() || request.revision) return Error::from_string_literal("Invalid credential comparison");
            return bytes + DerpBirdSecretStore::max_secret_bytes + Catalog::max_entries * (2048 + 1024 + 128 + 256);
        case Operation::Save:
            if (request.id.is_empty() != (request.revision == 0)) return Error::from_string_literal("Invalid credential save revision");
            return bytes + 8192;
        case Operation::Load:
            if (request.id.is_empty() || request.revision) return Error::from_string_literal("Invalid credential lookup");
            return DerpBirdSecretStore::max_secret_bytes + 4096;
        case Operation::Fill:
        case Operation::Reveal:
            if (request.id.is_empty() || !request.revision) return Error::from_string_literal("Invalid credential reveal");
            return DerpBirdSecretStore::max_secret_bytes + 4096;
        case Operation::Remove:
            if (request.id.is_empty() || !request.revision) return Error::from_string_literal("Invalid credential removal");
            return 4096;
        case Operation::Cleanup:
            if (!request.origin.is_empty() || !request.id.is_empty() || request.revision) return Error::from_string_literal("Invalid credential cleanup");
            return 8192;
        }
        return Error::from_string_literal("Unknown credential operation");
    }
    struct State {
        State(QString directory, String profile) : directory(move(directory)), profile(move(profile)) { }
        QString directory;
        String profile;
        std::unique_ptr<QLockFile> lock;
        OwnPtr<Catalog> catalog;
        OwnPtr<DerpBirdCredentialVault> vault;
        ErrorOr<void> initialize()
        {
            if (vault) return {};
            if (profile.is_empty() || profile.bytes().size() > 64 || !QDir::isAbsolutePath(directory))
                return Error::from_string_literal("Invalid credential profile");
            for (auto c : profile.bytes())
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                    return Error::from_string_literal("Invalid credential profile identity");
            if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
                return Error::from_string_literal("Cannot create credential directory");
            if (!lock) {
                auto candidate = std::make_unique<QLockFile>(directory + "/Credentials.lock");
                candidate->setStaleLockTime(0);
                if (!candidate->tryLock(0))
                    return Error::from_string_literal("Credential profile is already open");
                lock = move(candidate);
            }
            QFile file(directory + "/Credentials.db");
            if (!file.exists() && !file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                return Error::from_string_literal("Cannot create credential database");
            if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
                return Error::from_string_literal("Cannot restrict credential database permissions");
            file.close();
            auto path = directory.toUtf8();
            Database::Database::Options options;
            options.synchronous = Database::Database::Synchronous::Full;
            options.foreign_keys = Database::Database::ForeignKeys::Yes;
            auto opened = TRY(Catalog::create(TRY(Database::Database::create(ByteString(path.constData()), "Credentials"sv, options))));
            TRY(opened->recover_pending());
            catalog = move(opened);
            vault = adopt_own(*new DerpBirdCredentialVault(*catalog, profile));
            return {};
        }
        Worker::Result execute(Request const& request, GCancellable* cancel)
        {
            TRY(initialize());
            if (g_cancellable_is_cancelled(cancel)) return Error::from_string_literal("Credential request cancelled");
            if (request.operation == Operation::ListExceptions) return Response { TRY(catalog->save_exceptions()) };
            if (request.operation == Operation::ListAll) return Response { TRY(catalog->all()) };
            if (request.operation == Operation::Cleanup) return Response { TRY(vault->collect_garbage(cancel)) };
            if (request.operation == Operation::ImportCsv) {
                QString error;
                auto bytes = ChromeImport::read(request.import_path, error);
                if (!error.isEmpty()) return Error::from_string_literal("Cannot read selected CSV (maximum 32 MiB)");
                auto parsed = ChromeImport::passwords(move(bytes));
                if (!parsed.error.isEmpty()) return Error::from_string_literal("Invalid Chrome CSV: check UTF-8, quoting and url/username/password columns");
                size_t imported = 0, skipped = parsed.skipped, failed = 0;
                auto existing = TRY(catalog->all());
                for (auto& item : parsed.items) {
                    if (g_cancellable_is_cancelled(cancel)) break;
                    auto url = URL::Parser::basic_parse(TRY(String::from_utf8(StringView(item.url.toUtf8().constData(), item.url.toUtf8().size()))));
                    if (!url.has_value()) { ++skipped; continue; }
                    auto origin = Catalog::origin_for(*url);
                    if (origin.is_error()) { ++skipped; continue; }
                    auto username = TRY(String::from_utf8(StringView(item.username.toUtf8().constData(), item.username.toUtf8().size())));
                    bool duplicate = false;
                    for (auto const& entry : existing)
                        if (entry.origin == origin.value() && entry.username == username) { duplicate = true; break; }
                    if (duplicate) { ++skipped; item.password.fill('\0'); continue; }
                    auto saved = vault->save({}, *url, move(username), ReadonlyBytes { reinterpret_cast<u8 const*>(item.password.constData()), static_cast<size_t>(item.password.size()) }, 0, cancel);
                    item.password.fill('\0');
                    if (saved.is_error()) { ++failed; continue; }
                    existing.append(saved.release_value()); ++imported;
                }
                JsonObject report;
                report.set("passwords"sv, imported); report.set("skipped"sv, skipped); report.set("failed"sv, failed);
                return Response { move(report) };
            }
            auto document = URL::Parser::basic_parse(request.origin);
            if (!document.has_value() || TRY(Catalog::origin_for(*document)) != request.origin)
                return Error::from_string_literal("Invalid credential request origin");
            switch (request.operation) {
            case Operation::List: return Response { TRY(catalog->for_origin(*document)) };
            case Operation::Save: return Response { TRY(vault->save(request.id, *document, request.username, request.secret->bytes(), request.revision, cancel)) };
            case Operation::Load: return Response { TRY(vault->load(request.id, *document, cancel)) };
            case Operation::BlockOrigin:
            case Operation::AllowOrigin:
                TRY(catalog->set_save_exception(*document, request.operation == Operation::BlockOrigin));
                return Response { TRY(catalog->save_exceptions()) };
            case Operation::Matches: {
                if (TRY(catalog->save_suggestions_blocked(*document))) return Response { MatchValue { false, true } };
                auto entries = TRY(catalog->for_origin(*document));
                for (auto const& entry : entries) {
                    if (g_cancellable_is_cancelled(cancel)) return Error::from_string_literal("Credential comparison cancelled");
                    if (entry.username != request.username) continue;
                    auto saved = TRY(vault->load(entry.id, *document, cancel));
                    if (!saved.has_value()) continue;
                    auto expected = request.secret->bytes();
                    auto actual = saved->bytes();
                    if (expected.size() == actual.size() && timing_safe_compare(expected.data(), actual.data(), actual.size()))
                        return Response { MatchValue { true } };
                }
                return Response { MatchValue { false } };
            }
            case Operation::Fill:
            case Operation::Reveal: {
                auto entry = TRY(catalog->get(request.id, *document));
                if (!entry.has_value() || entry->revision != request.revision)
                    return Error::from_string_literal("Credential changed; refresh before viewing");
                auto secret = TRY(vault->load(request.id, *document, cancel));
                if (request.operation == Operation::Fill) {
                    if (!secret.has_value()) return Error::from_string_literal("Saved credential is unavailable");
                    return Response { FillValue { entry.release_value(), secret.release_value() } };
                }
                return Response { move(secret) };
            }
            case Operation::Remove: TRY(vault->remove(request.id, *document, request.revision, cancel)); return Response { size_t { 0 } };
            case Operation::ListExceptions:
            case Operation::ListAll:
            case Operation::ImportCsv:
            case Operation::Cleanup: VERIFY_NOT_REACHED();
            }
            return Error::from_string_literal("Unknown credential operation");
        }
    };
    // Worker destroys all SQLite state before stopping; UI-owned state then has
    // only path/profile metadata left when the final shared reference is released.
    std::shared_ptr<State> m_state;
    Worker m_worker;
};

}
