#pragma once

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace Ladybird {

// UI-thread only. The ticket expires on any clipboard change, so an async
// keyring read cannot overwrite a newer user copy. No foreign clipboard content
// is read to determine ownership or expiry.
class DerpBirdSecretClipboard final : public QObject {
public:
    using Ticket = std::weak_ptr<int>;
    explicit DerpBirdSecretClipboard(QObject* parent = nullptr, int lifetime_ms = 30000)
        : QObject(parent), m_clipboard(QGuiApplication::clipboard()), m_lifetime_ms(lifetime_ms)
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { clear(); });
        connect(m_clipboard, &QClipboard::changed, this, [this](QClipboard::Mode mode) {
            if (mode != QClipboard::Clipboard) return;
            m_epoch = std::make_shared<int>(0);
            if (!owns_item()) { m_timer.stop(); m_item = nullptr; }
        });
    }
    ~DerpBirdSecretClipboard() override { clear(); }
    Ticket ticket() const { return m_epoch; }
    bool publish(QString text, Ticket const& ticket)
    {
        if (ticket.lock() != m_epoch) return false;
        auto* item = new QMimeData;
        item->setText(text);
        // Advisory hint for clipboard managers that honor it, not a guarantee
        // that another application cannot retain a copied password.
        item->setData("application/x-kde-passwordManagerHint", "secret");
        m_item = item;
        m_clipboard->setMimeData(item, QClipboard::Clipboard);
        if (!owns_item()) { m_item = nullptr; return false; }
        m_timer.start(m_lifetime_ms);
        return true;
    }
    void clear()
    {
        m_timer.stop();
        bool owned = owns_item();
        m_item = nullptr;
        if (owned) m_clipboard->clear(QClipboard::Clipboard);
    }
private:
    bool owns_item() const
    {
        return m_item && m_clipboard->ownsClipboard() && m_clipboard->mimeData(QClipboard::Clipboard) == m_item;
    }
    QClipboard* m_clipboard;
    QPointer<QMimeData> m_item;
    QTimer m_timer;
    int m_lifetime_ms;
    std::shared_ptr<int> m_epoch { std::make_shared<int>(0) };
};

}
