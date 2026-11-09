#pragma once

#include <QPointF>
#include <QRect>
#include <QtMath>

namespace Ladybird {

// Only complete backing-store pixels are guaranteed to be hidden by a native
// surface. At fractional scale the QWidget origin can cut through an edge pixel.
inline QRect fully_covered_surface_pixels(QRect rect, QPointF widget_device_origin)
{
    auto left = qCeil(qCeil(widget_device_origin.x() + rect.x()) - widget_device_origin.x());
    auto top = qCeil(qCeil(widget_device_origin.y() + rect.y()) - widget_device_origin.y());
    auto right = qFloor(qFloor(widget_device_origin.x() + rect.x() + rect.width()) - widget_device_origin.x());
    auto bottom = qFloor(qFloor(widget_device_origin.y() + rect.y() + rect.height()) - widget_device_origin.y());
    return { left, top, qMax(0, right - left), qMax(0, bottom - top) };
}

}
