#pragma once

#include "DerpBirdValueService.h"
#include <LibWeb/Page/DerpBirdScriptRegistry.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/PageId.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <QHash>
#include <QUuid>

namespace Ladybird {

// One authority per profile (and a separate one for private browsing). All calls
// run on the service's UI thread. Context identifiers come from native connection
// and navigation state, never from JavaScript arguments. Only native selection
// of an installed script may issue a capability; this is not an IPC open API.
class DerpBirdValueAuthority {
public:
    struct Context {
        u64 connection;
        Web::PageId page;
        Web::HTML::CrossProcessId document;
        bool operator==(Context const&) const = default;
    };
    using Method = Web::DerpBirdValueProtocol::Method;
    struct Request {
        Method method;
        bool asynchronous;
        String key;
        Vector<DerpBirdValueService::Store::Update> updates;
    };

    explicit DerpBirdValueAuthority(DerpBirdValueService& service, bool is_private = false)
        : m_service(service), m_state(std::make_shared<State>()), m_is_private(is_private) { }
    ~DerpBirdValueAuthority()
    {
        if (m_hook_installed) m_service.set_change_callback({});
        m_state->reservations.clear();
        m_state->grants.clear();
        m_state->subscriptions.clear();
        m_state->index.clear();
        m_state->counts.clear();
    }
    DerpBirdValueAuthority(DerpBirdValueAuthority const&) = delete;
    DerpBirdValueAuthority& operator=(DerpBirdValueAuthority const&) = delete;

    // Called only after the trusted profile store accepted an update. Validation
    // is atomic; bad snapshots cannot revoke or grant any capability.
    ErrorOr<void> replace_registry(JsonValue const& snapshot)
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        // Validate before filtering so a malformed snapshot stays atomic.
        Web::DerpBirdScriptRegistry validated;
        TRY(validated.replace(snapshot));
        TRY(m_registry.replace(Web::derpbird_scripts_for_context(snapshot, m_is_private)));
        m_state->grants.removeIf([&](auto const& item) {
            for (auto const& definition : m_registry.definitions()) {
                if (definition.id == item.value().script_id && definition.enabled && !definition.grant_none
                    && definition.configuration == item.value().configuration)
                    return false;
            }
            return true;
        });
        prune_subscriptions();
        return {};
    }

    ErrorOr<QString> issue(Context context, String const& script_id, URL::URL const& url, bool top_level)
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        auto definition_it = m_registry.definitions().find_if([&](auto const& item) { return item.id == script_id; });
        if (definition_it == m_registry.definitions().end())
            return Error::from_string_literal("Userscript is not installed");
        auto const& definition = *definition_it;
        if (!definition.enabled || definition.grant_none || !context.connection || context.page == 0
            || !context.document.namespace_id || !context.document.local_id)
            return Error::from_string_literal("Userscript storage capability denied");
        Web::DerpBirdScriptMatchBudget budget;
        if (definition.url_policy.evaluate(url, top_level, definition.file_access_granted, budget) != Web::DerpBirdScriptEligibility::Allowed)
            return Error::from_string_literal("Userscript storage document is not eligible");
        if (m_state->grants.size() >= 4096)
            return Error::from_string_literal("Too many userscript storage capabilities");
        auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_state->grants.insert(token, Grant { context, definition.id, definition.grants, definition.configuration });
        return token;
    }

    ErrorOr<String> tab_data_permission(Context context, QString const& token, Method method) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript tab-data capability");
        StringView permission;
        switch (method) {
        case Method::TabGet: permission="GM.getTab"sv; break;
        case Method::TabGetLegacy: permission="GM_getTab"sv; break;
        case Method::TabSave: permission="GM.saveTab"sv; break;
        case Method::TabSaveLegacy: permission="GM_saveTab"sv; break;
        case Method::TabsGet: permission="GM.getTabs"sv; break;
        case Method::TabsGetLegacy: permission="GM_getTabs"sv; break;
        default: return Error::from_string_literal("Invalid userscript tab-data method");
        }
        if (!found->permissions.contains_slow(permission)) return Error::from_string_literal("Userscript tab-data API was not granted");
        return found->script_id;
    }
    struct MenuIdentity { String id; String name; };
    ErrorOr<MenuIdentity> menu_permission(Context context, QString const& token, Method method) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript menu capability");
        StringView permission;
        switch (method) {
        case Method::MenuRegister: permission="GM.registerMenuCommand"sv; break;
        case Method::MenuRegisterLegacy: permission="GM_registerMenuCommand"sv; break;
        case Method::MenuRemove: permission="GM.unregisterMenuCommand"sv; break;
        case Method::MenuRemoveLegacy: permission="GM_unregisterMenuCommand"sv; break;
        case Method::MenuWait: permission=found->permissions.contains_slow("GM.registerMenuCommand"sv) ? "GM.registerMenuCommand"sv : "GM_registerMenuCommand"sv; break;
        default: return Error::from_string_literal("Invalid userscript menu method");
        }
        if (!found->permissions.contains_slow(permission)) return Error::from_string_literal("Userscript menu API was not granted");
        for (auto const& definition : m_registry.definitions())
            if (definition.id == found->script_id && definition.configuration == found->configuration) return MenuIdentity { definition.id, definition.name };
        return Error::from_string_literal("Userscript menu capability expired");
    }
    ErrorOr<void> cookie_permission(Context context, QString const& token, Method method, URL::URL const& url) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript cookie capability");
        auto permission = method == Method::Cookie ? "GM.cookie"sv : method == Method::CookieLegacy ? "GM_cookie"sv : ""sv;
        if (permission.is_empty() || !found->permissions.contains_slow(permission))
            return Error::from_string_literal("Userscript cookie API was not granted");
        if ((url.scheme() != "http"sv && url.scheme() != "https"sv) || !url.username().is_empty() || !url.password().is_empty())
            return Error::from_string_literal("Invalid userscript cookie URL");
        for (auto const& definition : m_registry.definitions()) {
            if (definition.id != found->script_id || definition.configuration != found->configuration) continue;
            Web::DerpBirdScriptMatchBudget budget;
            if (definition.url_policy.evaluate(url, true, definition.file_access_granted, budget) == Web::DerpBirdScriptEligibility::Allowed)
                return {};
        }
        return Error::from_string_literal("Cookie URL is outside userscript match permissions");
    }
    ErrorOr<MenuIdentity> notification_permission(Context context, QString const& token, Method method) const
    {
        auto found=m_state->grants.constFind(token);
        if(found==m_state->grants.cend() || found->context!=context) return Error::from_string_literal("Invalid notification capability");
        bool modern=found->permissions.contains_slow("GM.notification"sv), legacy=found->permissions.contains_slow("GM_notification"sv);
        if (!(method==Method::Notify ? modern : method==Method::NotifyLegacy ? legacy : (method==Method::NotifyWait || method==Method::NotifyActivate) && (modern||legacy)))
            return Error::from_string_literal("Userscript notification API was not granted");
        for(auto const& definition:m_registry.definitions())
            if(definition.id==found->script_id && definition.configuration==found->configuration) return MenuIdentity{definition.id,definition.name};
        return Error::from_string_literal("Notification capability expired");
    }
    ErrorOr<void> download_permission(Context context, QString const& token, Method method) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript download capability");
        bool modern = found->permissions.contains_slow("GM.download"sv);
        bool legacy = found->permissions.contains_slow("GM_download"sv);
        if (method == Method::Download ? modern : method == Method::DownloadLegacy ? legacy
            : (method == Method::DownloadAbort || method == Method::DownloadWatch || method == Method::DownloadChunk || method == Method::DownloadFinish) && (modern || legacy)) return {};
        return Error::from_string_literal("Userscript download API was not granted");
    }
    ErrorOr<void> tab_permission(Context context, QString const& token, Method method) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript tab capability");
        bool modern = found->permissions.contains_slow("GM.openInTab"sv);
        bool legacy = found->permissions.contains_slow("GM_openInTab"sv);
        if (method == Method::TabOpen ? modern : method == Method::TabOpenLegacy ? legacy
            : (method == Method::TabClose || method == Method::TabWatch) && (modern || legacy)) return {};
        return Error::from_string_literal("Userscript tab API was not granted");
    }
    ErrorOr<void> audio_permission(Context context, QString const& token, Method method) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript audio capability");
        auto permission = method == Method::Audio ? "GM.audio"sv : method == Method::AudioLegacy ? "GM_audio"sv : ""sv;
        if (permission.is_empty() || !found->permissions.contains_slow(permission))
            return Error::from_string_literal("Userscript audio API was not granted");
        return {};
    }
    ErrorOr<void> window_permission(Context context, QString const& token, Method method) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript window capability");
        auto permission = method == Method::WindowClose ? "window.close"sv
            : method == Method::WindowFocus ? "window.focus"sv : ""sv;
        if (permission.is_empty() || !found->permissions.contains_slow(permission))
            return Error::from_string_literal("Userscript window API was not granted");
        return {};
    }
    ErrorOr<void> clipboard_permission(Context context, QString const& token, Method method) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript clipboard capability");
        auto permission = method == Method::Clipboard ? "GM.setClipboard"sv
            : method == Method::ClipboardLegacy ? "GM_setClipboard"sv : ""sv;
        if (permission.is_empty() || !found->permissions.contains_slow(permission))
            return Error::from_string_literal("Userscript clipboard API was not granted");
        return {};
    }
    ErrorOr<Vector<String>> network_policy(Context context, QString const& token, Method method) const
    {
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context)
            return Error::from_string_literal("Invalid userscript network capability");
        bool allowed = method == Method::Network ? found->permissions.contains_slow("GM.xmlHttpRequest"sv)
            : method == Method::NetworkLegacy ? found->permissions.contains_slow("GM_xmlhttpRequest"sv)
            : (method == Method::NetworkAbort || method == Method::NetworkRead) && (found->permissions.contains_slow("GM.xmlHttpRequest"sv) || found->permissions.contains_slow("GM_xmlhttpRequest"sv));
        if (!allowed) return Error::from_string_literal("Userscript network API was not granted");
        for (auto const& definition : m_registry.definitions())
            if (definition.id == found->script_id && definition.configuration == found->configuration)
                return definition.connects;
        return Error::from_string_literal("Userscript network capability expired");
    }

    ErrorOr<void> submit(Context context, QString const& token, Request request, DerpBirdValueService::Completion completion)
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        auto found = m_state->grants.constFind(token);
        if (found == m_state->grants.cend() || found->context != context || !completion)
            return Error::from_string_literal("Invalid userscript storage capability");
        auto grant = method_grant(request.method, request.asynchronous);
        if (grant.is_empty() || !found->permissions.contains_slow(grant))
            return Error::from_string_literal("Userscript storage method was not granted");
        using Operation = DerpBirdValueService::Operation;
        Operation operation;
        switch (request.method) {
        case Method::Get:
            if (!request.updates.is_empty()) return Error::from_string_literal("Unexpected storage updates");
            operation = Operation::Get;
            break;
        case Method::List:
            if (!request.updates.is_empty() || !request.key.is_empty()) return Error::from_string_literal("Unexpected storage arguments");
            operation = Operation::Keys;
            break;
        case Method::GetMany:
            if (!request.key.is_empty()) return Error::from_string_literal("Unexpected storage arguments");
            for (auto const& entry : request.updates)
                if (entry.value.has_value()) return Error::from_string_literal("Read selection cannot contain values");
            operation = request.updates.is_empty() ? Operation::Snapshot : Operation::Selected;
            break;
        case Method::Set:
        case Method::Delete:
        case Method::SetMany:
        case Method::DeleteMany: {
            bool single = request.method == Method::Set || request.method == Method::Delete;
            bool deletion = request.method == Method::Delete || request.method == Method::DeleteMany;
            if (!request.key.is_empty() || (single && request.updates.size() != 1))
                return Error::from_string_literal("Invalid storage mutation arguments");
            for (auto const& update : request.updates) {
                if (update.value.has_value() == deletion)
                    return Error::from_string_literal("Storage mutation does not match granted method");
            }
            operation = Operation::Apply;
            break;
        }
        default: return Error::from_string_literal("Unknown userscript storage method");
        }
        auto script_id = found->script_id;
        // Values contain no caller-selected storage identity. An accepted write
        // may commit after revocation; it was authorized at submission. Its reply
        // must not disclose data to a later document or a revoked script.
        auto origin_utf8 = token.toUtf8();
        auto origin = TRY(String::from_utf8(StringView(origin_utf8.constData(), origin_utf8.size())));
        auto sequence = m_service.last_sequence() + 1;
        std::shared_ptr<Reservations> reservations;
        if (operation == Operation::Apply) {
            if (request.updates.size() > 256) return Error::from_string_literal("Too many storage updates");
            auto indexed = m_state->index.find(script_id);
            if (indexed != m_state->index.end()) {
                HashTable<String> keys;
                for (auto const& update : request.updates) {
                    if (update.key.bytes().size() > 1024 || keys.set(update.key) == AK::HashSetResult::ReplacedExistingEntry
                        || (update.value.has_value() && update.value->size() > DerpBirdValueService::Store::max_value_bytes))
                        return Error::from_string_literal("Invalid storage update for event admission");
                    auto listeners = indexed->value.find(update.key);
                    if (listeners == indexed->value.end()) continue;
                    for (auto id : listeners->value) {
                        auto subscription = m_state->subscriptions.get(id).value();
                        if (!subscription->reserve) continue;
                        // Old data may have changed in an earlier queued transaction.
                        // Reserve its maximum plus this write's known new payload.
                        auto bytes = DerpBirdValueService::Store::max_value_bytes + 4096
                            + (update.value.has_value() ? update.value->size() : 0);
                        auto notification = TRY(subscription->reserve(id, bytes));
                        if (!notification) return Error::from_string_literal("Missing reserved event delivery");
                        if (!reservations) reservations = std::make_shared<Reservations>();
                        reservations->notifications.set(id, move(notification));
                    }
                }
            }
        }
        if (reservations) m_state->reservations.set(sequence, reservations);
        auto accepted = m_service.submit({ operation, move(script_id), move(request.key), move(request.updates), {}, move(origin) },
            [state = m_state, context, token, sequence, completion = move(completion)](auto result) mutable {
                state->reservations.remove(sequence);
                auto active = state->grants.constFind(token);
                if (active == state->grants.cend() || active->context != context) {
                    completion(Error::from_string_literal("Userscript storage capability was revoked"));
                    return;
                }
                completion(move(result));
            });
        if (accepted.is_error()) m_state->reservations.remove(sequence);
        return accepted;
    }

    // Native dispatch only: copy/serialize before returning. Do not spin a nested
    // event loop here; JS callbacks run later in their WebContent process.
    using Notification = Function<void(u64, i64, DerpBirdValueService::Store::Change const&, bool)>;
    // Factories must only reserve native transport capacity; no nested event loop
    // or authority mutation. The returned callable owns and consumes that lease.
    using ReserveNotification = Function<ErrorOr<Notification>(u64, size_t)>;
    ErrorOr<u64> subscribe(Context context, QString const& token, String key, bool asynchronous, Notification notify, ReserveNotification reserve = {})
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        auto found = m_state->grants.constFind(token);
        auto permission = asynchronous ? "GM.addValueChangeListener"sv : "GM_addValueChangeListener"sv;
        if (found == m_state->grants.cend() || found->context != context || !found->permissions.contains_slow(permission))
            return Error::from_string_literal("Userscript value listener was not granted");
        if ((!notify && !reserve) || key.bytes().size() > 1024 || m_state->subscriptions.size() >= 4096
            || m_state->counts.value(token) >= 128 || m_state->next_subscription == NumericLimits<u64>::max())
            return Error::from_string_literal("Userscript value listener exceeds limits");
        auto id = m_state->next_subscription++;
        auto subscription = std::make_shared<Subscription>(Subscription {
            token, context, found->script_id, move(key), m_service.last_sequence(), move(notify), move(reserve) });
        m_state->index.ensure(subscription->script_id).ensure(subscription->key).append(id);
        ++m_state->counts[token];
        m_state->subscriptions.set(id, move(subscription));
        if (!m_hook_installed) {
            m_hook_installed = true;
            m_service.set_change_callback([state = m_state](String const& script_id, String const& origin,
                                              DerpBirdValueService::Store::Commit const& commit, u64 sequence) {
                deliver(*state, script_id, origin, commit, sequence);
            });
        }
        return id;
    }

    ErrorOr<bool> unsubscribe(Context context, QString const& token, u64 id, bool asynchronous)
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        auto grant = m_state->grants.constFind(token);
        auto permission = asynchronous ? "GM.removeValueChangeListener"sv : "GM_removeValueChangeListener"sv;
        if (grant == m_state->grants.cend() || grant->context != context || !grant->permissions.contains_slow(permission))
            return Error::from_string_literal("Userscript listener removal was not granted");
        auto found = m_state->subscriptions.find(id);
        if (found == m_state->subscriptions.end()) return false;
        if (found->value->token != token || found->value->context != context)
            return Error::from_string_literal("Invalid userscript listener owner");
        remove_subscription(id);
        release_unused_hook();
        return true;
    }
    bool cancel_subscription(Context context, QString const& token, u64 id)
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        auto found = m_state->subscriptions.find(id);
        if (found == m_state->subscriptions.end() || found->value->token != token || found->value->context != context) return false;
        remove_subscription(id);
        release_unused_hook();
        return true;
    }
    bool cancel_subscription(Context context, u64 id)
    {
        auto found = m_state->subscriptions.find(id);
        if (found == m_state->subscriptions.end()) return false;
        auto token = found->value->token;
        return cancel_subscription(context, token, id);
    }
    size_t subscription_count() const { return m_state->subscriptions.size(); }

    void revoke_document(Context context)
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        m_state->grants.removeIf([&](auto const& item) { return item.value().context == context; });
        prune_subscriptions();
    }
    void revoke_connection(u64 connection)
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        m_state->grants.removeIf([&](auto const& item) { return item.value().context.connection == connection; });
        prune_subscriptions();
    }
    void revoke_script(String const& script_id)
    {
        VERIFY(QThread::currentThread() == m_service.thread());
        m_state->grants.removeIf([&](auto const& item) { return item.value().script_id == script_id; });
        prune_subscriptions();
    }

private:
    static StringView method_grant(Method method, bool asynchronous)
    {
        switch (method) {
        case Method::TabGet: case Method::TabGetLegacy: case Method::TabSave: case Method::TabSaveLegacy: case Method::TabsGet: case Method::TabsGetLegacy:
        case Method::MenuRegister: case Method::MenuRegisterLegacy: case Method::MenuRemove: case Method::MenuRemoveLegacy: case Method::MenuWait:
        case Method::TabOpen: case Method::TabOpenLegacy: case Method::TabClose: case Method::TabWatch:
        case Method::Clipboard: case Method::ClipboardLegacy:
        case Method::Network: case Method::NetworkLegacy: case Method::NetworkAbort: case Method::NetworkRead:
        case Method::Download: case Method::DownloadLegacy: case Method::DownloadAbort: case Method::DownloadWatch: case Method::DownloadChunk: case Method::DownloadFinish:
        case Method::Notify: case Method::NotifyLegacy: case Method::NotifyWait: case Method::NotifyActivate:
        case Method::Cookie: case Method::CookieLegacy:
        case Method::WindowClose: case Method::WindowFocus: case Method::Audio: case Method::AudioLegacy:
        case Method::Open: case Method::Subscribe: case Method::Unsubscribe: return {};
        case Method::Get: return asynchronous ? "GM.getValue"sv : "GM_getValue"sv;
        case Method::Set: return asynchronous ? "GM.setValue"sv : "GM_setValue"sv;
        case Method::Delete: return asynchronous ? "GM.deleteValue"sv : "GM_deleteValue"sv;
        case Method::List: return asynchronous ? "GM.listValues"sv : "GM_listValues"sv;
        case Method::GetMany: return asynchronous ? "GM.getValues"sv : "GM_getValues"sv;
        case Method::SetMany: return asynchronous ? "GM.setValues"sv : "GM_setValues"sv;
        case Method::DeleteMany: return asynchronous ? "GM.deleteValues"sv : "GM_deleteValues"sv;
        }
        return {};
    }
    struct Grant { Context context; String script_id; Vector<String> permissions; String configuration; };
    struct Subscription {
        QString token;
        Context context;
        String script_id;
        String key;
        u64 after_sequence;
        Notification notify;
        ReserveNotification reserve;
    };
    struct Reservations { HashMap<u64, Notification> notifications; };
    struct State {
        HashMap<u64, std::shared_ptr<Reservations>> reservations;
        QHash<QString, Grant> grants;
        HashMap<u64, std::shared_ptr<Subscription>> subscriptions;
        HashMap<String, HashMap<String, Vector<u64>>> index;
        QHash<QString, size_t> counts;
        u64 next_subscription { 1 };
    };
    static void deliver(State& state, String const& script_id, String const& origin,
        DerpBirdValueService::Store::Commit const& commit, u64 sequence)
    {
        auto origin_bytes = origin.bytes();
        auto origin_token = QString::fromUtf8(reinterpret_cast<char const*>(origin_bytes.data()), origin_bytes.size());
        for (auto const& change : commit.changes) {
            auto script = state.index.find(script_id);
            if (script == state.index.end()) break;
            auto key = script->value.find(change.key);
            if (key == script->value.end()) continue;
            // Delivery may synchronously revoke/remove/register listeners. Iterate
            // stable IDs, recheck ownership each time and retain only current call.
            auto ids = key->value;
            for (auto id : ids) {
                auto found = state.subscriptions.find(id);
                if (found == state.subscriptions.end()) continue;
                auto subscription = found->value;
                if (sequence <= subscription->after_sequence) continue;
                auto grant = state.grants.constFind(subscription->token);
                if (grant == state.grants.cend() || grant->context != subscription->context) continue;
                if (subscription->reserve) {
                    auto batch = state.reservations.find(sequence);
                    if (batch == state.reservations.end()) continue;
                    auto notification = batch->value->notifications.take(id);
                    if (!notification.has_value()) continue;
                    (*notification)(id, commit.revision, change, subscription->token != origin_token);
                } else {
                    subscription->notify(id, commit.revision, change, subscription->token != origin_token);
                }
            }
        }
    }
    void remove_subscription(u64 id)
    {
        auto removed = m_state->subscriptions.take(id);
        if (!removed.has_value()) return;
        for (auto& entry : m_state->reservations) entry.value->notifications.remove(id);
        auto const& subscription = **removed;
        auto script = m_state->index.find(subscription.script_id);
        if (script != m_state->index.end()) {
            auto key = script->value.find(subscription.key);
            if (key != script->value.end()) {
                key->value.remove_all_matching([&](u64 other) { return other == id; });
                if (key->value.is_empty()) script->value.remove(subscription.key);
            }
            if (script->value.is_empty()) m_state->index.remove(subscription.script_id);
        }
        auto count = m_state->counts.find(subscription.token);
        VERIFY(count != m_state->counts.end() && *count > 0);
        if (--*count == 0) m_state->counts.erase(count);
    }
    void release_unused_hook()
    {
        if (m_hook_installed && m_state->subscriptions.is_empty()) {
            m_service.set_change_callback({});
            m_hook_installed = false;
        }
    }
    void prune_subscriptions()
    {
        Vector<u64> removed;
        for (auto const& [id, subscription] : m_state->subscriptions) {
            if (!m_state->grants.contains(subscription->token)) removed.append(id);
        }
        for (auto id : removed) remove_subscription(id);
        release_unused_hook();
    }
    Web::DerpBirdScriptRegistry m_registry;
    DerpBirdValueService& m_service;
    std::shared_ptr<State> m_state;
    bool m_hook_installed { false };
    bool m_is_private { false };
};

}
