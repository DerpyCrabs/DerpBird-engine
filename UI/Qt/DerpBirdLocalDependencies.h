#pragma once
#include <AK/Error.h>
#include <AK/String.h>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QUrl>

namespace Ladybird {
inline QString derpbird_dependency_text(StringView text) { return QString::fromUtf8(text.characters_without_null_termination(), text.length()); }
inline String derpbird_dependency_string(QString const& text)
{
    auto bytes = text.toUtf8();
    return MUST(String::from_utf8(StringView(bytes.constData(), static_cast<size_t>(bytes.size()))));
}

struct DerpBirdLocalScript { QString root; QString path; };
// Resolve identity through the native-selected directory registry. Editor paths
// never authorize filesystem access, and dependencies stay within that root.
inline ErrorOr<DerpBirdLocalScript> derpbird_local_script(QString const& config_path, QString const& id)
{
    if (!id.startsWith("folder_") || id.size() != 71)
        return Error::from_string_literal("Relative dependencies require a directory-backed userscript");
    QFile config(config_path);
    if (!config.open(QIODevice::ReadOnly) || config.size() > 128 * 1024)
        return Error::from_string_literal("Cannot read userscript directory configuration");
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(config.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray() || document.array().size() > 16)
        return Error::from_string_literal("Invalid userscript directory configuration");
    size_t visited = 0;
    for (auto const& value : document.array()) {
        if (!value.isString() || !QDir::isAbsolutePath(value.toString())) continue;
        auto root = QFileInfo(value.toString()).canonicalFilePath();
        if (root.isEmpty() || !QFileInfo(root).isDir()) continue;
        QDirIterator iterator(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (++visited > 8192) return Error::from_string_literal("Userscript directory exceeds entry limit");
            auto path = iterator.next();
            if (!path.endsWith(".js") || !QFileInfo(path).isFile()) continue;
            path = QFileInfo(path).canonicalFilePath();
            auto key = "folder_" + QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex());
            if (key == id) return DerpBirdLocalScript { root, path };
        }
    }
    return Error::from_string_literal("Directory-backed userscript is no longer available");
}
struct DerpBirdLocalDependency { QByteArray bytes; QString mime; };
inline ErrorOr<DerpBirdLocalDependency> derpbird_read_local_dependency(DerpBirdLocalScript const& script, QString const& address)
{
    QUrl url(address, QUrl::StrictMode);
    if (!url.isValid() || !url.isRelative() || !url.host().isEmpty() || url.hasQuery() || url.path().isEmpty()
        || QDir::isAbsolutePath(url.path()) || url.path().contains(QChar('\0')))
        return Error::from_string_literal("Local userscript dependency must be a relative path");
    auto path = QFileInfo(QDir(QFileInfo(script.path).absolutePath()).filePath(url.path())).canonicalFilePath();
    auto prefix = script.root.endsWith('/') ? script.root : script.root + '/';
    if (path.isEmpty() || !path.startsWith(prefix) || !QFileInfo(path).isFile())
        return Error::from_string_literal("Local dependency is missing or outside the selected directory");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024)
        return Error::from_string_literal("Cannot read local dependency or it exceeds 2 MiB");
    auto bytes = file.read(2 * 1024 * 1024 + 1);
    if (file.error() != QFile::NoError || bytes.size() > 2 * 1024 * 1024)
        return Error::from_string_literal("Cannot read local dependency");
    return DerpBirdLocalDependency { move(bytes), QMimeDatabase().mimeTypeForFile(path).name() };
}
}
