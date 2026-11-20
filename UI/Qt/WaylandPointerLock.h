#pragma once

#include <AK/Function.h>
#include <UI/Qt/WaylandRegistry.h>
#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformwindow_p.h>
#include <wayland-client.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#include <pointer-constraints-unstable-v1-client-protocol.h>
#include <relative-pointer-unstable-v1-client-protocol.h>
#pragma GCC diagnostic pop

namespace Ladybird {

class WaylandPointerLock {
public:
    bool acquire(QWindow& window)
    {
        auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        auto* native = window.nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        if (!app || !app->pointer() || !native || !native->surface())
            return false;
        auto registry = WaylandRegistry::create();
        auto* constraints = registry->bind<zwp_pointer_constraints_v1>(zwp_pointer_constraints_v1_interface, 1);
        auto* manager = registry->bind<zwp_relative_pointer_manager_v1>(zwp_relative_pointer_manager_v1_interface, 1);
        if (constraints && manager) {
            m_lock = zwp_pointer_constraints_v1_lock_pointer(constraints, native->surface(), app->pointer(), nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
            m_relative = zwp_relative_pointer_manager_v1_get_relative_pointer(manager, app->pointer());
            static constexpr zwp_locked_pointer_v1_listener lock_events {
                [](void* data, zwp_locked_pointer_v1*) { static_cast<WaylandPointerLock*>(data)->m_active = true; },
                [](void* data, zwp_locked_pointer_v1*) {
                    auto& self = *static_cast<WaylandPointerLock*>(data);
                    self.m_active = false;
                    if (self.on_unlock)
                        self.on_unlock();
                }
            };
            static constexpr zwp_relative_pointer_v1_listener motion_events {
                [](void* data, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t x, wl_fixed_t y, wl_fixed_t raw_x, wl_fixed_t raw_y) {
                    auto& self = *static_cast<WaylandPointerLock*>(data);
                    if (self.m_active && self.on_motion)
                        self.on_motion(QPointF(wl_fixed_to_double(self.raw ? raw_x : x), wl_fixed_to_double(self.raw ? raw_y : y)));
                }
            };
            zwp_locked_pointer_v1_add_listener(m_lock, &lock_events, this);
            zwp_relative_pointer_v1_add_listener(m_relative, &motion_events, this);
            registry->roundtrip();
            WaylandRegistry::move_to_qt_queue(m_lock);
            WaylandRegistry::move_to_qt_queue(m_relative);
        }
        if (constraints)
            zwp_pointer_constraints_v1_destroy(constraints);
        if (manager)
            zwp_relative_pointer_manager_v1_destroy(manager);
        return m_active;
    }

    ~WaylandPointerLock()
    {
        if (m_relative)
            zwp_relative_pointer_v1_destroy(m_relative);
        if (m_lock)
            zwp_locked_pointer_v1_destroy(m_lock);
    }

    bool active() const { return m_active; }

    bool raw { false };
    Function<void(QPointF)> on_motion;
    Function<void()> on_unlock;

private:
    bool m_active { false };
    zwp_locked_pointer_v1* m_lock { nullptr };
    zwp_relative_pointer_v1* m_relative { nullptr };
};

}
