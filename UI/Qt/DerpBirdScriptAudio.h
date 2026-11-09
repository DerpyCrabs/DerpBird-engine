#pragma once
#include <AK/JsonObject.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebView/ViewImplementation.h>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace Ladybird {
class DerpBirdScriptAudio final : public QObject {
public:
    using Protocol = Web::DerpBirdValueProtocol;
    using Completion = Function<void(ErrorOr<ByteBuffer>)>;
    explicit DerpBirdScriptAudio(QObject* parent) : QObject(parent) { }
    void prune() { m_waiters.removeIf([](auto const& item) { return !item.value()->valid() || !item.value()->view; }); }
    void shutdown() { m_waiters.clear(); }
    void changed(WebView::ViewImplementation& view) {
        auto entries = m_waiters;
        auto lifetime = QPointer(this);
        auto* changed_view = &view;
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if (!lifetime) return;
            if (it.value()->view.ptr() == changed_view) finish(it.key(), it.value());
        }
    }
    ErrorOr<void> dispatch(WebView::ViewImplementation& view, QString token, Protocol::Request const& request, Function<bool()> valid, Completion completion) {
        prune();
        auto raw = TRY(JsonValue::from_string(request.updates[0].value->bytes()));
        if (!raw.is_object()) return Error::from_string_literal("Invalid audio options");
        auto const& options = raw.as_object();
        token += request.method == Protocol::Method::Audio ? ":modern" : ":legacy";
        if (request.key == "set"sv) {
            auto muted = options.get_bool("isMuted"sv);
            if (!muted.has_value()) return Error::from_string_literal("Expected isMuted boolean");
            auto lifetime = QPointer(this);
            auto target = view.make_weak_ptr();
            if (*muted != (view.page_mute_state() == Web::HTML::MuteState::Muted)) view.toggle_page_mute_state(true);
            if (!lifetime || !target) return Error::from_string_literal("Audio source closed during state change");
        }
        if (request.key == "cancel"sv) {
            auto response = reply(view);
            if (auto old = m_waiters.value(token)) finish(token, old, true);
            completion(move(response));
            return {};
        }
        if (request.key != "watch"sv) { completion(reply(view)); return {}; }
        auto previous = options.get_string("previous"sv);
        if (!previous.has_value() || previous->bytes().size() > 512) return Error::from_string_literal("Invalid previous audio state");
        if (*previous != snapshot(view)) { completion(reply(view)); return {}; }
        if (m_waiters.contains(token) || m_waiters.size() >= 256) return Error::from_string_literal("Too many audio observers");
        auto entry = std::make_shared<Waiter>(view, move(valid), move(completion), *previous);
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        entry->timer = timer;
        m_waiters.insert(token, entry);
        connect(timer, &QTimer::timeout, timer, [this, token, weak = std::weak_ptr(entry)] {
            if (auto entry = weak.lock()) finish(token, entry, true);
        });
        timer->start(20000);
        return {};
    }
private:
    struct Waiter {
        Waiter(WebView::ViewImplementation& view, Function<bool()> valid, Completion done, String previous)
            : view(view.make_weak_ptr()), valid(move(valid)), done(move(done)), previous(move(previous)) { }
        ~Waiter() { if (timer) { timer->stop(); timer->deleteLater(); } }
        WeakPtr<WebView::ViewImplementation> view;
        Function<bool()> valid;
        Completion done;
        String previous;
        QPointer<QTimer> timer;
    };
    static String snapshot(WebView::ViewImplementation const& view) {
        JsonObject state;
        bool muted = view.page_mute_state() == Web::HTML::MuteState::Muted;
        state.set("isMuted"sv, muted);
        state.set("isAudible"sv, view.is_playing_audio());
        if (muted) state.set("muteReason"sv, view.derpbird_muted_by_script() ? "extension"sv : "user"sv);
        return state.serialized();
    }
    static ErrorOr<ByteBuffer> reply(WebView::ViewImplementation const& view) {
        auto state = snapshot(view);
        return Protocol::encode(Protocol::Response { Protocol::Kind::Network, {}, TRY(ByteBuffer::copy(state.bytes())), {}, 0 });
    }
    void finish(QString const& key, std::shared_ptr<Waiter> const& entry, bool force = false) {
        if (!entry->done) return;
        if (!entry->valid() || !entry->view) { m_waiters.remove(key); return; }
        if (!force && snapshot(*entry->view) == entry->previous) return;
        auto done = move(entry->done);
        auto response = reply(*entry->view);
        m_waiters.remove(key);
        done(move(response));
    }
    QHash<QString, std::shared_ptr<Waiter>> m_waiters;
};
}
