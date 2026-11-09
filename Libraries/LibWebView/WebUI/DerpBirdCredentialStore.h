#pragma once

#include <AK/Array.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NumericLimits.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/ResultRow.h>
#include <LibURL/Parser.h>
#include <LibURL/Origin.h>

namespace WebView {

// Profile-scoped metadata only. Own on a worker. Secret preparation precedes any
// keyring write; publication and retirement of its predecessor are one SQLite
// transaction. On startup, recover_pending() retires interrupted preparations.
class DerpBirdCredentialStore {
public:
    AK_ALLOC_WITH_KMALLOC;
    struct Entry { String id; String origin; String username; String secret_id; i64 revision { 0 }; };
    static constexpr size_t max_entries = 4096;
    static constexpr size_t max_secrets = 8192;
    static ErrorOr<NonnullOwnPtr<DerpBirdCredentialStore>> create(NonnullRefPtr<Database::Database> db)
    {
        TRY(db->set_busy_timeout(250));
        // The preparation journal must be durable before a separate keyring write.
        TRY(db->execute_raw("PRAGMA synchronous=FULL;"));
        TRY(db->execute_raw("PRAGMA foreign_keys=ON;"));
        Array migrations { Database::Migration { .version = 1, .sql = R"SQL(
            CREATE TABLE CredentialState(id INTEGER PRIMARY KEY CHECK(id=1), revision INTEGER NOT NULL CHECK(revision>=0));
            INSERT INTO CredentialState VALUES(1,0);
            CREATE TABLE CredentialSecrets(id TEXT PRIMARY KEY NOT NULL, state INTEGER NOT NULL CHECK(state IN(0,1,2)));
            CREATE TABLE Credentials(
                id TEXT PRIMARY KEY NOT NULL, origin TEXT NOT NULL, username TEXT NOT NULL,
                secret_id TEXT NOT NULL UNIQUE REFERENCES CredentialSecrets(id),
                revision INTEGER NOT NULL CHECK(revision>0), UNIQUE(origin,username)
            );
            CREATE INDEX CredentialsOrigin ON Credentials(origin,id);
        )SQL" }, Database::Migration { .version = 2, .sql = R"SQL(
            CREATE TABLE CredentialSaveExceptions(origin TEXT PRIMARY KEY NOT NULL);
        )SQL" } };
        if (TRY(db->migrate("DerpBirdCredentials"sv, migrations)) != Database::MigrationOutcome::Success)
            return Error::from_string_literal("Credential database is newer than browser");
        auto store = adopt_own(*new DerpBirdCredentialStore(move(db)));
        auto& database = *store->m_db;
        store->m_get = TRY(database.prepare_statement("SELECT id,origin,username,secret_id,revision FROM Credentials WHERE id=:id"sv));
        store->m_list = TRY(database.prepare_statement("SELECT id,origin,username,secret_id,revision FROM Credentials WHERE origin=:origin ORDER BY id"sv));
        store->m_all = TRY(database.prepare_statement("SELECT id,origin,username,secret_id,revision FROM Credentials ORDER BY origin,username,id"sv));
        store->m_revision = TRY(database.prepare_statement("SELECT revision FROM CredentialState WHERE id=1"sv));
        store->m_bump = TRY(database.prepare_statement("UPDATE CredentialState SET revision=:revision WHERE id=1"sv));
        store->m_secret = TRY(database.prepare_statement("SELECT state FROM CredentialSecrets WHERE id=:id"sv));
        store->m_prepare = TRY(database.prepare_statement("INSERT INTO CredentialSecrets(id,state) VALUES(:id,0)"sv));
        store->m_count = TRY(database.prepare_statement("SELECT (SELECT COUNT(*) FROM Credentials) AS entries,(SELECT COUNT(*) FROM CredentialSecrets) AS secrets"sv));
        store->m_state = TRY(database.prepare_statement("UPDATE CredentialSecrets SET state=:state WHERE id=:id"sv));
        store->m_put = TRY(database.prepare_statement("INSERT INTO Credentials(id,origin,username,secret_id,revision) VALUES(:id,:origin,:username,:secret,:revision) ON CONFLICT(id) DO UPDATE SET username=excluded.username,secret_id=excluded.secret_id,revision=excluded.revision"sv));
        store->m_delete = TRY(database.prepare_statement("DELETE FROM Credentials WHERE id=:id"sv));
        store->m_garbage = TRY(database.prepare_statement("SELECT id FROM CredentialSecrets WHERE state=2 ORDER BY id LIMIT 128"sv));
        store->m_forget = TRY(database.prepare_statement("DELETE FROM CredentialSecrets WHERE id=:id AND state=2 AND NOT EXISTS(SELECT 1 FROM Credentials WHERE secret_id=:id)"sv));
        store->m_blocked = TRY(database.prepare_statement("SELECT origin FROM CredentialSaveExceptions WHERE origin=:origin"sv));
        store->m_blocked_all = TRY(database.prepare_statement("SELECT origin FROM CredentialSaveExceptions ORDER BY origin"sv));
        store->m_blocked_count = TRY(database.prepare_statement("SELECT COUNT(*) AS count FROM CredentialSaveExceptions"sv));
        store->m_block = TRY(database.prepare_statement("INSERT INTO CredentialSaveExceptions(origin) VALUES(:origin) ON CONFLICT(origin) DO NOTHING"sv));
        store->m_allow = TRY(database.prepare_statement("DELETE FROM CredentialSaveExceptions WHERE origin=:origin"sv));
        return store;
    }
    static ErrorOr<String> origin_for(URL::URL const& url)
    {
        if (!url.scheme().is_one_of("https"sv, "http"sv) || !url.username().is_empty() || !url.password().is_empty())
            return Error::from_string_literal("Unsupported credential origin");
        auto origin = url.origin();
        if (origin.is_opaque()) return Error::from_string_literal("Opaque credential origin");
        auto serialized = origin.serialize();
        if (serialized.bytes().size() > 2048) return Error::from_string_literal("Credential origin exceeds limit");
        return serialized;
    }
    ErrorOr<bool> save_suggestions_blocked(URL::URL const& url)
    {
        auto origin = TRY(origin_for(url));
        bool found = false;
        TRY(m_db->try_execute_bound_statement(m_blocked, [&](auto& bind) { return bind("origin"sv, origin); },
            [&](Database::ResultRow&) -> ErrorOr<void> { found = true; return {}; }));
        return found;
    }
    ErrorOr<Vector<String>> save_exceptions()
    {
        return m_db->try_collect_bound_statement<String>(m_blocked_all, max_entries,
            [](auto&) -> ErrorOr<void> { return {}; }, [](Database::ResultRow& row) { return row.read_text("origin"sv, 2048); });
    }
    ErrorOr<void> set_save_exception(URL::URL const& url, bool blocked)
    {
        auto origin = TRY(origin_for(url));
        return m_db->transaction([&]() -> ErrorOr<void> {
            if (blocked && !TRY(save_suggestions_blocked(url))) {
                i64 count = 0;
                TRY(m_db->try_execute_bound_statement(m_blocked_count, [](auto&) -> ErrorOr<void> { return {}; },
                    [&](Database::ResultRow& row) -> ErrorOr<void> { count = TRY(row.read_integer<i64>("count"sv)); return {}; }));
                if (count >= static_cast<i64>(max_entries)) return Error::from_string_literal("Too many credential save exceptions");
            }
            return m_db->try_execute_bound_statement(blocked ? m_block : m_allow, [&](auto& bind) { return bind("origin"sv, origin); });
        });
    }
    ErrorOr<void> prepare_secret(String const& id)
    {
        TRY(validate_id(id));
        return m_db->transaction([&]() -> ErrorOr<void> {
            TRY(check_limits(false, true));
            return m_db->try_execute_bound_statement(m_prepare, [&](auto& bind) { return bind("id"sv, id); });
        });
    }
    ErrorOr<void> abandon_secret(String const& id)
    {
        TRY(validate_id(id));
        return m_db->transaction([&]() -> ErrorOr<void> {
            if (TRY(secret_state(id)) != Optional<i64> { 0 }) return Error::from_string_literal("Secret is not pending");
            return set_state(id, 2);
        });
    }
    ErrorOr<void> recover_pending()
    {
        return m_db->execute_raw("UPDATE CredentialSecrets SET state=2 WHERE state=0;");
    }
    ErrorOr<Vector<String>> garbage()
    {
        return m_db->try_collect_bound_statement<String>(m_garbage, 128, [](auto&) -> ErrorOr<void> { return {}; },
            [](Database::ResultRow& row) { return row.read_text("id"sv, 64); });
    }
    // Call only after the corresponding keyring item has been removed.
    ErrorOr<void> forget_garbage(String const& id)
    {
        TRY(validate_id(id));
        return m_db->try_execute_bound_statement(m_forget, [&](auto& bind) { return bind("id"sv, id); });
    }
    ErrorOr<Vector<Entry>> all()
    {
        return m_db->try_collect_bound_statement<Entry>(m_all, max_entries,
            [](auto&) -> ErrorOr<void> { return {}; }, read_entry);
    }
    ErrorOr<Vector<Entry>> for_origin(URL::URL const& url)
    {
        auto origin = TRY(origin_for(url));
        return m_db->try_collect_bound_statement<Entry>(m_list, max_entries,
            [&](auto& bind) { return bind("origin"sv, origin); }, read_entry);
    }
    ErrorOr<Optional<Entry>> get(String const& id, URL::URL const& url)
    {
        TRY(validate_id(id));
        auto origin = TRY(origin_for(url));
        auto entry = TRY(read(id));
        if (entry.has_value() && entry->origin != origin) return Optional<Entry> {};
        return entry;
    }
    ErrorOr<i64> put(Entry const& entry, i64 expected_revision)
    {
        TRY(validate_id(entry.id)); TRY(validate_id(entry.secret_id));
        if (entry.origin.bytes().size() > 2048 || entry.username.bytes().size() > 1024)
            return Error::from_string_literal("Credential metadata exceeds limit");
        auto url = URL::Parser::basic_parse(entry.origin);
        if (!url.has_value() || TRY(origin_for(*url)) != entry.origin || entry.username.bytes().size() > 1024 || expected_revision < 0)
            return Error::from_string_literal("Invalid credential metadata");
        i64 revision = 0;
        TRY(m_db->transaction([&]() -> ErrorOr<void> {
            auto old = TRY(read(entry.id));
            if (old.has_value() ? old->revision != expected_revision || old->origin != entry.origin : expected_revision != 0)
                return Error::from_string_literal("Credential changed; refresh before replacing");
            TRY(check_limits(!old.has_value(), false));
            auto same_secret = old.has_value() && old->secret_id == entry.secret_id;
            if (TRY(secret_state(entry.secret_id)) != Optional<i64> { same_secret ? 1 : 0 })
                return Error::from_string_literal("Credential secret was not prepared");
            revision = TRY(next_revision());
            TRY(m_db->try_execute_bound_statement(m_put, [&](auto& bind) -> ErrorOr<void> {
                TRY(bind("id"sv, entry.id)); TRY(bind("origin"sv, entry.origin)); TRY(bind("username"sv, entry.username));
                TRY(bind("secret"sv, entry.secret_id)); return bind("revision"sv, revision);
            }));
            TRY(set_state(entry.secret_id, 1));
            if (old.has_value() && !same_secret) TRY(set_state(old->secret_id, 2));
            return {};
        }));
        return revision;
    }
    ErrorOr<void> remove(String const& id, URL::URL const& url, i64 expected_revision)
    {
        TRY(validate_id(id)); auto origin = TRY(origin_for(url));
        return m_db->transaction([&]() -> ErrorOr<void> {
            auto old = TRY(read(id));
            if (!old.has_value() || old->origin != origin || old->revision != expected_revision)
                return Error::from_string_literal("Credential changed or origin mismatch");
            TRY(next_revision());
            TRY(m_db->try_execute_bound_statement(m_delete, [&](auto& bind) { return bind("id"sv, id); }));
            return set_state(old->secret_id, 2);
        });
    }
private:
    explicit DerpBirdCredentialStore(NonnullRefPtr<Database::Database> db) : m_db(move(db)) { }
    static ErrorOr<void> validate_id(String const& id)
    {
        if (id.is_empty() || id.bytes().size() > 64) return Error::from_string_literal("Invalid credential identifier");
        for (auto c : id.bytes())
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                return Error::from_string_literal("Invalid credential identifier");
        return {};
    }
    static ErrorOr<Entry> read_entry(Database::ResultRow& row)
    {
        return Entry { TRY(row.read_text("id"sv,64)), TRY(row.read_text("origin"sv,2048)),
            TRY(row.read_text("username"sv,1024)), TRY(row.read_text("secret_id"sv,64)), TRY(row.read_integer<i64>("revision"sv)) };
    }
    ErrorOr<Optional<Entry>> read(String const& id)
    {
        Optional<Entry> entry;
        TRY(m_db->try_execute_bound_statement(m_get, 1, [&](auto& bind) { return bind("id"sv,id); },
            [&](Database::ResultRow& row) -> ErrorOr<void> { entry = TRY(read_entry(row)); return {}; }));
        return entry;
    }
    ErrorOr<Optional<i64>> secret_state(String const& id)
    {
        Optional<i64> result;
        TRY(m_db->try_execute_bound_statement(m_secret, 1, [&](auto& bind) { return bind("id"sv,id); },
            [&](Database::ResultRow& row) -> ErrorOr<void> { result = TRY(row.read_integer<i64>("state"sv)); return {}; }));
        return result;
    }
    ErrorOr<void> set_state(String const& id, i64 state)
    {
        return m_db->try_execute_bound_statement(m_state, [&](auto& bind) -> ErrorOr<void> { TRY(bind("id"sv,id)); return bind("state"sv,state); });
    }
    ErrorOr<void> check_limits(bool new_entry, bool new_secret)
    {
        return m_db->try_execute_bound_statement(m_count, [](auto&) -> ErrorOr<void> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void> {
            if (TRY(row.read_integer<i64>("entries"sv)) + new_entry > static_cast<i64>(max_entries) || TRY(row.read_integer<i64>("secrets"sv)) + new_secret > static_cast<i64>(max_secrets))
                return Error::from_string_literal("Credential catalog exceeds limit");
            return {};
        });
    }
    ErrorOr<i64> next_revision()
    {
        i64 revision = -1;
        TRY(m_db->try_execute_bound_statement(m_revision, [](auto&) -> ErrorOr<void> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void> { revision = TRY(row.read_integer<i64>("revision"sv)); return {}; }));
        if (revision < 0 || revision == NumericLimits<i64>::max()) return Error::from_string_literal("Credential revision exhausted");
        ++revision;
        TRY(m_db->try_execute_bound_statement(m_bump, [&](auto& bind) { return bind("revision"sv,revision); }));
        return revision;
    }
    NonnullRefPtr<Database::Database> m_db;
    Database::StatementID m_blocked {}, m_blocked_all {}, m_blocked_count {}, m_block {}, m_allow {};
    Database::StatementID m_get {}, m_list {}, m_all {}, m_revision {}, m_bump {}, m_secret {}, m_prepare {}, m_count {}, m_state {}, m_put {}, m_delete {}, m_garbage {}, m_forget {};
};

}
