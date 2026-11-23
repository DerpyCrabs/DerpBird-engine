#pragma once

#include <AK/JsonObject.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WebContentView.h>
#include <QBuffer>
#include <QPainter>
#include <QPointer>
#include <QRegion>
#include <memory>

namespace Ladybird {

// Qt owns geometry and stacking, while the compositor owns page pixels (including GPU-only
// surfaces). Capture both without asking the desktop compositor to expose or activate a window.
class DerpBirdWindowScreenshot : public std::enable_shared_from_this<DerpBirdWindowScreenshot> {
public:
    using Completion = Function<void(ErrorOr<JsonValue>)>;
    static void capture(QWidget& window, Completion completion)
    {
        auto capture = std::shared_ptr<DerpBirdWindowScreenshot>(new DerpBirdWindowScreenshot(window, AK::move(completion)));
        auto size = window.size() * window.devicePixelRatioF();
        if (size.width() <= 0 || size.height() <= 0 || static_cast<qint64>(size.width()) * size.height() > 64 * 1024 * 1024) {
            capture->fail();
            return;
        }
        capture->m_image = QImage(size, QImage::Format_ARGB32_Premultiplied);
        capture->m_image.setDevicePixelRatio(window.devicePixelRatioF());
        capture->m_image.fill(Qt::transparent);
        if (capture->m_image.isNull()) { capture->fail(); return; }
        capture->m_surfaces.append({ &window, {}, window.rect(), QRegion(window.rect()) });
        capture->collect(window, QRegion(window.rect()));
        capture->next();
    }
private:
    struct Surface { QPointer<QWidget> widget; QPointer<WebContentView> view; QRect rect; QRegion clip; };
    DerpBirdWindowScreenshot(QWidget& window, Completion completion)
        : m_window(&window), m_size(window.size()), m_completion(AK::move(completion)) { }
    void collect(QWidget& parent, QRegion clip)
    {
        // QObject's child order follows QWidget raise/lower order.
        for (auto* object : parent.children()) {
            auto* widget = qobject_cast<QWidget*>(object);
            if (!widget || widget->isHidden() || widget->isWindow()) continue;
            auto position = widget->mapTo(m_window, QPoint());
            auto region = QRegion(QRect(position, widget->size()));
            if (!widget->mask().isEmpty()) region &= widget->mask().translated(position);
            region &= clip;
            if (region.isEmpty()) continue;
            m_surfaces.append({ widget, qobject_cast<WebContentView*>(widget), QRect(position, widget->size()), region });
            collect(*widget, region);
        }
    }
    void fail()
    {
        if (m_completion) { auto done = AK::move(m_completion); done(Error::from_string_literal("Window changed or a rendered surface was unavailable during capture; retry the screenshot")); }
    }
    void next()
    {
        if (!m_window || m_window->size() != m_size) { fail(); return; }
        if (m_index == m_surfaces.size()) {
            QByteArray bytes;
            QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
            if (!m_image.save(&buffer, "PNG")) { fail(); return; }
            JsonObject result;
            result.set("data"sv, ak_string_from_qstring(QString::fromLatin1(bytes.toBase64())));
            auto done = AK::move(m_completion); done(JsonValue { AK::move(result) });
            return;
        }
        auto surface = m_surfaces[m_index++];
        if (!surface.widget) { fail(); return; }
        if (!surface.view) {
            {
                QPainter painter(&m_image);
                painter.setClipRegion(surface.clip);
                surface.widget->render(&painter, surface.rect.topLeft(), {}, QWidget::DrawWindowBackground);
            }
            next();
            return;
        }
        surface.view->take_frame_snapshot([self = shared_from_this(), surface](Optional<Gfx::ShareableBitmap> frame) {
            if (!self->m_window || !surface.view || !surface.view->isVisibleTo(self->m_window)
                || QRect(surface.view->mapTo(self->m_window, QPoint()), surface.view->size()) != surface.rect
                || !frame.has_value() || !frame->is_valid() || !frame->bitmap()) { self->fail(); return; }
            auto const& bitmap = *frame->bitmap();
            QImage image(bitmap.scanline_u8(0), bitmap.width(), bitmap.height(), bitmap.pitch(), QImage::Format_ARGB32_Premultiplied);
            {
                QPainter painter(&self->m_image);
                painter.setClipRegion(surface.clip);
                painter.drawImage(surface.rect, image);
            }
            self->next();
        });
    }
    QPointer<QWidget> m_window;
    QSize m_size;
    QImage m_image;
    QList<Surface> m_surfaces;
    qsizetype m_index { 0 };
    Completion m_completion;
};
}
