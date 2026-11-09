#pragma once

#include "DerpBirdSecretStore.h"
#include <LibWebView/WebUI/DerpBirdCredentialStore.h>
#include <QUuid>

namespace Ladybird {

// Worker-only coordinator. Metadata never points at a secret before a successful
// keyring write. Every new key is journaled first so interruption remains recoverable.
// The caller owns the catalog on this same worker and runs recover_pending once
// during exclusive profile startup, before accepting any operations.
class DerpBirdCredentialVault {
public:
    AK_ALLOC_WITH_KMALLOC;
    using Catalog = WebView::DerpBirdCredentialStore;
    DerpBirdCredentialVault(Catalog& catalog, String profile) : m_catalog(catalog), m_profile(move(profile)) { }
    ErrorOr<Catalog::Entry> save(String id, URL::URL const& document, String username,
        ReadonlyBytes secret, i64 expected_revision, GCancellable* cancel)
    {
        TRY(check_cancel(cancel));
        if (username.bytes().size() > 1024 || secret.size() > DerpBirdSecretStore::max_secret_bytes || expected_revision < 0)
            return Error::from_string_literal("Credential exceeds limits");
        auto origin = TRY(Catalog::origin_for(document));
        if (id.is_empty()) {
            if (expected_revision != 0) return Error::from_string_literal("Missing credential identity");
            id = TRY(new_id());
        } else {
            auto current = TRY(m_catalog.get(id, document));
            if (!current.has_value() || current->revision != expected_revision)
                return Error::from_string_literal("Credential changed; refresh before replacing");
        }
        auto secret_id = TRY(new_id());
        TRY(m_catalog.prepare_secret(secret_id));
        auto stored = DerpBirdSecretStore::put(m_profile, secret_id, secret, cancel);
        if (stored.is_error()) {
            retire_preparation(secret_id);
            return stored.release_error();
        }
        if (g_cancellable_is_cancelled(cancel)) {
            retire_preparation(secret_id);
            return Error::from_string_literal("Credential operation cancelled");
        }
        Catalog::Entry entry { move(id), move(origin), move(username), secret_id };
        auto published = m_catalog.put(entry, expected_revision);
        if (published.is_error()) {
            retire_preparation(secret_id);
            return published.release_error();
        }
        entry.revision = published.release_value();
        return entry;
    }
    ErrorOr<Optional<DerpBirdSecret>> load(String const& id, URL::URL const& document, GCancellable* cancel)
    {
        TRY(check_cancel(cancel));
        auto entry = TRY(m_catalog.get(id, document));
        if (!entry.has_value()) return Optional<DerpBirdSecret> {};
        auto secret = TRY(DerpBirdSecretStore::get(m_profile, entry->secret_id, cancel));
        if (!secret.has_value()) return Error::from_string_literal("Saved credential secret is unavailable");
        return secret;
    }
    ErrorOr<void> remove(String const& id, URL::URL const& document, i64 expected_revision, GCancellable* cancel)
    {
        TRY(check_cancel(cancel));
        // Revoke metadata immediately; deletion from keyring is durably queued.
        return m_catalog.remove(id, document, expected_revision);
    }
    ErrorOr<size_t> collect_garbage(GCancellable* cancel)
    {
        TRY(check_cancel(cancel));
        auto ids = TRY(m_catalog.garbage());
        size_t removed = 0;
        // Schedule further batches at idle; do not monopolize the worker queue.
        for (auto const& id : ids) {
            if (removed == 16) break;
            TRY(check_cancel(cancel));
            TRY(DerpBirdSecretStore::remove(m_profile, id, cancel));
            TRY(m_catalog.forget_garbage(id));
            ++removed;
        }
        return removed;
    }
private:
    static ErrorOr<void> check_cancel(GCancellable* cancel)
    {
        if (!cancel || g_cancellable_is_cancelled(cancel)) return Error::from_string_literal("Credential operation cancelled");
        return {};
    }
    static ErrorOr<String> new_id()
    {
        auto bytes = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
        return String::from_utf8(StringView(bytes.constData(), bytes.size()));
    }
    void retire_preparation(String const& secret_id)
    {
        // Failure leaves the existing pending journal row for startup recovery.
        // Never replace the original error or publish an unconfirmed credential.
        auto retired = m_catalog.abandon_secret(secret_id);
        if (retired.is_error()) warnln("Credential cleanup deferred until profile recovery");
    }
    Catalog& m_catalog;
    String m_profile;
};

}
