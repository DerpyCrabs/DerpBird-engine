#pragma once

#include "DerpBirdSecretStore.h"
#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <QElapsedTimer>
#include <QTimer>
#include <QUuid>

namespace Ladybird {

// A short-lived native save proposal. Only metadata/token may enter WebUI.
// The owner supplies canonical origin and a document/shell authority predicate;
// capture IPC alone is not authorization to store anything in the profile.
class DerpBirdCredentialProposal final {
public:
    explicit DerpBirdCredentialProposal(int lifetime_ms = 120000)
        : m_lifetime_ms(lifetime_ms)
    {
        VERIFY(lifetime_ms > 0);
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { clear(); });
    }
    ~DerpBirdCredentialProposal() { on_change = {}; clear(); }
    Function<void()> on_change;
    JsonValue metadata()
    {
        if (!m_pending.has_value()) return {};
        if (m_started.elapsed() >= m_lifetime_ms || !m_authority || !m_authority()) { clear(); return {}; }
        JsonObject value;
        value.set("token"sv, m_pending->token);
        value.set("origin"sv, m_pending->origin);
        value.set("username"sv, m_pending->username);
        return value;
    }
    ErrorOr<JsonValue> propose(String origin, String username, String const& password, Function<bool()> authority)
    {
        clear();
        if (!authority || !authority() || origin.bytes().size() > 2048 || username.bytes().size() > 1024
            || password.is_empty() || password.bytes().size() > DerpBirdSecretStore::max_secret_bytes)
            return Error::from_string_literal("Credential capture is unavailable or exceeds limits");
        auto url = URL::Parser::basic_parse(origin);
        if (!url.has_value() || !url->scheme().is_one_of("http"sv, "https"sv) || url->origin().serialize() != origin)
            return Error::from_string_literal("Invalid credential capture origin");
        auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
        auto token = TRY(String::from_utf8(StringView { uuid.constData(), static_cast<size_t>(uuid.size()) }));
        DerpBirdSecret secret { secret_value_new(password.bytes_as_string_view().characters_without_null_termination(), password.bytes().size(), "application/octet-stream") };
        JsonObject metadata;
        metadata.set("token"sv, token);
        metadata.set("origin"sv, origin);
        metadata.set("username"sv, username);
        m_pending = Pending { move(token), move(origin), move(username), move(secret) };
        m_authority = move(authority);
        m_started.start();
        m_timer.start(m_lifetime_ms);
        if (on_change) on_change();
        return JsonValue { move(metadata) };
    }
    // Native caller sends this directly to the credential manager, never WebUI.
    // Existing id/revision means an explicitly confirmed update; service performs
    // origin matching and CAS. Empty id/revision0 means a new saved credential.
    ErrorOr<JsonValue> take(String const& token, String const& id, String const& revision)
    {
        if (!m_pending.has_value() || m_pending->token != token)
            return Error::from_string_literal("Credential proposal is unavailable");
        auto pending = m_pending.release_value();
        auto authority = move(m_authority);
        if (on_change) on_change();
        m_timer.stop();
        if (m_started.elapsed() >= m_lifetime_ms || !authority || !authority())
            return Error::from_string_literal("Credential proposal expired");
        if (revision.bytes().size() > 19 || id.bytes().size() > 64)
            return Error::from_string_literal("Invalid credential update identity");
        auto parsed = revision.to_number<i64>();
        if (!parsed.has_value() || *parsed < 0 || String::number(*parsed) != revision
            || (id.is_empty() != (*parsed == 0)))
            return Error::from_string_literal("Invalid credential update identity");
        for (auto c : id.bytes())
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                return Error::from_string_literal("Invalid credential update identity");
        auto password = TRY(String::from_utf8(pending.secret.bytes()));
        JsonObject payload;
        payload.set("type"sv, "credentials.save"sv);
        payload.set("id"sv, id);
        payload.set("revision"sv, revision);
        payload.set("origin"sv, move(pending.origin));
        payload.set("username"sv, move(pending.username));
        payload.set("password"sv, move(password));
        return JsonValue { move(payload) };
    }
    void cancel(String const& token) { if (m_pending.has_value() && m_pending->token == token) clear(); }
    void clear()
    {
        m_timer.stop();
        bool changed = m_pending.has_value();
        m_pending.clear();
        m_authority = {};
        if (changed && on_change) on_change();
    }
private:
    struct Pending { String token; String origin; String username; DerpBirdSecret secret; };
    Optional<Pending> m_pending;
    Function<bool()> m_authority;
    QTimer m_timer;
    QElapsedTimer m_started;
    int m_lifetime_ms;
};

}
