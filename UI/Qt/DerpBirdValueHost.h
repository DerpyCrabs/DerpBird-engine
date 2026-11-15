#pragma once

#include "DerpBirdValueDispatcher.h"
#include "DerpBirdNetworkRequest.h"
#include "DerpBirdScriptTabs.h"
#include "DerpBirdScriptAudio.h"
#include "DerpBirdScriptDownloads.h"
#include "DerpBirdScriptNotifications.h"
#include "DerpBirdTabData.h"
#include "DerpBirdScriptCookies.h"
#include "DerpBirdScriptCommands.h"
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Profile.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <QPointer>
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>

namespace Ladybird {

class DerpBirdValueHost final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;
    DerpBirdScriptTabs tabs { this };
    DerpBirdScriptAudio audio { this };
    DerpBirdScriptDownloads downloads { this };
    DerpBirdScriptNotifications notifications { this };
    DerpBirdScriptCommands commands { this };
    Function<ErrorOr<void>(WebView::WebContentPage&, bool)> window_action;
    Function<void(WebView::WebContentPage&, bool, Function<void(ErrorOr<void>)>)> window_action_async;
    Function<void(WebView::WebContentPage&, String const&, ByteBuffer, Function<void(ErrorOr<void>)>)> clipboard_write_async;
    explicit DerpBirdValueHost(WebView::Application& application)
        : m_application(application)
    {
        m_application.on_derpbird_audio_changed = [this](auto& view) { audio.changed(view); };
        m_application.on_derpbird_scripts_changed = [this](JsonValue const& registry) {
            m_registry = registry;
            MUST(m_menu_registry.replace(registry));
            m_context_commands.clear();
            for (auto& entry : m_entries)
                MUST(entry->authority.replace_registry(registry));
            audio.prune();
            commands.prune();
            downloads.prune();
            notifications.prune();
        };
        m_application.on_derpbird_value_request = [this](auto& page, auto& document, ByteBuffer payload, auto completion) {
            auto result = dispatch(page, document, payload.bytes(), completion);
            if (result.is_error()) completion(result.release_error());
        };
    }
    ~DerpBirdValueHost() override
    {
        for (auto task : m_network.values()) if (task) task->cancel();
        m_application.on_derpbird_audio_changed = nullptr;
        m_application.on_derpbird_value_request = nullptr;
        m_application.on_derpbird_scripts_changed = nullptr;
        // Normal quit uses shutdown() below. Unexpected teardown still drains
        // accepted writes before destroying QThreads or the QApplication.
        m_entries.clear();
        m_closing.clear();
    }
    JsonArray list_commands(WebView::WebContentPage const& page, Optional<Web::HTML::CrossProcessId> clicked_document = {})
    {
        auto result = commands.list(page, clicked_document);
        WebView::CanonicalNavigable const* selected = &page.traversable();
        if (clicked_document.has_value()) {
            selected = nullptr;
            page.traversable().for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable const& navigable) {
                if (navigable.active_document().id() != *clicked_document) return IterationDecision::Continue;
                selected = &navigable; return IterationDecision::Break;
            });
            if (!selected) return JsonArray {};
        }
        auto& document = selected->active_document();
        m_context_commands.removeIf([](auto const& item) {
            auto const& command = item.value();
            auto* page = command.client ? command.client->page(command.page_id) : nullptr;
            return !command.document || !page || !page->is_open()
                || !active_document(*page, *command.document).has_value();
        });
        Web::DerpBirdScriptMatchBudget budget;
        for (auto const& definition : m_menu_registry.definitions()) {
            if (result.size() >= 256) break;
            if (!definition.enabled || !definition.runs_in(page.client().session().is_private() == WebView::IsPrivate::Yes) || definition.run_at != Web::DerpBirdScriptRunAt::ContextMenu
                || definition.url_policy.evaluate(selected->active_document_url(), selected->is_top_level_traversable(), definition.file_access_granted, budget)
                    != Web::DerpBirdScriptEligibility::Allowed) continue;
            QString key;
            for (auto it = m_context_commands.cbegin(); it != m_context_commands.cend(); ++it) {
                if (it->document.ptr() == &document && it->script == definition.id && it->generation == definition.generation) {
                    key = it.key(); break;
                }
            }
            if (key.isEmpty()) {
                if (m_context_commands.size() >= 4096) break;
                key = QUuid::createUuid().toString(QUuid::WithoutBraces);
                m_context_commands.insert(key, ContextCommand { document.make_weak_ptr(), page.client().make_weak_ptr<WebView::WebContentClient>(), page.id(), definition.id, definition.generation, definition.configuration });
            }
            auto bytes = key.toUtf8();
            JsonObject item;
            item.set("id"sv, MUST(String::from_utf8(StringView { bytes.constData(), static_cast<size_t>(bytes.size()) })));
            item.set("script"sv, definition.name);
            item.set("label"sv, "Запустить скрипт"sv);
            item.set("title"sv, "Запустить на текущей странице"sv);
            item.set("accessKey"sv, ""sv);
            item.set("autoClose"sv, true);
            result.must_append(move(item));
        }
        return result;
    }
    ErrorOr<void> invoke_command(WebView::WebContentPage const& page, String const& id, JsonObject const& event, Optional<Web::HTML::CrossProcessId> clicked_document = {})
    {
        if (clicked_document.has_value()) {
            bool active = false;
            page.traversable().for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable const& navigable) {
                if (navigable.active_document().id() != *clicked_document) return IterationDecision::Continue;
                active = true;
                return IterationDecision::Break;
            });
            if (!active) return Error::from_string_literal("Context-menu document changed");
        }
        auto bytes = id.bytes_as_string_view();
        auto found = m_context_commands.constFind(QString::fromUtf8(bytes.characters_without_null_termination(), bytes.length()));
        if (found == m_context_commands.cend()) return commands.invoke(page, id, event, clicked_document);
        auto const& command = *found;
        auto* source_page = command.client ? command.client->page(command.page_id) : nullptr;
        if (!command.document || !source_page || !source_page->is_open()
            || !active_document(*source_page, *command.document).has_value()
            || (clicked_document.has_value() && command.document->id() != *clicked_document)
            || &source_page->traversable() != &page.traversable())
            return Error::from_string_literal("Context-menu script document changed");
        Optional<Web::HTML::CrossProcessId> selected_navigable;
        page.traversable().for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable const& navigable) {
            if (&navigable.active_document() != command.document.ptr()) return IterationDecision::Continue;
            selected_navigable = navigable.id(); return IterationDecision::Break;
        });
        auto target = selected_navigable.has_value() ? page.page_hosting_navigable(*selected_navigable) : RefPtr<WebView::WebContentPage> {};
        if (!target || !target->is_open()) return Error::from_string_literal("Context-menu script page is unavailable");
        target->client().async_invoke_derpbird_context_script(target->id(), command.document->id(), command.script, command.configuration);
        return {};
    }
    void shutdown(Function<void()> completion)
    {
        m_shutdown.append(move(completion));
        if (m_stopping) { finish_shutdown(); return; }
        m_stopping = true;
        audio.shutdown();
        downloads.shutdown();
        notifications.shutdown();
        for (auto task : m_network.values()) if (task) task->cancel();
        auto keys = m_entries.keys();
        for (auto key : keys) close_session(key);
        finish_shutdown();
    }

private:
    struct ContextCommand {
        WeakPtr<WebView::CanonicalDocument> document;
        WeakPtr<WebView::WebContentClient> client;
        Web::PageId page_id;
        String script;
        u64 generation;
        String configuration;
    };
    Web::DerpBirdScriptRegistry m_menu_registry;
    QHash<QString, ContextCommand> m_context_commands;
    struct Entry {
        Entry(QString directory, bool is_private, u64 serial)
            : service(move(directory), is_private), authority(service, is_private), serial(serial) { }
        DerpBirdTabData tab_data;
        DerpBirdValueService service;
        DerpBirdValueAuthority authority;
        u64 serial;
    };
    static Optional<bool> active_document(WebView::WebContentPage& page, WebView::CanonicalDocument& document)
    {
        Optional<bool> top_level;
        page.traversable().for_each_in_inclusive_subtree([&](WebView::CanonicalNavigable const& navigable) {
            if (&navigable.active_document() != &document) return IterationDecision::Continue;
            top_level = navigable.is_top_level_traversable();
            return IterationDecision::Break;
        });
        return top_level;
    }
    ErrorOr<void> dispatch(WebView::WebContentPage& page, WebView::CanonicalDocument& document,
        ReadonlyBytes payload, DerpBirdValueDispatcher::Completion& completion)
    {
        if (m_stopping) return Error::from_string_literal("Userscript storage is shutting down");
        auto top_level = active_document(page, document);
        if (!top_level.has_value()) return Error::from_string_literal("Userscript document is not active");
        auto& session = page.client().session();
        auto key = reinterpret_cast<quintptr>(&session);
        auto entry = m_entries.value(key);
        if (!entry) {
            bool is_private = session.is_private() == WebView::IsPrivate::Yes;
            auto directory = is_private ? QString {} : QString::fromUtf8(WebView::Application::profile().paths().data.characters()) + "/GMValues";
            entry = std::make_shared<Entry>(move(directory), is_private, m_next_serial++);
            TRY(entry->authority.replace_registry(m_registry));
            m_entries.insert(key, entry);
            session.on_derpbird_values_close = [weak = QPointer { this }, key] {
                if (weak) weak->close_session(key);
            };
        }
        auto context = DerpBirdValueAuthority::Context { page.client().child_id(), page.id(), document.id() };
        page.client().on_derpbird_values_close = [self = QPointer(this), weak = std::weak_ptr<Entry>(entry), connection = context.connection] {
            if (auto owner = weak.lock()) owner->authority.revoke_connection(connection);
            if (self) { self->audio.prune(); self->downloads.prune(); self->notifications.prune(); }
        };
        document.on_derpbird_values_revoke = [self = QPointer(this), weak = std::weak_ptr<Entry>(entry), client = page.client().make_weak_ptr<WebView::WebContentClient>(), context] {
            if (auto owner = weak.lock()) owner->authority.revoke_document(context);
            if (client) client->cancel_derpbird_value_events(context.page, context.document);
            if (self) { self->audio.prune(); self->commands.prune(); self->downloads.prune(); self->notifications.prune(); }
        };
        // Shared completion holder also handles synchronous dispatch failure. It
        // does not own a session/document or keep private browsing alive.
        using Protocol = Web::DerpBirdValueProtocol;
        auto decoded = TRY(Protocol::decode_request(payload));
        auto done = std::make_shared<DerpBirdValueDispatcher::Completion>(move(completion));
        if (decoded.method == Protocol::Method::Cookie || decoded.method == Protocol::Method::CookieLegacy) {
            auto result = [&]() -> ErrorOr<ByteBuffer> {
                if (!session.cookie_jar) return Error::from_string_literal("Cookie store is unavailable");
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                return DerpBirdScriptCookies::dispatch(*session.cookie_jar, document.creation_url(), decoded, [&](URL::URL const& url) {
                    return entry->authority.cookie_permission(context, token, decoded.method, url);
                }, session.is_private() == WebView::IsPrivate::Yes);
            }();
            (*done)(move(result));
            return {};
        }
        if (decoded.method == Protocol::Method::Audio || decoded.method == Protocol::Method::AudioLegacy) {
            auto result = [&]() -> ErrorOr<void> {
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                TRY(entry->authority.audio_permission(context, token, decoded.method));
                auto valid = [weak = std::weak_ptr<Entry>(entry), client = page.client().make_weak_ptr<WebView::WebContentClient>(), document = document.make_weak_ptr(), context, token, method = decoded.method] {
                    auto owner = weak.lock();
                    if (!owner || !client || !document) return false;
                    auto* page = client->page(context.page);
                    return page && page->is_open() && active_document(*page, *document).has_value()
                        && !owner->authority.audio_permission(context, token, method).is_error();
                };
                return audio.dispatch(page.view(), token, decoded, move(valid), [done](auto response) { (*done)(move(response)); });
            }();
            if (result.is_error()) (*done)(result.release_error());
            return {};
        }
        if (decoded.method == Protocol::Method::WindowClose || decoded.method == Protocol::Method::WindowFocus) {
            auto result = [&]() -> ErrorOr<ByteBuffer> {
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                TRY(entry->authority.window_permission(context, token, decoded.method));
                if (window_action_async) {
                    window_action_async(page, decoded.method == Protocol::Method::WindowClose, [done](ErrorOr<void> result) {
                        if (result.is_error()) (*done)(result.release_error());
                        else (*done)(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
                    });
                    return ByteBuffer {};
                }
                if (!window_action) return Error::from_string_literal("Window host is unavailable");
                TRY(window_action(page, decoded.method == Protocol::Method::WindowClose));
                return Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 });
            }();
            if (result.is_error() || !result.value().is_empty()) (*done)(move(result));
            return {};
        }
        if (decoded.method == Protocol::Method::Notify || decoded.method == Protocol::Method::NotifyLegacy || decoded.method == Protocol::Method::NotifyWait || decoded.method == Protocol::Method::NotifyActivate) {
            auto result = [&]() -> ErrorOr<void> {
                auto identity=decoded.identity.bytes_as_string_view();
                auto token=QString::fromUtf8(identity.characters_without_null_termination(),identity.length());
                auto script=TRY(entry->authority.notification_permission(context,token,decoded.method));
                auto valid=[weak=std::weak_ptr<Entry>(entry),client=page.client().make_weak_ptr<WebView::WebContentClient>(),document=document.make_weak_ptr(),context,token]{
                    auto owner=weak.lock();if(!owner||!client||!document)return false;auto* page=client->page(context.page);
                    return page&&page->is_open()&&active_document(*page,*document).has_value()&&!owner->authority.notification_permission(context,token,Protocol::Method::NotifyWait).is_error();
                };
                auto scope=QString::number(entry->serial)+":"+QString::fromUtf8(script.id.bytes_as_string_view().characters_without_null_termination(),script.id.bytes().size());
                String configuration;
                for(auto const& definition:m_menu_registry.definitions())
                    if(definition.id==script.id)configuration=definition.configuration;
                auto keep_alive=[self=QPointer(this),session=session.make_weak_ptr(),script=script.id,configuration]{
                    if(!self||!session||configuration.is_empty())return false;
                    for(auto const& definition:self->m_menu_registry.definitions())
                        if(definition.id==script&&definition.enabled&&definition.configuration==configuration)return true;
                    return false;
                };
                return notifications.dispatch(page,token,scope,script.name,decoded,move(valid),[done](auto response){(*done)(move(response));},move(keep_alive));
            }();
            if (result.is_error())
                (*done)(result.release_error());
            return {};
        }
        if (decoded.method == Protocol::Method::Download || decoded.method == Protocol::Method::DownloadLegacy
            || decoded.method == Protocol::Method::DownloadAbort || decoded.method == Protocol::Method::DownloadWatch
            || decoded.method == Protocol::Method::DownloadChunk || decoded.method == Protocol::Method::DownloadFinish) {
            auto result = [&]() -> ErrorOr<void> {
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                TRY(entry->authority.download_permission(context, token, decoded.method));
                auto valid = [weak = std::weak_ptr<Entry>(entry), client = page.client().make_weak_ptr<WebView::WebContentClient>(),
                                 document = document.make_weak_ptr(), context, token] {
                    auto owner = weak.lock();
                    if (!owner || !client || !document) return false;
                    auto* page = client->page(context.page);
                    return page && page->is_open() && active_document(*page, *document).has_value()
                        && !owner->authority.download_permission(context, token, Protocol::Method::DownloadWatch).is_error();
                };
                return downloads.dispatch(page, token, decoded, move(valid), [done](auto result) { (*done)(move(result)); });
            }();
            if (result.is_error()) (*done)(result.release_error());
            return {};
        }
        if (decoded.method == Protocol::Method::TabGet || decoded.method == Protocol::Method::TabGetLegacy
            || decoded.method == Protocol::Method::TabSave || decoded.method == Protocol::Method::TabSaveLegacy
            || decoded.method == Protocol::Method::TabsGet || decoded.method == Protocol::Method::TabsGetLegacy) {
            auto result = [&]() -> ErrorOr<ByteBuffer> {
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                auto script = TRY(entry->authority.tab_data_permission(context, token, decoded.method));
                return entry->tab_data.dispatch(page.view(), script, decoded);
            }();
            (*done)(move(result));
            return {};
        }
        if (decoded.method == Protocol::Method::MenuRegister || decoded.method == Protocol::Method::MenuRegisterLegacy
            || decoded.method == Protocol::Method::MenuRemove || decoded.method == Protocol::Method::MenuRemoveLegacy || decoded.method == Protocol::Method::MenuWait) {
            auto result = [&]() -> ErrorOr<void> {
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                auto script = TRY(entry->authority.menu_permission(context, token, decoded.method));
                auto valid = [weak = std::weak_ptr<Entry>(entry), client = page.client().make_weak_ptr<WebView::WebContentClient>(),
                                 document = document.make_weak_ptr(), context, token] {
                    auto owner = weak.lock();
                    if (!owner || !client || !document) return false;
                    auto* page = client->page(context.page);
                    return page && page->is_open() && active_document(*page, *document).has_value()
                        && !owner->authority.menu_permission(context, token, Protocol::Method::MenuWait).is_error();
                };
                return commands.dispatch(page, token, move(script.id), move(script.name), decoded, move(valid), [done](auto result) { (*done)(move(result)); }, context.document);
            }();
            if (result.is_error()) (*done)(result.release_error());
            return {};
        }
        if (decoded.method == Protocol::Method::TabOpen || decoded.method == Protocol::Method::TabOpenLegacy
            || decoded.method == Protocol::Method::TabClose || decoded.method == Protocol::Method::TabWatch) {
            auto result = [&]() -> ErrorOr<void> {
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                TRY(entry->authority.tab_permission(context, token, decoded.method));
                auto valid = [weak = std::weak_ptr<Entry>(entry), client = page.client().make_weak_ptr<WebView::WebContentClient>(),
                                 document = document.make_weak_ptr(), context, token] {
                    auto owner = weak.lock();
                    if (!owner || !client || !document) return false;
                    auto* page = client->page(context.page);
                    return page && page->is_open() && active_document(*page, *document).has_value()
                        && !owner->authority.tab_permission(context, token, Protocol::Method::TabWatch).is_error();
                };
                return tabs.dispatch(page, token, decoded, move(valid), [done](auto result) { (*done)(move(result)); });
            }();
            if (result.is_error()) (*done)(result.release_error());
            return {};
        }
        if (decoded.method == Protocol::Method::Clipboard || decoded.method == Protocol::Method::ClipboardLegacy) {
            auto result = [&]() -> ErrorOr<void> {
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                TRY(entry->authority.clipboard_permission(context, token, decoded.method));
                if (clipboard_write_async) {
                    clipboard_write_async(page, decoded.key, TRY(ByteBuffer::copy(decoded.updates[0].value->bytes())), [done](ErrorOr<void> result) {
                        if (result.is_error()) (*done)(result.release_error());
                        else (*done)(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
                    });
                    return {};
                }
                if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
                    return Error::from_string_literal("Clipboard requires a graphical application");
                auto* clipboard = QGuiApplication::clipboard();
                if (!clipboard) return Error::from_string_literal("System clipboard is unavailable");
                auto bytes = decoded.updates[0].value->bytes();
                auto text = QString::fromUtf8(reinterpret_cast<char const*>(bytes.data()), bytes.size());
                auto* mime = new QMimeData;
                if (decoded.key == "text/html"sv) mime->setHtml(text);
                else mime->setText(text);
                clipboard->setMimeData(mime);
                return {};
            }();
            if (result.is_error()) (*done)(result.release_error());
            else if (!clipboard_write_async) (*done)(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
            return {};
        }
        if (decoded.method == Protocol::Method::Network || decoded.method == Protocol::Method::NetworkLegacy || decoded.method == Protocol::Method::NetworkAbort || decoded.method == Protocol::Method::NetworkRead) {
            auto dispatch_network = [&]() -> ErrorOr<void> {
                auto identity = decoded.identity.bytes_as_string_view();
                auto token = QString::fromUtf8(identity.characters_without_null_termination(), identity.length());
                auto policy = TRY(entry->authority.network_policy(context, token, decoded.method));
                auto id = token + ":" + QString::fromUtf8(decoded.key.bytes_as_string_view().characters_without_null_termination(), decoded.key.bytes().size());
                if (decoded.method == Protocol::Method::NetworkRead) {
                    auto task = m_network.value(id);
                    if (!task) return Error::from_string_literal("Unknown userscript response stream");
                    return task->read([done](auto response) { (*done)(move(response)); });
                }
                if (decoded.method == Protocol::Method::NetworkAbort) {
                    if (auto task = m_network.value(id)) task->cancel();
                    (*done)(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
                    return {};
                }
                if (m_network.size() >= 32 || m_network.contains(id))
                    return Error::from_string_literal("Too many or duplicate userscript network requests");
                auto valid = [weak = std::weak_ptr<Entry>(entry), client = page.client().make_weak_ptr<WebView::WebContentClient>(),
                                 document = document.make_weak_ptr(), context, token, method = decoded.method] {
                    auto owner = weak.lock();
                    if (!owner || !client || !document) return false;
                    auto* page = client->page(context.page);
                    return page && page->is_open() && active_document(*page, *document).has_value()
                        && !owner->authority.network_policy(context, token, method).is_error();
                };
                auto* task = new DerpBirdNetworkRequest(this, move(policy), document.creation_url(), session.is_private(), [valid] { return valid(); },
                    [done, valid](ErrorOr<ByteBuffer> response) mutable {
                        if (valid()) (*done)(move(response));
                    });
                m_network.insert(id, task);
                connect(task, &QObject::destroyed, this, [this, id] { m_network.remove(id); });
                auto result = task->start(decoded.updates[0].value->bytes());
                if (result.is_error()) {
                    m_network.remove(id);
                    delete task;
                    return result.release_error();
                }
                return {};
            }();
            if (dispatch_network.is_error()) (*done)(dispatch_network.release_error());
            return {};
        }
        auto result = DerpBirdValueDispatcher::dispatch(entry->authority, context, document.creation_url(), *top_level, payload,
            [done, weak_client = page.client().make_weak_ptr<WebView::WebContentClient>(), page_id = page.id(), weak_document = document.make_weak_ptr()](auto response) mutable {
                if (!weak_client || !weak_document) return;
                auto* page = weak_client->page(page_id);
                if (!page || !page->is_open() || !active_document(*page, *weak_document).has_value()) return;
                (*done)(move(response));
            }, {}, [weak = std::weak_ptr<Entry>(entry), client = page.client().make_weak_ptr<WebView::WebContentClient>(),
                       weak_document = document.make_weak_ptr(), context](u64 subscription, size_t bytes) -> ErrorOr<DerpBirdValueDispatcher::EventSink> {
                if (!client || !weak_document) return Error::from_string_literal("Userscript event document closed");
                auto* page = client->page(context.page);
                if (!page || !active_document(*page, *weak_document).has_value())
                    return Error::from_string_literal("Userscript event document inactive");
                auto reservation = TRY(client->reserve_derpbird_value_event(context.page, context.document, bytes));
                return DerpBirdValueDispatcher::EventSink { [weak, context, subscription, reservation = move(reservation)](ByteBuffer packet) {
                    auto cancel = [weak, context, subscription] {
                        if (auto owner = weak.lock()) owner->authority.cancel_subscription(context, subscription);
                    };
                    auto sent = reservation->enqueue(move(packet), [cancel](auto result) {
                        if (result.is_error()) cancel();
                    });
                    if (sent.is_error()) cancel();
                } };
            });
        if (result.is_error()) (*done)(result.release_error());
        return {};
    }
    void close_session(quintptr key)
    {
        auto entry = m_entries.take(key);
        if (!entry) return;
        MUST(entry->authority.replace_registry(JsonValue { JsonArray {} }));
        auto serial = entry->serial;
        notifications.close_session(QString::number(serial)+":");
        m_closing.insert(serial, entry);
        MUST(entry->service.close([weak = QPointer { this }, serial] {
            if (!weak) return;
            weak->m_closing.remove(serial);
            weak->finish_shutdown();
        }));
    }
    void finish_shutdown()
    {
        if (m_stopping && m_entries.isEmpty() && m_closing.isEmpty() && !m_shutdown.is_empty()) {
            auto completions = move(m_shutdown);
            for (auto& completion : completions) completion();
        }
    }
    QHash<QString, QPointer<DerpBirdNetworkRequest>> m_network;
    WebView::Application& m_application;
    JsonValue m_registry { JsonArray {} };
    QHash<quintptr, std::shared_ptr<Entry>> m_entries;
    QHash<u64, std::shared_ptr<Entry>> m_closing;
    u64 m_next_serial { 1 };
    bool m_stopping { false };
    Vector<Function<void()>> m_shutdown;
};

}
