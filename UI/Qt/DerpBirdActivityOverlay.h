#pragma once

#include <UI/Qt/Theme.h>
#include <UI/Qt/WebContentView.h>
#include <QElapsedTimer>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPicture>
#include <QVariant>
#include <QTimer>
#include <algorithm>

namespace Ladybird {

// Browser-owned, pointer-transparent sibling of the page surface. No page DOM,
// system cursor movement, focus changes, input text or script contents are used.
class DerpBirdActivityOverlay final : public QWidget {
public:
    explicit DerpBirdActivityOverlay(WebContentView& view)
        : QWidget(view.parentWidget()), m_view(&view)
    {
        setObjectName("derpbird-activity-overlay");
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        view.add_floating_surface(*this);
        setFocusPolicy(Qt::NoFocus);
        view.installEventFilter(this);
        m_clock.start();
        connect(&m_timer, &QTimer::timeout, this, [this] {
            auto age = m_clock.elapsed() - m_last_activity;
            if (!m_view || !m_view->isVisible() || age > 1400) { hide(); m_timer.stop(); return; }
            if (m_kind == "evaluate" && m_clock.elapsed() - m_started < 300) return;
            sync_geometry(); record_picture(); show(); raise(); update();
        });
        hide();
    }
    void activity(QString kind, Optional<QPointF> point, Optional<QPointF> from, Optional<QRectF> field)
    {
        auto now = m_clock.elapsed();
        if (kind == "keepalive") { m_last_activity = now; return; }
        if (kind == "end") {
            if (m_kind == "evaluate") {
                if (now - m_started < 300) { hide(); m_timer.stop(); return; }
                m_last_activity = now - 1050;
            }
            return;
        }
        m_last_activity = now;
        m_started = now;
        m_kind = kind;
        m_field = field;
        m_has_point = point.has_value();
        if (point.has_value()) {
            m_from = from.value_or(m_point);
            if (!isVisible() && kind != "drag") m_from = *point;
            m_point = *point;
        }
        if (!m_view || !m_view->isVisible()) return;
        sync_geometry(); m_timer.start(16);
        if (kind == "evaluate") { hide(); return; }
        record_picture(); show(); raise(); update();
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == m_view && (event->type() == QEvent::Hide || event->type() == QEvent::Destroy)) {
            hide(); m_timer.stop();
        }
        return QWidget::eventFilter(object, event);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this); painter.drawPicture(0, 0, m_picture);
    }
    void draw_activity(QPainter& p)
    {
        if (!m_view) return;
        auto now = m_clock.elapsed();
        auto elapsed = now - m_started;
        if (m_kind == "evaluate" && elapsed < 300) return; // Do not flash on internal queries.
        auto fade = std::clamp((1400.0 - (now - m_last_activity)) / 300.0, 0.0, 1.0);
        p.setRenderHint(QPainter::Antialiasing); p.setOpacity(fade);
        auto const& colors = Theme::panel();
        auto accent = Theme::accent();
        auto zoom = m_view->zoom_level();
        if (m_field.has_value() && m_kind == "type") {
            auto r = *m_field;
            r = QRectF(r.topLeft() * zoom, r.size() * zoom).adjusted(-2, -2, 2, 2);
            p.setPen(QPen(Theme::focus_edge(), 1.5)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(r, 4, 4);
        }
        QString label = m_kind == "type" ? "AI · Input" : m_kind == "drag" ? "AI · Dragging" : m_kind == "scroll" ? "AI · Scrolling" : m_kind == "evaluate" ? "AI · Evaluating" : m_kind == "click" ? "AI · Click" : "AI";
        auto font = p.font(); font.setPixelSize(11); p.setFont(font);
        auto text_width = p.fontMetrics().horizontalAdvance(label);
        QPointF label_position(width() - text_width - 30, 14);
        if (m_has_point) {
            auto duration = m_kind == "drag" ? 300.0 : 130.0;
            auto progress = std::clamp(elapsed / duration, 0.0, 1.0);
            progress = progress * progress * (3 - 2 * progress);
            auto position = (m_from + (m_point - m_from) * progress) * zoom;
            if (m_kind == "click" && elapsed < 650) {
                p.save();p.setOpacity(fade * (1.0 - elapsed / 650.0));p.setPen(QPen(accent, 2));p.setBrush(Qt::NoBrush);
                auto radius = 7.0 + elapsed / 35.0; p.drawEllipse(m_point * zoom, radius, radius);p.restore();
            }
            // Conventional arrow silhouette, with contrasting outline on any page.
            p.save(); p.translate(position); p.scale(1.25, 1.25);
            QPainterPath arrow; arrow.moveTo(0, 0); arrow.lineTo(0, 21); arrow.lineTo(5, 16); arrow.lineTo(9, 25); arrow.lineTo(13, 23); arrow.lineTo(9, 14); arrow.lineTo(17, 14); arrow.closeSubpath();
            p.setPen(QPen(QColor("#ffffff"), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));p.setBrush(accent);p.drawPath(arrow);
            p.setPen(QPen(QColor("#202124"), 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));p.drawPath(arrow);p.restore();
            label_position = position + QPointF(26, 33);
        }
        label_position.setX(std::clamp(label_position.x(), 4.0, std::max(4.0, width() - text_width - 20.0)));
        label_position.setY(std::clamp(label_position.y(), 4.0, std::max(4.0, height() - 28.0)));
        QRectF badge(label_position, QSizeF(text_width + 16, 24));
        p.setPen(QPen(colors.edge, 1));p.setBrush(colors.surface);p.drawRoundedRect(badge, 4, 4);
        p.setPen(colors.text);p.drawText(badge, Qt::AlignCenter, label);
    }
private:
    void record_picture()
    {
        m_picture = QPicture {};
        QPainter painter(&m_picture); draw_activity(painter); painter.end();
        setProperty("derpbirdOverlayBounds", m_picture.boundingRect().adjusted(-3, -3, 3, 3));
    }
    QPicture m_picture;
    void sync_geometry() { if (m_view) setGeometry(QRect(m_view->mapTo(parentWidget(), QPoint()), m_view->size())); }
    QPointer<WebContentView> m_view;
    QElapsedTimer m_clock;
    QTimer m_timer;
    qint64 m_started { 0 }, m_last_activity { 0 };
    QString m_kind;
    QPointF m_from, m_point;
    bool m_has_point { false };
    Optional<QRectF> m_field;
};
}
