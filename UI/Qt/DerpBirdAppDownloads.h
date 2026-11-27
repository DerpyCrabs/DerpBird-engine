#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/RemoteDownloads.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/WebContentView.h>
#include <QEvent>
#include <UI/Qt/Theme.h>

namespace Ladybird {

class DerpBirdAppDownloads final : public QObject, private RemoteDownloadsObserver {
public:
    DerpBirdAppDownloads(BrowserWindow& window, QWidget& host)
        : QObject(&host)
        , m_window(window)
        , m_host(host)
    {
        host.installEventFilter(this);
    }

private:
    void download_added(RemoteDownloads::Download const& file) override
    {
        auto* tab = m_window.current_tab();
        if (!tab || !tab->is_loaded() || file.source_view_id != tab->view().view_id())
            return;
        m_visible.append(file.id);
        if (!m_surface) {
            WebContentViewInitialState initial;
            initial.is_private = m_window.is_private();
            initial.display_id = m_window.display_id();
            initial.maximum_frames_per_second = m_window.refresh_rate();
            m_surface = new WebContentView(&m_host, {}, initial);
            m_surface->setFocusPolicy(Qt::ClickFocus);
            m_surface->set_transparent_background(true);
            m_surface->on_derpbird_request = [this](JsonValue const& data, auto done) {
                auto type = data.as_object().get_string("type"sv);
                if (type == "devtools.bootstrap"sv) {
                    done(JsonValue("app-downloads"_string));
                } else if (type == "downloads.ready"sv) {
                    m_ready = true;
                    done(JsonValue {});
                    publish();
                } else if (type == "downloads.dismiss"sv) {
                    auto id = data.as_object().get_string("id"sv);
                    if (id.has_value()) {
                        if (auto number = id->to_number<u64>(); number.has_value())
                            dismiss(*number);
                    }
                    done(JsonValue {});
                } else {
                    done(Error::from_string_literal("Unknown download notification command"));
                }
            };
            m_surface->load(URL::Parser::basic_parse("about:derpbird"sv).release_value());
        }
        layout();
        publish();
    }

    void download_updated(RemoteDownloads::Download const& file) override
    {
        if (m_visible.contains_slow(file.id))
            publish();
    }

    void download_removed(u64 id, WebView::IsPrivate) override { dismiss(id); }

    void dismiss(u64 id)
    {
        if (!m_visible.remove_first_matching([&](auto value) { return value == id; }))
            return;
        layout();
        publish();
    }

    void publish()
    {
        if (!m_ready)
            return;
        JsonArray rows;
        using Status = WebView::FileDownloader::DownloadStatus;
        for (auto id : m_visible) {
            auto file = RemoteDownloads::the().download(id);
            if (!file.has_value())
                continue;
            JsonObject row;
            row.set("id"sv, String::number(id));
            row.set("name"sv, String::from_utf8_with_replacement_character(file->destination.basename(), String::WithBOMHandling::No));
            row.set("received"sv, file->downloaded_size);
            if (file->total_size.has_value())
                row.set("total"sv, *file->total_size);
            switch (file->status) {
            case Status::InProgress: row.set("status"sv, "Downloading"sv); break;
            case Status::Completed: row.set("status"sv, "Finished"sv); break;
            case Status::Paused: row.set("status"sv, "Paused"sv); break;
            case Status::Failed: row.set("status"sv, "Failed"sv); break;
            case Status::Canceled: row.set("status"sv, "Canceled"sv); break;
            }
            rows.must_append(move(row));
        }
        JsonObject state;
        state.set("rows"sv, move(rows));
        state.set("theme"sv, Theme::is_dark() ? "dark"sv : "light"sv);
        m_surface->send_derpbird_shell_message("derpbird.snapshot"_string, move(state));
    }

    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == &m_host && event->type() == QEvent::Resize)
            layout();
        return false;
    }

    void layout()
    {
        if (!m_surface)
            return;
        if (m_visible.is_empty()) {
            m_surface->hide();
            return;
        }
        int width = qBound(1, m_host.width() - 16, 320);
        int height = qBound(1, static_cast<int>(m_visible.size()) * 76, qMax(1, m_host.height() - 16));
        m_surface->setGeometry(qMax(0, m_host.width() - width - 8), 8, width, height);
        m_surface->show();
        m_surface->raise();
    }

    BrowserWindow& m_window;
    QWidget& m_host;
    WebContentView* m_surface { nullptr };
    Vector<u64> m_visible;
    bool m_ready { false };
};

}
