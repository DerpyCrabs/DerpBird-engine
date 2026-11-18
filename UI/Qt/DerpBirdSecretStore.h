#pragma once

#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>

namespace Ladybird {

// Native secret payloads never enter browser snapshots. Keep libsecret's owned
// SecretValue instead of copying plaintext into ordinary application buffers.
class DerpBirdSecret {
public:
    explicit DerpBirdSecret(SecretValue* value) : m_value(value) { VERIFY(value); }
    DerpBirdSecret(DerpBirdSecret&& other) : m_value(exchange(other.m_value, nullptr)) { }
    DerpBirdSecret& operator=(DerpBirdSecret&& other)
    {
        if (this != &other) { clear(); m_value = exchange(other.m_value, nullptr); }
        return *this;
    }
    ~DerpBirdSecret() { clear(); }
    DerpBirdSecret(DerpBirdSecret const&) = delete;
    DerpBirdSecret& operator=(DerpBirdSecret const&) = delete;
    ReadonlyBytes bytes() const
    {
        if (!m_value) return {};
        gsize length = 0;
        auto* bytes = secret_value_get(m_value, &length);
        return { reinterpret_cast<u8 const*>(bytes), length };
    }
    bool matches(DerpBirdSecret const& other) const
    {
        auto left = bytes();
        auto right = other.bytes();
        if (left.size() != right.size())
            return false;
        // Read every byte even when an earlier pair already differs.
        u8 const volatile* left_data = left.data();
        u8 const volatile* right_data = right.data();
        u8 difference = 0;
        for (size_t i = 0; i < left.size(); ++i)
            difference |= left_data[i] ^ right_data[i];
        return difference == 0;
    }

private:
    void clear() { if (m_value) secret_value_unref(exchange(m_value, nullptr)); }
    SecretValue* m_value;
};

// Linux platform primitive, called ONLY on a cancellable worker, never the UI
// thread. A missing/locked secret service is an error; there is no plaintext file
// fallback. Profile and credential identifiers are opaque native-generated IDs.
// Private browsing must not use this persistent provider.
class DerpBirdSecretStore {
public:
    static constexpr size_t max_secret_bytes = 64 * 1024;
    static ErrorOr<void> put(String const& profile, String const& credential, ReadonlyBytes bytes, GCancellable* cancel)
    {
        TRY(validate(profile, credential, cancel));
        if (bytes.size() > max_secret_bytes) return Error::from_string_literal("Credential exceeds limit");
        auto* attributes = attributes_for(profile, credential);
        ScopeGuard release_attributes = [&] { g_hash_table_unref(attributes); };
        auto* secret = secret_value_new(bytes.is_empty() ? "" : reinterpret_cast<char const*>(bytes.data()), bytes.size(), "application/octet-stream");
        ScopeGuard release_secret = [&] { secret_value_unref(secret); };
        GError* error = nullptr;
        auto stored = secret_password_storev_binary_sync(&schema(), attributes, SECRET_COLLECTION_DEFAULT,
            "DerpBird credential", secret, cancel, &error);
        ScopeGuard release_error = [&] { if (error) g_error_free(error); };
        if (error || !stored) { reset_failed_service(error); return Error::from_string_literal("Cannot store credential in system secret service"); }
        return {};
    }
    static ErrorOr<Optional<DerpBirdSecret>> get(String const& profile, String const& credential, GCancellable* cancel)
    {
        TRY(validate(profile, credential, cancel));
        auto* attributes = attributes_for(profile, credential);
        ScopeGuard release_attributes = [&] { g_hash_table_unref(attributes); };
        GError* error = nullptr;
        auto* secret = secret_password_lookupv_binary_sync(&schema(), attributes, cancel, &error);
        ScopeGuard release = [&] { if (secret) secret_value_unref(secret); if (error) g_error_free(error); };
        if (error) { reset_failed_service(error); return Error::from_string_literal("Cannot read credential from system secret service"); }
        if (!secret) return Optional<DerpBirdSecret> {};
        gsize length = 0;
        secret_value_get(secret, &length);
        if (length > max_secret_bytes) return Error::from_string_literal("Stored credential exceeds limit");
        return Optional<DerpBirdSecret> { DerpBirdSecret { exchange(secret, nullptr) } };
    }
    static ErrorOr<void> remove(String const& profile, String const& credential, GCancellable* cancel)
    {
        TRY(validate(profile, credential, cancel));
        auto* attributes = attributes_for(profile, credential);
        ScopeGuard release_attributes = [&] { g_hash_table_unref(attributes); };
        GError* error = nullptr;
        secret_password_clearv_sync(&schema(), attributes, cancel, &error);
        ScopeGuard release_error = [&] { if (error) g_error_free(error); };
        if (error) { reset_failed_service(error); return Error::from_string_literal("Cannot remove credential from system secret service"); }
        return {};
    }
private:
    static void reset_failed_service(GError const* error)
    {
        // libsecret's convenience API shares a cached service/session. A daemon
        // failure can invalidate that session. Let the next explicit operation
        // establish a fresh one; never automatically replay a credential write.
        // Cancellation alone does not indicate a failed provider.
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            secret_service_disconnect();
    }
    static bool valid_id(String const& id)
    {
        if (id.is_empty() || id.bytes().size() > 64) return false;
        for (auto ch : id.bytes())
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')) return false;
        return true;
    }
    static ErrorOr<void> validate(String const& profile, String const& credential, GCancellable* cancel)
    {
        if (!valid_id(profile) || !valid_id(credential) || !cancel || g_cancellable_is_cancelled(cancel))
            return Error::from_string_literal("Invalid or cancelled credential operation");
        return {};
    }
    static SecretSchema const& schema()
    {
        static SecretSchema schema = [] {
            SecretSchema value {};
            value.name = "org.derpbird.Credential.v1";
            value.flags = SECRET_SCHEMA_NONE;
            value.attributes[0] = { "profile", SECRET_SCHEMA_ATTRIBUTE_STRING };
            value.attributes[1] = { "credential", SECRET_SCHEMA_ATTRIBUTE_STRING };
            return value;
        }();
        return schema;
    }
    static GHashTable* attributes_for(String const& profile, String const& credential)
    {
        auto* attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        auto profile_bytes = profile.bytes();
        auto credential_bytes = credential.bytes();
        g_hash_table_insert(attributes, g_strdup("profile"), g_strndup(reinterpret_cast<char const*>(profile_bytes.data()), profile_bytes.size()));
        g_hash_table_insert(attributes, g_strdup("credential"), g_strndup(reinterpret_cast<char const*>(credential_bytes.data()), credential_bytes.size()));
        return attributes;
    }
};

}
