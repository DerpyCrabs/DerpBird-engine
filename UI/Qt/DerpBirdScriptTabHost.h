#pragma once

#include "DerpBirdScriptTabs.h"
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/WebContentView.h>
#include <QApplication>

namespace Ladybird {
inline ErrorOr<void> perform_derpbird_script_window_action(WebView::WebContentPage& source, bool close)
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* window = as_if<BrowserWindow>(widget);
        if (!window) continue;
        for (int index = 0; index < window->tab_count(); ++index) {
            auto* tab = window->tab_at(index);
            if (!tab->is_loaded() || &tab->view().page().traversable() != &source.traversable()) continue;
            if (!close) {
                window->activate_tab(index);
                window->show();
                window->raise();
                window->activateWindow();
                return {};
            }
            if (window->tab_count() <= 1)
                return Error::from_string_literal("Userscripts cannot close the last tab of a window");
            // Acknowledge the capability request before tearing down its sender.
            QTimer::singleShot(0, window, [window = QPointer(window), tab = QPointer(tab)] {
                if (!window || !tab || window->tab_count() <= 1) return;
                auto current = window->tab_index(tab);
                if (current >= 0) window->request_to_close_tab(current);
            });
            return {};
        }
    }
    return Error::from_string_literal("Userscript source tab is no longer open");
}

inline ErrorOr<DerpBirdScriptTabs::Handle> open_derpbird_script_tab(WebView::WebContentPage& source, URL::URL const& url, JsonObject const& options)
{
    BrowserWindow* origin_window = nullptr;
    Tab* origin_tab = nullptr;
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* window = as_if<BrowserWindow>(widget);
        if (!window) continue;
        for (int i = 0; i < window->tab_count(); ++i) {
            auto* tab = window->tab_at(i);
            if (tab->is_loaded() && &tab->view().page().traversable() == &source.traversable()) {
                origin_window = window;
                origin_tab = tab;
                break;
            }
        }
        if (origin_tab) break;
    }
    if (!origin_tab) return Error::from_string_literal("Userscript source tab is no longer open");
    bool active = options.get_bool("active"sv).value_or(false);
    auto* window = origin_window;
    Tab* created = nullptr;
    if (options.get_bool("incognito"sv).value_or(false) && window->is_private() == WebView::IsPrivate::No) {
        window = &Application::the().new_window({ url }, {}, BrowserWindow::IsPopupWindow::No, WebView::IsPrivate::Yes);
        created = window->current_tab();
        if (!active) { origin_window->activateWindow(); origin_window->raise(); }
    } else {
        auto location = BrowserWindow::TabLocation::end();
        if (options.get_bool("insert"sv).value_or(false)) location = BrowserWindow::TabLocation::after_tab(*origin_tab);
        else if (auto index = options.get_integer<int>("insert"sv); index.has_value())
            location = BrowserWindow::TabLocation::at_index(min(*index, window->tab_count()));
        created = &window->new_tab_from_url(url, active ? Web::HTML::ActivateTab::Yes : Web::HTML::ActivateTab::No, location);
        if (options.get_bool("setParent"sv).value_or(false)) created->set_derpbird_script_parent(origin_tab);
    }
    if (!created) return Error::from_string_literal("Unable to open userscript tab");
    return DerpBirdScriptTabs::Handle { QPointer<QObject> { created }, [window = QPointer(window), tab = QPointer(created)] {
        if (!window || !tab) return;
        auto index = window->tab_index(tab);
        if (index >= 0) window->request_to_close_tab(index);
    } };
}
}
