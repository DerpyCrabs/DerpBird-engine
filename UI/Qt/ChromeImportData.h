#pragma once

#include <QByteArray>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QMap>
#include <QtEndian>
#include <algorithm>
#include <QStringDecoder>
#include <QUuid>
#include <QUrl>
#include <sqlite3.h>
#include <memory>

namespace Ladybird::ChromeImport {
constexpr qint64 max_file = 32 * 1024 * 1024;
inline bool web_url(QString const& text)
{
    QUrl url(text, QUrl::StrictMode);
    return text.size() <= 8192 && url.isValid() && !url.host().isEmpty()
        && (url.scheme() == "https" || url.scheme() == "http") && url.userInfo().isEmpty();
}
inline QByteArray read(QString const& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = "Cannot read the selected file."; return {}; }
    if (file.size() > max_file) { error = "The selected file exceeds 32 MiB."; return {}; }
    auto bytes = file.read(max_file + 1);
    if (bytes.size() > max_file || file.error() != QFileDevice::NoError) { error = "Cannot read the selected file."; return {}; }
    return bytes;
}
struct Password { QString url; QString username; QByteArray password; };
struct Passwords { QList<Password> items; int skipped { 0 }; QString error; };
inline Passwords passwords(QByteArray bytes)
{
    Passwords result;
    if (bytes.size() > max_file) { result.error = "CSV exceeds 32 MiB."; return result; }
    QStringDecoder decoder(QStringDecoder::Utf8);
    auto text = QString(decoder(bytes));
    bytes.fill('\0');
    if (decoder.hasError()) { result.error = "CSV must be UTF-8."; return result; }
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false, closed = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        auto c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
                else { quoted = false; closed = true; }
            } else field += c;
        } else if (c == ',' || c == '\n' || c == '\r') {
            row.append(field); field.clear(); closed = false;
            if (c != ',') {
                rows.append(row); row.clear();
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            }
        } else if (c == '"' && field.isEmpty() && !closed) quoted = true;
        else if (closed || c == '"') { result.error = "Malformed CSV quoting."; return result; }
        else field += c;
        if (field.size() > 65536 || row.size() > 32 || rows.size() > 10001) { result.error = "CSV exceeds import limits (10,000 logins)."; return result; }
    }
    if (quoted) { result.error = "Unterminated quoted CSV field."; return result; }
    if (!field.isEmpty() || !row.isEmpty() || closed) { row.append(field); rows.append(row); }
    if (rows.isEmpty()) { result.error = "CSV is empty."; return result; }
    auto headers = rows.takeFirst();
    for (auto& header : headers) header = header.trimmed().toLower();
    int url = headers.indexOf("url"), username = headers.indexOf("username"), password = headers.indexOf("password");
    if (url < 0 || username < 0 || password < 0 || QSet<QString>(headers.begin(), headers.end()).size() != headers.size()) {
        result.error = "Expected Chrome CSV columns: url, username, password."; return result;
    }
    for (auto const& cells : rows) {
        if (cells.size() == 1 && cells[0].isEmpty()) continue;
        if (cells.size() != headers.size() || !web_url(cells[url]) || cells[username].toUtf8().size() > 1024
            || cells[password].isEmpty() || cells[password].toUtf8().size() > 65536) { ++result.skipped; continue; }
        result.items.append({ cells[url], cells[username], cells[password].toUtf8() });
    }
    return result;
}
inline QJsonArray profiles(QString const& config)
{
    QJsonArray out;
    for (auto const& browser : { "google-chrome", "google-chrome-beta", "google-chrome-unstable", "chromium" }) {
        QDir root(config + '/' + browser);
        QFile state(root.filePath("Local State"));
        QJsonObject names;
        if (state.open(QIODevice::ReadOnly) && state.size() <= max_file)
            names = QJsonDocument::fromJson(state.readAll()).object()["profile"].toObject()["info_cache"].toObject();
        for (auto const& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (name != "Default" && !name.startsWith("Profile ")) continue;
            QString path = root.filePath(name);
            if (!QFile::exists(path + "/Preferences")) continue;
            out.append(QJsonObject { { "id", path }, { "name", QString(browser) + " · " + names[name].toObject()["name"].toString(name) } });
        }
    }
    return out;
}
inline QJsonArray bookmarks(QJsonArray const& source, QSet<QString>& known, int& added, int& skipped, QString& error, int depth = 0)
{
    QJsonArray out;
    if (depth > 32) { error = "Bookmark nesting exceeds 32 levels."; return {}; }
    for (auto const& value : source) {
        auto item = value.toObject();
        QJsonObject converted { { "id", QUuid::createUuid().toString(QUuid::WithoutBraces) }, { "title", item["name"] } };
        auto date = item["date_added"].toString().toLongLong() / 1000 - 11644473600000LL;
        auto modified = item["date_modified"].toString().toLongLong() / 1000 - 11644473600000LL;
        converted["dateAdded"] = date >= 0 ? date : QDateTime::currentMSecsSinceEpoch();
        converted["lastModified"] = modified >= 0 ? modified : converted["dateAdded"].toInteger();
        if (item["type"] == "folder") {
            auto children = bookmarks(item["children"].toArray(), known, added, skipped, error, depth + 1);
            if (children.isEmpty()) continue;
            converted["type"] = "folder"; converted["children"] = children;
        } else if (item["type"] == "url") {
            auto url = item["url"].toString();
            if (!web_url(url) || known.contains(url)) { ++skipped; continue; }
            if (++added > 50000) { error = "Bookmark import exceeds 50,000 entries."; return {}; }
            known.insert(url); converted["type"] = "bookmark"; converted["url"] = url;
        } else { ++skipped; continue; }
        out.append(converted);
    }
    return out;
}
inline QJsonArray history(QString const& path, QString& error)
{
    sqlite3* raw = nullptr;
    auto filename = QFile::encodeName(path);
    int opened = sqlite3_open_v2(filename.constData(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    if (opened != SQLITE_OK) { error = "Cannot read Chrome history. Close Chrome and try again."; return {}; }
    sqlite3_busy_timeout(raw, 1000);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(raw, "SELECT url,title,last_visit_time FROM urls WHERE hidden=0 ORDER BY last_visit_time DESC LIMIT 100001", -1, &statement, nullptr) != SQLITE_OK) {
        error = "Unsupported Chrome history database."; return {};
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
    QJsonArray out;
    int status;
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        auto url = QString::fromUtf8(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
        auto title = QString::fromUtf8(reinterpret_cast<char const*>(sqlite3_column_text(statement, 1)));
        auto timestamp = sqlite3_column_int64(statement, 2) / 1000 - 11644473600000LL;
        if (web_url(url) && timestamp >= 0) out.append(QJsonObject { { "url", url }, { "title", title }, { "time", timestamp } });
        if (out.size() > 100000) { error = "History exceeds 100,000 entries."; return {}; }
    }
    if (status != SQLITE_DONE) error = "Could not finish reading Chrome history. Close Chrome and try again.";
    return out;
}
// Chromium session_service_commands.cc and base_session_service_commands.cc.
// Replay the session log rather than collecting obsolete navigation URLs.
inline QJsonArray tabs(QByteArray const& data, QString& error)
{
    auto integer = [](QByteArray const& b, int offset) { return qFromLittleEndian<qint32>(b.constData() + offset); };
    if (data.size() < 8 || data.first(4) != "SNSS") { error = "Invalid Chrome session file."; return {}; }
    int version = integer(data, 4);
    if (version != 1 && version != 3) { error = "Chrome session is encrypted or uses an unsupported version; tabs were not imported."; return {}; }
    struct Tab { int window = 0; int order = 0; int selected = 0; bool pinned = false; QMap<int, QString> navigation; };
    QMap<int, Tab> state;
    QSet<int> closed_windows;
    bool marker = version == 1;
    for (qsizetype offset = 8; offset < data.size();) {
        if (data.size() - offset < 3) { error = "Incomplete Chrome session. Close Chrome and try again."; return {}; }
        int size = qFromLittleEndian<quint16>(data.constData() + offset); offset += 2;
        if (size < 1 || size > data.size() - offset) { error = "Incomplete Chrome session. Close Chrome and try again."; return {}; }
        int command = static_cast<unsigned char>(data[offset]);
        auto body = data.mid(offset + 1, size - 1); offset += size;
        if (command == 255) { marker = true; continue; }
        if (command == 6) {
            if (body.size() < 16 || integer(body, 0) != body.size() - 4) { error = "Invalid Chrome navigation record."; return {}; }
            int id = integer(body, 4), index = integer(body, 8), length = integer(body, 12);
            if (length < 0 || length > body.size() - 16) { error = "Invalid Chrome navigation URL."; return {}; }
            state[id].navigation[index] = QString::fromUtf8(body.mid(16, length));
        } else if (command == 0 || command == 2 || command == 7 || command == 5 || command == 11 || command == 24) {
            if (body.size() < (command == 24 ? 12 : 8)) { error = "Invalid Chrome session command."; return {}; }
            int id = integer(body, 0), value = integer(body, 4);
            if (command == 0) state[value].window = id;
            else if (command == 2) state[id].order = value;
            else if (command == 7) state[id].selected = value;
            else if (command == 5) {
                auto& nav = state[id].navigation;
                for (auto it = nav.lowerBound(value); it != nav.end();) it = nav.erase(it);
            } else {
                int start = command == 11 ? 0 : value, count = command == 11 ? value : integer(body, 8);
                if (start < 0 || count <= 0 || count > 1000000 || start > 1000000) { error = "Invalid navigation pruning."; return {}; }
                auto& tab = state[id];
                QMap<int, QString> next;
                for (auto it = tab.navigation.begin(); it != tab.navigation.end(); ++it)
                    if (it.key() < start) next[it.key()] = it.value();
                    else if (it.key() >= start + count) next[it.key() - count] = it.value();
                tab.navigation = next;
                if (tab.selected >= start) tab.selected = tab.selected < start + count ? start - 1 : tab.selected - count;
            }
        } else if (command == 16 || command == 17) {
            if (body.size() < 4) { error = "Invalid closed-tab record."; return {}; }
            if (command == 16) state.remove(integer(body, 0)); else closed_windows.insert(integer(body, 0));
        } else if (command == 12) {
            if (body.size() < 5) { error = "Invalid pinned-tab record."; return {}; }
            state[integer(body, 0)].pinned = body[4] != 0;
        }
        if (state.size() > 10000) { error = "Session exceeds 10,000 tabs."; return {}; }
    }
    if (!marker) { error = "Chrome has not finished writing its session."; return {}; }
    auto ordered = state.values();
    std::stable_sort(ordered.begin(), ordered.end(), [](auto const& a, auto const& b) { return a.window == b.window ? a.order < b.order : a.window < b.window; });
    QJsonArray out;
    for (auto const& tab : ordered) {
        if (!tab.window || closed_windows.contains(tab.window) || tab.navigation.isEmpty()) continue;
        auto it = tab.navigation.lowerBound(tab.selected);
        if (it == tab.navigation.end()) --it;
        if (web_url(it.value())) out.append(QJsonObject { { "url", it.value() }, { "pinned", tab.pinned } });
    }
    return out;
}

}
