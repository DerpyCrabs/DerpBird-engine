#pragma once

#include <AK/Function.h>
#include <AK/JsonValue.h>
#include <LibURL/Parser.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebView/WebContentPage.h>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace Ladybird {

// Handles belong to the requesting script capability, never a page-supplied tab ID.
// A close notification is a bounded long-poll, not a repeating JavaScript timer.
class DerpBirdScriptTabs final : public QObject {
public:
    struct Handle {
        QPointer<QObject> lifetime;
        Function<void()> close;
    };
    using Open = Function<ErrorOr<Handle>(WebView::WebContentPage&, URL::URL const&, JsonObject const&)>;
    using Completion = Function<void(ErrorOr<ByteBuffer>)>;
    Open on_open;
    explicit DerpBirdScriptTabs(QObject* parent) : QObject(parent) { }
    ErrorOr<void> dispatch(WebView::WebContentPage& page, QString const& token,
        Web::DerpBirdValueProtocol::Request const& request, Function<bool()> valid, Completion completion)
    {
        using Protocol = Web::DerpBirdValueProtocol;
        auto key = token + ":" + QString::fromUtf8(request.key.bytes_as_string_view().characters_without_null_termination(), request.key.bytes().size());
        // Release revoked handles without closing their independent browser tabs.
        m_handles.removeIf([](auto const& item) { return !item.value()->valid(); });
        if (request.method == Protocol::Method::TabOpen || request.method == Protocol::Method::TabOpenLegacy) {
            if (!on_open) return Error::from_string_literal("Tab host is unavailable");
            if (m_handles.contains(key) || m_handles.size() >= 256) return Error::from_string_literal("Too many or duplicate userscript tabs");
            size_t count = 0;
            for (auto const& item : m_handles) if (item->token == token) ++count;
            if (count >= 32) return Error::from_string_literal("Userscript tab limit reached");
            auto json = TRY(JsonValue::from_string(request.updates[0].value->bytes()));
            if (!json.is_object()) return Error::from_string_literal("Invalid tab options");
            auto const& options = json.as_object();
            auto text = options.get_string("url"sv);
            auto url = text.has_value() ? URL::Parser::basic_parse(*text) : Optional<URL::URL> {};
            if (!url.has_value() || !(url->scheme() == "http"sv || url->scheme() == "https"sv || url->serialize() == "about:blank"sv))
                return Error::from_string_literal("Unsupported userscript tab URL");
            for (auto flag : { "active"sv, "setParent"sv, "incognito"sv })
                if (options.has(flag) && !options.get_bool(flag).has_value()) return Error::from_string_literal("Invalid tab option");
            if (auto insert = options.get("insert"sv); insert.has_value() && !insert->is_bool()
                && (!options.get_integer<int>("insert"sv).has_value() || *options.get_integer<int>("insert"sv) < 0))
                return Error::from_string_literal("Invalid tab insertion index");
            auto handle = TRY(on_open(page, *url, options));
            m_handles.insert(key, std::make_shared<Entry>(token, move(handle), move(valid)));
            completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
            return {};
        }
        auto entry = m_handles.value(key);
        if (request.method == Protocol::Method::TabClose) {
            if (entry && entry->handle.lifetime) entry->handle.close();
            completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
            return {};
        }
        if (!entry || !entry->handle.lifetime) {
            m_handles.remove(key);
            completion(reply(true));
            return {};
        }
        if (entry->watching) return Error::from_string_literal("Tab already has a close observer");
        entry->watching = true;
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        auto done = std::make_shared<Completion>(move(completion));
        auto finish = [this, timer, key, entry, done] {
            if (!*done) return;
            timer->stop();
            timer->deleteLater();
            entry->watching = false;
            auto callback = move(*done);
            bool closed = !entry->handle.lifetime;
            if (closed) m_handles.remove(key);
            if (entry->valid()) callback(reply(closed));
        };
        connect(timer, &QTimer::timeout, timer, finish);
        connect(entry->handle.lifetime, &QObject::destroyed, timer, finish);
        timer->start(20000); // Below the transport's 30-second request deadline.
        return {};
    }
private:
    static ErrorOr<ByteBuffer> reply(bool closed)
    {
        using Protocol = Web::DerpBirdValueProtocol;
        return Protocol::encode(Protocol::Response { Protocol::Kind::Network, {},
            TRY(ByteBuffer::copy((closed ? "closed"sv : "open"sv).bytes())), {}, 0 });
    }
    struct Entry {
        Entry(QString token, Handle handle, Function<bool()> valid) : token(move(token)), handle(move(handle)), valid(move(valid)) { }
        QString token;
        Handle handle;
        Function<bool()> valid;
        bool watching { false };
    };
    QHash<QString, std::shared_ptr<Entry>> m_handles;
};

}
