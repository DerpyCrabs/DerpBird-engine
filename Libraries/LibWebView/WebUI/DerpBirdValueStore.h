#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NumericLimits.h>
#include <LibDatabase/Database.h>
#include <LibDatabase/ResultRow.h>

namespace WebView {

// Single owning service/thread, as with Ladybird's other profile stores. Run
// persistent I/O on a worker, not on the chrome UI thread. The
// caller authenticates the script identity before using this API. Encoded values
// are opaque; deletion is distinct from an encoded JS undefined or empty blob.
class DerpBirdValueStore {
public:
    AK_ALLOC_WITH_KMALLOC;
    struct Update { String key; Optional<ByteBuffer> value; };
    struct Value { String key; ByteBuffer value; };
    struct Snapshot { i64 revision { 0 }; Vector<Value> values; };
    struct Change { String key; Optional<ByteBuffer> old_value; Optional<ByteBuffer> new_value; };
    struct Commit { i64 revision { 0 }; Vector<Change> changes; };
    static constexpr size_t max_value_bytes = 1024 * 1024;
    static constexpr size_t max_scope_bytes = 10 * 1024 * 1024;
    static constexpr size_t max_keys = 4096;

    static ErrorOr<NonnullOwnPtr<DerpBirdValueStore>> create(NonnullRefPtr<Database::Database> database)
    {
        TRY(database->set_busy_timeout(250));
        Array migrations { Database::Migration { .version = 1, .sql = R"SQL(
            CREATE TABLE GMScopes (
                id TEXT PRIMARY KEY NOT NULL,
                revision INTEGER NOT NULL DEFAULT 0 CHECK(revision >= 0),
                bytes INTEGER NOT NULL DEFAULT 0 CHECK(bytes >= 0),
                key_count INTEGER NOT NULL DEFAULT 0 CHECK(key_count >= 0)
            );
            CREATE TABLE GMValues (
                scope TEXT NOT NULL REFERENCES GMScopes(id) ON DELETE CASCADE,
                key TEXT NOT NULL, payload BLOB NOT NULL, size INTEGER NOT NULL CHECK(size >= 0),
                PRIMARY KEY(scope, key)
            ) WITHOUT ROWID;
        )SQL" } };
        if (TRY(database->migrate("DerpBirdGMValues"sv, migrations)) != Database::MigrationOutcome::Success)
            return Error::from_string_literal("Userscript value database is newer than this browser");
        auto store = adopt_own(*new DerpBirdValueStore(move(database)));
        auto& db = *store->m_database;
        store->m_scope = TRY(db.prepare_statement("SELECT revision, bytes, key_count FROM GMScopes WHERE id=:id"sv));
        store->m_get = TRY(db.prepare_statement("SELECT payload FROM GMValues WHERE scope=:id AND key=:key"sv));
        store->m_keys = TRY(db.prepare_statement("SELECT key FROM GMValues WHERE scope=:id ORDER BY key"sv));
        store->m_list = TRY(db.prepare_statement("SELECT key,payload FROM GMValues WHERE scope=:id ORDER BY key"sv));
        store->m_insert_scope = TRY(db.prepare_statement("INSERT INTO GMScopes(id) VALUES(:id) ON CONFLICT(id) DO NOTHING"sv));
        store->m_size = TRY(db.prepare_statement("SELECT size FROM GMValues WHERE scope=:id AND key=:key"sv));
        store->m_set = TRY(db.prepare_statement("INSERT INTO GMValues(scope,key,payload,size) VALUES(:id,:key,COALESCE(:payload,zeroblob(0)),:size) ON CONFLICT(scope,key) DO UPDATE SET payload=excluded.payload,size=excluded.size"sv));
        store->m_delete = TRY(db.prepare_statement("DELETE FROM GMValues WHERE scope=:id AND key=:key"sv));
        store->m_commit = TRY(db.prepare_statement("UPDATE GMScopes SET revision=:revision,bytes=:bytes,key_count=:count WHERE id=:id"sv));
        store->m_total = TRY(db.prepare_statement("SELECT COUNT(*) AS count,COALESCE(SUM(bytes),0) AS bytes FROM GMScopes"sv));
        return store;
    }

    ErrorOr<Optional<ByteBuffer>> get(String const& id, String const& key)
    {
        TRY(validate_id(id));
        if (key.bytes().size() > 1024)
            return Error::from_string_literal("Userscript value key exceeds quota");
        Optional<ByteBuffer> result;
        TRY(m_database->try_execute_bound_statement(m_get, 1, [&](auto& bind) -> ErrorOr<void> {
            TRY(bind("id"sv, id));
            return bind("key"sv, key);
        }, [&](Database::ResultRow& row) -> ErrorOr<void> {
            result = TRY(row.read_blob("payload"sv, max_value_bytes));
            return {};
        }));
        return result;
    }

    ErrorOr<Vector<String>> keys(String const& id)
    {
        TRY(validate_id(id));
        return m_database->try_collect_bound_statement<String>(m_keys, max_keys,
            [&](auto& bind) { return bind("id"sv, id); },
            [&](Database::ResultRow& row) { return row.read_text("key"sv, 1024); });
    }

    ErrorOr<Snapshot> selected(String const& id, ReadonlySpan<Update> selection)
    {
        TRY(validate_id(id));
        if (selection.size() > 256) return Error::from_string_literal("Too many selected storage keys");
        HashTable<String> keys;
        for (auto const& entry : selection) {
            if (entry.key.bytes().size() > 1024 || entry.value.has_value() || keys.contains(entry.key))
                return Error::from_string_literal("Invalid storage selection");
            keys.set(entry.key);
        }
        Snapshot result;
        TRY(read_transaction([&]() -> ErrorOr<void> {
            result.revision = TRY(read_scope(id)).revision;
            size_t bytes = 0;
            for (auto const& entry : selection) {
                auto value = TRY(get(id, entry.key));
                if (!value.has_value()) continue;
                bytes += entry.key.bytes().size() + value->size();
                if (bytes > max_scope_bytes) return Error::from_string_literal("Userscript stored values exceed quota");
                result.values.append({ entry.key, value.release_value() });
            }
            return {};
        }));
        return result;
    }

    ErrorOr<Snapshot> snapshot(String const& id)
    {
        TRY(validate_id(id));
        Snapshot result;
        TRY(read_transaction([&]() -> ErrorOr<void> {
            auto scope = TRY(read_scope(id));
            result.revision = scope.revision;
            size_t bytes = 0;
            TRY(m_database->try_execute_bound_statement(m_list, max_keys,
                [&](auto& bind) { return bind("id"sv, id); },
                [&](Database::ResultRow& row) -> ErrorOr<void> {
                    auto key = TRY(row.read_text("key"sv, 1024));
                    auto value = TRY(row.read_blob("payload"sv, max_value_bytes));
                    bytes += key.bytes().size() + value.size();
                    if (bytes > max_scope_bytes)
                        return Error::from_string_literal("Userscript stored values exceed quota");
                    result.values.append({ move(key), move(value) });
                    return {};
                }));
            if (static_cast<i64>(bytes) != scope.bytes || static_cast<i64>(result.values.size()) != scope.count)
                return Error::from_string_literal("Userscript value accounting is corrupt");
            return {};
        }));
        return result;
    }

    // Optional CAS is for snapshot-based callers. Normal GM writes update only
    // the named keys, retaining concurrent writes to all other keys.
    ErrorOr<i64> apply(String const& id, ReadonlySpan<Update> updates, Optional<i64> expected_revision = {})
    {
        return TRY(apply_impl(id, updates, expected_revision, false)).revision;
    }

    // Values and revision are captured within the same write transaction. No
    // changes escape if validation, quota checks or COMMIT fail.
    ErrorOr<Commit> apply_with_changes(String const& id, ReadonlySpan<Update> updates, Optional<i64> expected_revision = {})
    {
        return apply_impl(id, updates, expected_revision, true);
    }

private:
    ErrorOr<Commit> apply_impl(String const& id, ReadonlySpan<Update> updates, Optional<i64> expected_revision, bool capture_changes)
    {
        TRY(validate_id(id));
        if (updates.size() > 256)
            return Error::from_string_literal("Too many userscript value changes");
        HashTable<String> keys;
        size_t incoming_bytes = 0;
        for (auto const& update : updates) {
            if (update.key.bytes().size() > 1024 || keys.contains(update.key)
                || (update.value.has_value() && update.value->size() > max_value_bytes))
                return Error::from_string_literal("Invalid or duplicate userscript value key/payload");
            incoming_bytes += update.key.bytes().size() + (update.value.has_value() ? update.value->size() : 0);
            if (incoming_bytes > max_scope_bytes)
                return Error::from_string_literal("Userscript value changes exceed quota");
            keys.set(update.key);
        }
        Commit commit;
        auto& revision = commit.revision;
        TRY(m_database->transaction([&]() -> ErrorOr<void> {
            auto scope = TRY(read_scope(id));
            if (expected_revision.has_value() && *expected_revision != scope.revision)
                return Error::from_string_literal("Userscript values changed; refresh before replacing");
            revision = scope.revision;
            if (updates.is_empty())
                return {};
            if (revision == NumericLimits<i64>::max())
                return Error::from_string_literal("Userscript value revision exhausted");
            TRY(m_database->try_execute_bound_statement(m_insert_scope, [&](auto& bind) { return bind("id"sv, id); }));
            for (auto const& update : updates) {
                Optional<i64> old_size;
                auto bind_key = [&](auto& bind) -> ErrorOr<void> {
                    TRY(bind("id"sv, id));
                    return bind("key"sv, update.key);
                };
                TRY(m_database->try_execute_bound_statement(m_size, bind_key, [&](Database::ResultRow& row) -> ErrorOr<void> {
                    old_size = TRY(row.read_integer<i64>("size"sv));
                    if (*old_size < 0 || *old_size > static_cast<i64>(max_value_bytes + 1024))
                        return Error::from_string_literal("Invalid stored userscript value size");
                    return {};
                }));
                if (capture_changes) {
                    Optional<ByteBuffer> old_value;
                    if (old_size.has_value()) {
                        old_value = TRY(get(id, update.key));
                        if (!old_value.has_value() || static_cast<i64>(old_value->size() + update.key.bytes().size()) != *old_size)
                            return Error::from_string_literal("Stored userscript value size is inconsistent");
                    }
                    bool same = old_value.has_value() == update.value.has_value()
                        && (!old_value.has_value() || old_value->bytes() == update.value->bytes());
                    if (!same) {
                        Optional<ByteBuffer> new_value;
                        if (update.value.has_value()) new_value = TRY(ByteBuffer::copy(update.value->bytes()));
                        commit.changes.append({ update.key, move(old_value), move(new_value) });
                    }
                }
                if (old_size.has_value()) { scope.bytes -= *old_size; --scope.count; }
                if (update.value.has_value()) {
                    auto size = static_cast<i64>(update.key.bytes().size() + update.value->size());
                    TRY(m_database->try_execute_bound_statement(m_set, [&](auto& bind) -> ErrorOr<void> {
                        TRY(bind_key(bind));
                        TRY(bind("payload"sv, update.value->bytes()));
                        return bind("size"sv, size);
                    }));
                    scope.bytes += size; ++scope.count;
                } else {
                    TRY(m_database->try_execute_bound_statement(m_delete, bind_key));
                }
            }
            if (scope.bytes < 0 || scope.bytes > static_cast<i64>(max_scope_bytes) || scope.count < 0 || scope.count > static_cast<i64>(max_keys))
                return Error::from_string_literal("Userscript value storage quota exceeded");
            ++revision;
            TRY(m_database->try_execute_bound_statement(m_commit, [&](auto& bind) -> ErrorOr<void> {
                TRY(bind("id"sv, id));
                TRY(bind("revision"sv, revision));
                TRY(bind("bytes"sv, scope.bytes));
                return bind("count"sv, scope.count);
            }));
            TRY(m_database->try_execute_bound_statement(m_total, [](auto&) -> ErrorOr<void> { return {}; }, [&](Database::ResultRow& row) -> ErrorOr<void> {
                if (TRY(row.read_integer<i64>("count"sv)) > 256 || TRY(row.read_integer<i64>("bytes"sv)) > 512 * 1024 * 1024)
                    return Error::from_string_literal("Profile userscript value quota exceeded");
                return {};
            }));
            return {};
        }));
        return commit;
    }

private:
    ErrorOr<void> read_transaction(Function<ErrorOr<void>()> callback)
    {
        // A WAL snapshot must not acquire the writer reservation used by the
        // Database::transaction() helper for read-modify-write operations.
        TRY(m_database->execute_raw("BEGIN;"));
        bool committed = false;
        ScopeGuard rollback = [&] {
            if (!committed) {
                auto result = m_database->execute_raw("ROLLBACK;");
                if (result.is_error()) warnln("Userscript value read rollback failed: {}", result.error());
            }
        };
        TRY(callback());
        TRY(m_database->execute_raw("COMMIT;"));
        committed = true;
        return {};
    }
    struct Scope { i64 revision { 0 }; i64 bytes { 0 }; i64 count { 0 }; };
    explicit DerpBirdValueStore(NonnullRefPtr<Database::Database> database) : m_database(move(database)) { }
    static ErrorOr<void> validate_id(String const& id)
    {
        if (id.is_empty() || id.bytes().size() > 128)
            return Error::from_string_literal("Invalid userscript storage identity");
        for (auto ch : id.bytes()) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
                return Error::from_string_literal("Invalid userscript storage identity");
        }
        return {};
    }
    ErrorOr<Scope> read_scope(String const& id)
    {
        Scope result;
        TRY(m_database->try_execute_bound_statement(m_scope, [&](auto& bind) { return bind("id"sv, id); }, [&](Database::ResultRow& row) -> ErrorOr<void> {
            result = { TRY(row.read_integer<i64>("revision"sv)), TRY(row.read_integer<i64>("bytes"sv)), TRY(row.read_integer<i64>("key_count"sv)) };
            if (result.revision < 0 || result.bytes < 0 || result.bytes > static_cast<i64>(max_scope_bytes) || result.count < 0 || result.count > static_cast<i64>(max_keys))
                return Error::from_string_literal("Invalid userscript value scope");
            return {};
        }));
        return result;
    }
    NonnullRefPtr<Database::Database> m_database;
    Database::StatementID m_scope {}, m_list {}, m_get {}, m_keys {}, m_insert_scope {}, m_size {}, m_set {}, m_delete {}, m_commit {}, m_total {};
};

}
