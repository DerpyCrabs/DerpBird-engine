#pragma once

#include <AK/JsonArray.h>
#include <AK/HashTable.h>
#include <QPointer>
#include <AK/JsonObject.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebContentClient.h>
#include <QHash>
#include <QTimer>
#include <QUuid>
#include <memory>

namespace Ladybird {
class DerpBirdScriptCommands final : public QObject {
public:
    using Protocol = Web::DerpBirdValueProtocol;
    using Completion = Function<void(ErrorOr<ByteBuffer>)>;
    Function<void()> on_change;
    explicit DerpBirdScriptCommands(QObject* parent) : QObject(parent) { }
    void prune()
    {
        auto count = m_owners.size();
        m_owners.removeIf([](auto const& item) { return !item.value()->valid(); });
        if (count != m_owners.size()) notify_change();
    }
    ErrorOr<void> dispatch(WebView::WebContentPage& page, QString const& token, String script_id, String script_name,
        Protocol::Request const& request, Function<bool()> valid, Completion completion, Web::HTML::CrossProcessId document = {})
    {
        prune();
        auto owner = m_owners.value(token);
        if (!owner) {
            if (m_owners.size() >= 256) return Error::from_string_literal("Too many userscript command owners");
            owner = std::make_shared<Owner>(page, move(script_id), move(script_name), move(valid));
            owner->document = document;
            m_owners.insert(token, owner);
        }
        if (request.method == Protocol::Method::MenuWait) {
            if (owner->pending) return Error::from_string_literal("Userscript command observer already exists");
            owner->pending = move(completion);
            if (!owner->events.is_empty() || owner->commands.isEmpty()) { flush(*owner); return {}; }
            auto* timer = new QTimer(this);
            timer->setSingleShot(true);
            owner->timer = timer;
            connect(timer, &QTimer::timeout, timer, [weak = std::weak_ptr(owner), timer] {
                timer->deleteLater();
                if (auto owner = weak.lock()) { owner->timer = nullptr; flush(*owner); }
            });
            timer->start(20000);
            return {};
        }
        auto key = QString::fromUtf8(request.key.bytes_as_string_view().characters_without_null_termination(), request.key.bytes().size());
        if (request.method == Protocol::Method::MenuRemove || request.method == Protocol::Method::MenuRemoveLegacy) {
            owner->commands.remove(key);
        } else {
            auto json = TRY(JsonValue::from_string(request.updates[0].value->bytes()));
            if (!json.is_object()) return Error::from_string_literal("Invalid userscript command");
            auto data = json.as_object();
            auto name = data.get_string("name"sv);
            auto title = data.get_string("title"sv);
            auto access_key = data.get_string("accessKey"sv);
            auto auto_close = data.get_bool("autoClose"sv);
            if (data.has("clickedFrame"sv) && !data.get_bool("clickedFrame"sv).has_value()) return Error::from_string_literal("Invalid clickedFrame option");
            if (!name.has_value() || name->is_empty() || name->bytes().size()>512 || !title.has_value() || title->bytes().size()>1024
                || !access_key.has_value() || access_key->bytes().size()>16 || !auto_close.has_value())
                return Error::from_string_literal("Invalid userscript command options");
            size_t total = 0;
            for (auto const& current : m_owners) total += current->commands.size();
            if (!owner->commands.contains(key) && (owner->commands.size()>=64 || total>=256)) return Error::from_string_literal("Too many userscript menu commands");
            auto public_id = owner->commands.contains(key) ? owner->commands.value(key).public_id : QUuid::createUuid().toString(QUuid::WithoutBraces);
            owner->commands.insert(key, Command { public_id, request.key, *name, *title, *access_key, *auto_close, data.get_bool("clickedFrame"sv).value_or(false) });
        }
        notify_change();
        // Reply callbacks can close the host. Retain only the command owner
        // across them, and never access this afterwards.
        if (owner->commands.isEmpty() && owner->pending) flush(*owner);
        completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
        return {};
    }
    JsonArray list(WebView::WebContentPage const& page, Optional<Web::HTML::CrossProcessId> clicked_document = {})
    {
        prune();
        JsonArray result;
        HashTable<String> groups;
        for (auto const& owner : m_owners) {
            if (!same_page(*owner, page)) continue;
            for (auto const& item : owner->commands) {
                if (item.clicked_frame && owner->document != clicked_document.value_or(page.traversable().active_document().id())) continue;
                auto group = group_key(*owner, item);
                if (groups.contains(group)) continue;
                groups.set(move(group));
                JsonObject record;
                auto utf8 = item.public_id.toUtf8();
                record.set("id"sv, MUST(String::from_utf8(StringView { utf8.constData(), static_cast<size_t>(utf8.size()) })));
                record.set("script"sv, owner->name); record.set("label"sv, item.name); record.set("title"sv, item.title);
                record.set("accessKey"sv, item.access_key); record.set("autoClose"sv, item.auto_close);
                result.must_append(move(record));
            }
        }
        return result;
    }
    ErrorOr<void> invoke(WebView::WebContentPage const& page, String const& id, JsonObject const& event, Optional<Web::HTML::CrossProcessId> clicked_document = {})
    {
        prune();
        Optional<String> selected;
        for (auto const& owner : m_owners) {
            if (!same_page(*owner, page)) continue;
            for (auto const& command : owner->commands) {
                auto bytes = command.public_id.toUtf8();
                if (id.bytes_as_string_view() == StringView { bytes.constData(), static_cast<size_t>(bytes.size()) }
                    && (!command.clicked_frame || owner->document == clicked_document.value_or(page.traversable().active_document().id())))
                    selected = group_key(*owner, command);
            }
        }
        if (!selected.has_value()) return Error::from_string_literal("Userscript command is no longer available");
        for (auto const& owner : m_owners) {
            if (!same_page(*owner, page)) continue;
            size_t additions = 0;
            for (auto const& command : owner->commands)
                if (group_key(*owner, command) == *selected && (!command.clicked_frame || owner->document == clicked_document.value_or(page.traversable().active_document().id()))) ++additions;
            if (owner->events.size() + additions > 16) return Error::from_string_literal("Userscript command queue is full");
        }
        Vector<std::shared_ptr<Owner>> recipients;
        for (auto const& owner : m_owners) {
            if (!same_page(*owner, page)) continue;
            for (auto const& command : owner->commands) {
                if (group_key(*owner, command) != *selected || (command.clicked_frame && owner->document != clicked_document.value_or(page.traversable().active_document().id()))) continue;
                auto notification = event;
                notification.set("id"sv, command.id);
                owner->events.append(move(notification));
            }
            if (owner->pending && !owner->events.is_empty()) recipients.append(owner);
        }
        // Complete enqueueing before delivering any callback: a callback may
        // revoke another owner or destroy the host while a grouped command runs.
        for (auto const& owner : recipients) flush(*owner);
        return {};
    }
private:
    void notify_change()
    {
        if (m_change_queued) return;
        m_change_queued = true;
        QTimer::singleShot(0, this, [this] {
            m_change_queued = false;
            if (on_change) on_change();
        });
    }
    struct Command { QString public_id; String id; String name; String title; String access_key; bool auto_close; bool clicked_frame { false }; };
    struct Owner {
        Owner(WebView::WebContentPage& page, String id, String name, Function<bool()> valid)
            : client(page.client().make_weak_ptr<WebView::WebContentClient>()), page_id(page.id()), id(move(id)), name(move(name)), valid(move(valid)) { }
        ~Owner() { if (timer) timer->deleteLater(); }
        WeakPtr<WebView::WebContentClient> client;
        Web::PageId page_id;
        Web::HTML::CrossProcessId document;
        String id;
        String name;
        Function<bool()> valid;
        QHash<QString, Command> commands;
        Vector<JsonObject> events;
        Completion pending;
        QPointer<QTimer> timer;
    };
    static bool same_page(Owner const& owner, WebView::WebContentPage const& page)
    {
        auto* source = owner.client ? owner.client->page(owner.page_id) : nullptr;
        return source && owner.valid() && &source->traversable() == &page.traversable();
    }
    static String group_key(Owner const& owner, Command const& command)
    {
        JsonArray group;
        for (auto const& value : { owner.id, command.name, command.title, command.access_key }) group.must_append(value);
        group.must_append(command.clicked_frame);
        group.must_append(command.auto_close);
        return group.serialized();
    }
    static void flush(Owner& owner)
    {
        if (owner.timer) { owner.timer->stop(); owner.timer->deleteLater(); owner.timer = nullptr; }
        auto done = move(owner.pending);
        if (!done || !owner.valid()) return;
        JsonArray events;
        for (auto& event : owner.events) events.must_append(move(event));
        owner.events.clear();
        auto json = events.serialized();
        done(Protocol::encode(Protocol::Response { Protocol::Kind::Network, {}, MUST(ByteBuffer::copy(json.bytes())), {}, 0 }));
    }
    QHash<QString, std::shared_ptr<Owner>> m_owners;
    bool m_change_queued { false };
};
}
