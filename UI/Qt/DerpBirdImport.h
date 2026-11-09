#pragma once
#include "ChromeImportData.h"
#include "DerpBirdTaskService.h"
#include <QStandardPaths>
#include <QSaveFile>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/HistoryStore.h>
#include <LibWebView/Profile.h>
#include <LibURL/Parser.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>

namespace Ladybird {
inline String import_string(QString const& text) { auto bytes = text.toUtf8(); return MUST(String::from_utf8(StringView(bytes.constData(), bytes.size()))); }
class DerpBirdImport : public QObject {
public:
    using Completion = Function<void(ErrorOr<JsonValue>)>;
    using Worker = DerpBirdTaskService<QJsonObject, QJsonObject>;
    explicit DerpBirdImport(QObject* parent)
        : QObject(parent)
        , m_worker(parse, [](auto const&) -> ErrorOr<size_t> { return 1; }, {}, 1, 300000) { }
    static QString marker()
    {
        return QString::fromUtf8(WebView::Application::profile().paths().data.characters()) + "/ImportWelcomeSeen";
    }
    static QJsonArray profiles()
    {
        auto config = qEnvironmentVariable("XDG_CONFIG_HOME");
        if (config.isEmpty()) config = QDir::homePath() + "/.config";
        return ChromeImport::profiles(config);
    }
    static ErrorOr<JsonValue> status()
    {
        QJsonObject value { { "welcome", !QFile::exists(marker()) }, { "profiles", profiles() } };
        return json(value);
    }
    static ErrorOr<JsonValue> dismiss()
    {
        QSaveFile file(marker());
        if (!file.open(QIODevice::WriteOnly) || file.write("1\n") != 2 || !file.commit())
            return Error::from_string_literal("Could not save import preference");
        return JsonValue {};
    }
    static ErrorOr<JsonValue> json(QJsonObject const& object)
    {
        auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        return JsonValue::from_string(StringView(bytes.constData(), bytes.size()));
    }
    ErrorOr<void> start(QJsonObject request, Function<void(QString const&, bool)> open_tab, Completion completion)
    {
        m_completion = move(completion); m_open_tab = move(open_tab);
        QJsonArray known;
        auto collect = [&](auto&& self, auto const& items) -> void {
            for (auto const& item : items) {
                if (item.is_folder()) self(self, item.folder().children);
                else {
                    auto url = item.bookmark().url.serialize();
                    known.append(QString::fromUtf8(url.bytes_as_string_view().characters_without_null_termination(), url.bytes().size()));
                }
            }
        };
        collect(collect, WebView::Application::bookmark_store().root_items());
        request["known"] = known;
        auto submitted = m_worker.submit(move(request), [this](auto result) {
            if (result.is_error()) { finish(result.release_error()); return; }
            m_data = result.release_value();
            auto converted = json(m_data);
            if (converted.is_error()) { finish(converted.release_error()); return; }
            if (auto items = converted.value().as_object().get_array("bookmarks"sv); items.has_value())
                WebView::Application::bookmark_store().import_items(*items);
            m_history = m_data["history"].toArray(); m_tabs = m_data["tabs"].toArray();
            step();
        });
        if (submitted.is_error()) { auto error = submitted.release_error(); finish(move(error)); return {}; }
        return {};
    }
private:
    static Worker::Result parse(QJsonObject const& request, GCancellable* cancel)
    {
        QJsonObject result;
        QJsonArray warnings;
        auto profile = request["profile"].toString();
        QString error;
        if (request["bookmarks"].toBool()) {
            auto bytes = ChromeImport::read(profile + "/Bookmarks", error);
            QJsonParseError json_error;
            auto document = QJsonDocument::fromJson(bytes, &json_error);
            if (error.isEmpty() && (json_error.error != QJsonParseError::NoError || !document.object()["roots"].isObject())) error = "Invalid Chrome bookmarks.";
            if (error.isEmpty()) {
                auto roots = document.object()["roots"].toObject();
                QJsonArray source;
                for (auto it = roots.begin(); it != roots.end(); ++it) if (it.value().isObject()) source.append(it.value());
                QSet<QString> known;
                for (auto const& url : request["known"].toArray()) known.insert(url.toString());
                int added = 0, skipped = 0;
                auto items = ChromeImport::bookmarks(source, known, added, skipped, error);
                if (error.isEmpty()) { result["bookmarks"] = items; result["bookmarksAdded"] = added; result["skipped"] = skipped; }
            }
            if (!error.isEmpty()) { warnings.append("Bookmarks: " + error); error.clear(); }
        }
        if (g_cancellable_is_cancelled(cancel)) return Error::from_string_literal("Import cancelled");
        if (request["history"].toBool()) {
            auto history = ChromeImport::history(profile + "/History", error);
            if (error.isEmpty()) result["history"] = history;
            else { warnings.append("History: " + error); error.clear(); }
        }
        if (request["tabs"].toBool()) {
            QDir sessions(profile + "/Sessions");
            auto files = sessions.entryList({ "Session_*" }, QDir::Files, QDir::Name | QDir::Reversed);
            auto path = files.isEmpty() ? profile + "/Last Session" : sessions.filePath(files.first());
            auto bytes = ChromeImport::read(path, error);
            if (error.isEmpty()) result["tabs"] = ChromeImport::tabs(bytes, error);
            if (!error.isEmpty()) warnings.append("Tabs: " + error);
        }
        result["warnings"] = warnings;
        return result;
    }
    void step()
    {
        auto& store = *WebView::Application::default_session().history_store;
        // Yield between bounded write batches so menus and input stay responsive.
        for (int count = 0; count < 10 && m_history_index < m_history.size(); ++count) {
            auto item = m_history[m_history_index++].toObject();
            auto url = URL::Parser::basic_parse(import_string(item["url"].toString()));
            if (!url.has_value()) continue;
            if (store.entry_for_url(*url).has_value()) { ++m_skipped; continue; }
            store.record_visit(*url, import_string(item["title"].toString()), UnixDateTime::from_milliseconds_since_epoch(item["time"].toInteger()));
            ++m_history_added;
        }
        if (m_history_index < m_history.size()) { QTimer::singleShot(0, this, [this] { step(); }); return; }
        if (m_tab_index < m_tabs.size()) {
            auto item = m_tabs[m_tab_index++].toObject(); m_open_tab(item["url"].toString(), item["pinned"].toBool());
            QTimer::singleShot(0, this, [this] { step(); }); return;
        }
        QJsonObject report { { "bookmarks", m_data["bookmarksAdded"].toInt() }, { "history", m_history_added }, { "tabs", m_tab_index },
            { "skipped", m_skipped + m_data["skipped"].toInt() }, { "warnings", m_data["warnings"] } };
        finish(json(report));
    }
    void finish(ErrorOr<JsonValue> result) { auto done = move(m_completion); if (done) done(move(result)); deleteLater(); }
    Worker m_worker;
    Completion m_completion;
    Function<void(QString const&, bool)> m_open_tab;
    QJsonObject m_data;
    QJsonArray m_history, m_tabs;
    int m_history_index = 0, m_tab_index = 0, m_history_added = 0, m_skipped = 0;
};
}
