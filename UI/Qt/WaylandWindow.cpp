/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WaylandWindow.h>

#include <QGuiApplication>
#include <QWidget>
#include <QWindow>
#ifdef DERPBIRD_HAS_WAYLAND_APP_ID
#    include <qpa/qplatformwindow_p.h>
// Generated listeners cast away const, as in WaylandPageSurface.cpp.
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wcast-qual"
#    include <xdg-shell-client-protocol.h>
#    pragma GCC diagnostic pop
#endif

#include <string.h>
#include <wayland-client.h>

namespace Ladybird {

void set_wayland_agent_window_identity(QWidget& widget)
{
#ifdef DERPBIRD_HAS_WAYLAND_APP_ID
    if (!qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        return;
    widget.winId(); // Create the platform window before show(), without mapping it.
    auto* window = widget.windowHandle();
    auto* native = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!native)
        return;
    auto apply = [native] {
        if (auto* toplevel = native->surfaceRole<xdg_toplevel>())
            xdg_toplevel_set_app_id(toplevel, "org.derpbird.Agent");
    };
    // Qt emits this after its default app_id, before the initial surface commit.
    // Keep the connection for hide/show surface recreation as well.
    QObject::connect(native, &QNativeInterface::Private::QWaylandWindow::surfaceRoleCreated, &widget, apply);
    apply();
#else
    Q_UNUSED(widget);
#endif
}


wl_surface* wayland_surface_for_window(QWindow const& window)
{
    if (!window.handle() || !qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
        return nullptr;

    // Since Qt 6.10, the Wayland backend returns its wl_surface as the window ID.
    return reinterpret_cast<wl_surface*>(window.winId());
}

bool wayland_compositor_moves_dragged_windows()
{
    static bool const moves_dragged_windows = [] {
        auto* wayland_application = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!wayland_application || !wayland_application->display())
            return false;

        // NB: The globals are listed on a queue of our own, so that Qt's queue never sees our registry's events.
        auto* display = wayland_application->display();
        auto* queue = wl_display_create_queue(display);
        auto* display_on_queue = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(display_on_queue), queue);
        auto* registry = wl_display_get_registry(display_on_queue);

        bool has_toplevel_drag_manager = false;
        static wl_registry_listener const listener {
            .global = [](void* data, wl_registry*, uint32_t, char const* interface, uint32_t) {
                if (strcmp(interface, "xdg_toplevel_drag_manager_v1") == 0)
                    *static_cast<bool*>(data) = true; },
            .global_remove = [](void*, wl_registry*, uint32_t) { },
        };
        wl_registry_add_listener(registry, &listener, &has_toplevel_drag_manager);
        wl_display_roundtrip_queue(display, queue);

        wl_registry_destroy(registry);
        wl_proxy_wrapper_destroy(display_on_queue);
        wl_event_queue_destroy(queue);
        return has_toplevel_drag_manager;
    }();
    return moves_dragged_windows;
}

}
