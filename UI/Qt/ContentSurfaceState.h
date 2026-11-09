#pragma once

#include <QRect>
#include <QRegion>
#include <QVector>
#include <utility>

namespace Ladybird {

// Geometry belongs to one loaded shell document. It becomes unusable when that
// document navigates or crashes, even if the native window later resizes.
class ContentSurfaceState {
public:
    struct Placement {
        QRect viewport;
        QRegion visible_region;
        bool visible() const { return !viewport.isEmpty() && !visible_region.isEmpty(); }
    };

    void invalidate()
    {
        m_viewport = {};
        m_overlays.clear();
    }

    void accept(QRect viewport, QVector<QRect> overlays)
    {
        m_viewport = viewport;
        m_overlays = std::move(overlays);
    }

    Placement placement(QRect bounds) const
    {
        auto viewport = m_viewport.intersected(bounds);
        QRegion visible(QRect(QPoint(0, 0), viewport.size()));
        for (auto const& overlay : m_overlays)
            visible -= overlay.translated(-viewport.topLeft());
        return { viewport, visible };
    }

private:
    QRect m_viewport;
    QVector<QRect> m_overlays;
};

}
