#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <UI/Qt/StringUtils.h>

namespace Ladybird {

// Paths are selected by the native picker, never supplied as arbitrary shell read requests.
inline ErrorOr<JsonValue> derpbird_source_folders(QWidget* parent, QString const& config_path, bool scripts, JsonObject const& request, bool scan = true, QStringList* watch_paths = nullptr)
{
    // QFileSystemWatcher cannot watch a path that no longer exists. Keep the
    // nearest surviving ancestor so recreating a removed directory tree will
    // trigger another scan, which moves the watch down to the restored root.
    auto watch_source_path = [&](QString const& path) {
        if (!scan || !watch_paths) return;
        if (QFileInfo::exists(path)) watch_paths->append(path);
        auto ancestor = QFileInfo(path).absolutePath();
        while (!QFileInfo(ancestor).isDir()) {
            auto parent = QFileInfo(ancestor).absolutePath();
            if (parent == ancestor) break;
            ancestor = parent;
        }
        if (QFileInfo(ancestor).isDir()) watch_paths->append(ancestor);
    };
    // Register recovery watches even when configuration parsing fails.
    watch_source_path(config_path);
    QStringList directories;
    QFile config(config_path);
    if (config.exists()) {
        if (!config.open(QIODevice::ReadOnly) || config.size() > 128 * 1024)
            return Error::from_string_literal("Cannot read source directory configuration");
        QJsonParseError error;
        auto document = QJsonDocument::fromJson(config.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isArray() || document.array().size() > 16)
            return Error::from_string_literal("Invalid source directory configuration");
        for (auto const& entry : document.array()) {
            if (!entry.isString() || !QDir::isAbsolutePath(entry.toString()))
                return Error::from_string_literal("Invalid source directory");
            directories.append(entry.toString());
        }
    }
    auto action = request.get_string("action"sv);
    bool changed = false;
    if (action == "add"sv) {
        auto selected = QFileDialog::getExistingDirectory(parent, scripts ? "Каталог пользовательских скриптов" : "Каталог пользовательских стилей");
        if (!selected.isEmpty()) {
            selected = QFileInfo(selected).canonicalFilePath();
            if (selected.isEmpty()) return Error::from_string_literal("Selected directory disappeared");
            if (!directories.contains(selected)) { directories.append(selected); changed = true; }
        }
    } else if (action == "remove"sv) {
        auto path = request.get_string("path"sv);
        if (!path.has_value()) return Error::from_string_literal("Missing source directory");
        changed = directories.removeAll(qstring_from_ak_string(*path)) > 0;
    } else if (action != "list"sv) {
        return Error::from_string_literal("Unknown source directory operation");
    }
    if (directories.size() > 16) return Error::from_string_literal("At most 16 source directories are supported");
    JsonArray paths;
    for (auto const& directory : directories) {
        paths.must_append(ak_string_from_qstring(directory));
        // A failed scan of one root must not prevent recovery watches for the rest.
        watch_source_path(directory);
    }
    JsonObject style_imports;
    auto scanned = [&]() -> ErrorOr<JsonArray> {
    JsonArray files;
    if (!scan) return files;
    QSet<QString> seen;
    qint64 total = 0;
    qint64 scanned_bytes = 0;
    // Directory iteration order is unspecified. Hash sorted, fixed-size file
    // digests so an unchanged tree cannot revoke every script capability merely
    // because a filesystem or watcher returned entries in another order.
    QMap<QString, QByteArray> dependency_digests;
    int visited = 0;
    for (auto const& directory : directories) {
        auto first_file = files.size();
        QMap<QString, QByteArray> script_dependency_digests;
        if (!QFileInfo(directory).isDir() || !QFileInfo(directory).isReadable())
            return Error::from_string_literal("Source directory is unavailable; installed versions have been retained");
        QDirIterator entries(directory, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (entries.hasNext()) {
            if (++visited > 8192) return Error::from_string_literal("Too many source files");
            auto path = entries.next();
            if (watch_paths && watch_paths->size() < 8224) watch_paths->append(path);
            if (!QFileInfo(path).isFile()) continue;
            auto canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty()) continue;
            bool already_seen = seen.contains(canonical);
            if (already_seen && !scripts) continue;
            seen.insert(canonical);
            if (!scripts && !canonical.endsWith(".css", Qt::CaseInsensitive)
                && !canonical.endsWith(".less", Qt::CaseInsensitive)
                && !canonical.endsWith(".styl", Qt::CaseInsensitive)) continue;
            QFile file(canonical);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024)
                return Error::from_string_literal("Cannot read source file or file exceeds 2 MiB");
            auto bytes = file.read(2 * 1024 * 1024 + 1);
            if (file.error() != QFile::NoError || bytes.size() > 2 * 1024 * 1024) return Error::from_string_literal("Cannot read source directory file");
            if (!already_seen) scanned_bytes += bytes.size();
            if (scanned_bytes > 8 * 1024 * 1024) return Error::from_string_literal("Source directory contents exceed 8 MiB");
            auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
            if (scripts) script_dependency_digests.insert(canonical, digest);
            else dependency_digests.insert(canonical, digest);
            // Overlapping roots install each script once, but the parent root
            // still includes nested-root files in its dependency revision.
            if (already_seen) continue;
            Optional<String> style_base;
            if (scripts) {
                if (!canonical.endsWith(".js") || !bytes.contains("==UserScript==")) continue;
            } else {
                if (bytes.contains('\0')) return Error::from_string_literal("Style dependencies must be UTF-8 text");
                auto dependency_source = TRY(String::from_utf8(StringView(bytes.constData(), bytes.size())));
                auto relative = QDir(directory).relativeFilePath(canonical);
                if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative))
                    return Error::from_string_literal("Style dependency escaped selected directory");
                // Synthetic addresses identify already-read files, never grant
                // a filesystem/network read capability to the compiler.
                auto root_id = QString::fromLatin1(QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex());
                QUrl address("https://derpbird-source.invalid");
                address.setPath("/" + root_id + "/" + relative);
                style_base = ak_string_from_qstring(address.toString(QUrl::FullyEncoded));
                if (style_imports.size() >= 256) return Error::from_string_literal("Too many style dependency files");
                JsonObject dependency;
                dependency.set("url"sv, *style_base);
                dependency.set("source"sv, AK::move(dependency_source));
                style_imports.set(*style_base, AK::move(dependency));
                // Helper Less/Stylus files are dependencies, not standalone styles.
                if (!canonical.endsWith(".css", Qt::CaseInsensitive) && !bytes.contains("==UserStyle==")) continue;
            }
            if (files.size() >= 256) return Error::from_string_literal("Too many source files");
            total += bytes.size();
            if (file.error() != QFile::NoError || total > 2 * 1024 * 1024 || bytes.contains('\0'))
                return Error::from_string_literal("Source files must be UTF-8 text, at most 2 MiB combined");
            auto source = TRY(String::from_utf8(StringView(bytes.constData(), bytes.size())));
            JsonObject entry;
            entry.set("id"sv, ak_string_from_qstring("folder_" + QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex())));
            entry.set("name"sv, ak_string_from_qstring(QFileInfo(path).fileName()));
            entry.set("source"sv, AK::move(source));
            if (style_base.has_value()) entry.set("sourceBaseURL"sv, *style_base);
            files.must_append(AK::move(entry));
        }
        if (scripts) {
            // Relative script dependencies cannot leave their selected root.
            // Editing another root must not revoke these scripts' capabilities.
            QCryptographicHash dependency_hash(QCryptographicHash::Sha256);
            dependency_hash.addData(directory.toUtf8());
            dependency_hash.addData(QByteArrayView("\0", 1));
            for (auto it = script_dependency_digests.cbegin(); it != script_dependency_digests.cend(); ++it) {
                dependency_hash.addData(it.key().toUtf8());
                dependency_hash.addData(QByteArrayView("\0", 1));
                dependency_hash.addData(it.value());
            }
            auto revision = ak_string_from_qstring(QString::fromLatin1(dependency_hash.result().toHex()));
            for (size_t i = first_file; i < files.size(); ++i)
                files.values()[i].as_object().set("dependencyRevision"sv, revision);
        }
    }
    // Style imports use a shared URL snapshot and may explicitly reference
    // another selected root; retain cross-root invalidation for that graph.
    if (!scripts) {
        QCryptographicHash dependency_hash(QCryptographicHash::Sha256);
        for (auto it = dependency_digests.cbegin(); it != dependency_digests.cend(); ++it) {
            dependency_hash.addData(it.key().toUtf8());
            dependency_hash.addData(QByteArrayView("\0", 1));
            dependency_hash.addData(it.value());
        }
        auto revision = ak_string_from_qstring(QString::fromLatin1(dependency_hash.result().toHex()));
        for (auto& entry : files.values()) entry.as_object().set("dependencyRevision"sv, revision);
    }
    return files;
    }();
    if (changed) {
        QJsonArray values;
        for (auto const& path : directories) values.append(path);
        auto data = QJsonDocument(values).toJson(QJsonDocument::Compact);
        if (!QDir().mkpath(QFileInfo(config_path).absolutePath())) return Error::from_string_literal("Cannot create source configuration directory");
        QSaveFile file(config_path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || file.write(data) != data.size() || !file.commit())
            return Error::from_string_literal("Cannot save source directories");
    }
    JsonObject result;
    result.set("directories"sv, AK::move(paths));
    bool scan_failed = scanned.is_error();
    result.set("files"sv, scan_failed ? JsonArray {} : scanned.release_value());
    if (!scripts) result.set("imports"sv, scan_failed ? JsonObject {} : AK::move(style_imports));
    if (scan_failed) result.set("error"sv, "Не удалось прочитать каталог или исходные файлы. Проверьте доступность, UTF-8, предел 2 МиБ для исходников и 8 МиБ для каталога; установленные версии сохранены."sv);
    return result;
}
}
