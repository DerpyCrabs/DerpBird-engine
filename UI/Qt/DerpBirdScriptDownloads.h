#pragma once

#include <AK/JsonValue.h>
#include <LibCore/Promise.h>
#include <QFileInfo>
#include <LibURL/Parser.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/HeaderList.h>
#include <LibWeb/Page/DerpBirdValueProtocol.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebContentClient.h>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace Ladybird {

class DerpBirdScriptDownloads final : public QObject, public WebView::FileDownloaderObserver {
public:
    using Protocol = Web::DerpBirdValueProtocol;
    using Completion = Function<void(ErrorOr<ByteBuffer>)>;
    explicit DerpBirdScriptDownloads(QObject* parent) : QObject(parent) { }
    Function<NonnullRefPtr<Core::Promise<LexicalPath>>(ByteString const&)> choose_path;
    void prune() { m_entries.removeIf([](auto const& item) { return !item.value()->valid(); }); }
    void shutdown() { m_entries.clear(); }
    ErrorOr<void> dispatch(WebView::WebContentPage& page, QString const& token, Protocol::Request const& request,
        Function<bool()> valid, Completion completion)
    {
        auto key = token + ":" + QString::fromUtf8(request.key.bytes_as_string_view().characters_without_null_termination(), request.key.bytes().size());
        prune();
        auto& app = WebView::Application::the();
        auto& downloader = app.file_downloader();
        if (request.method == Protocol::Method::Download || request.method == Protocol::Method::DownloadLegacy) {
            if (m_entries.size() >= 64 || m_entries.contains(key)) return Error::from_string_literal("Too many or duplicate userscript downloads");
            auto json = TRY(JsonValue::from_string(request.updates[0].value->bytes()));
            if (!json.is_object()) return Error::from_string_literal("Invalid download options");
            auto const& options = json.as_object();
            auto text = options.get_string("url"sv);
            auto url = text.has_value() ? URL::Parser::basic_parse(*text) : Optional<URL::URL> {};
            if (!url.has_value() || !url->username().is_empty() || !url->password().is_empty())
                return Error::from_string_literal("Unsupported userscript download URL");
            Optional<u64> blob_size;
            if (options.has("blobSize"sv)) {
                blob_size = options.get_integer<u64>("blobSize"sv);
                if (!blob_size.has_value() || *blob_size > 9007199254740991ULL) return Error::from_string_literal("Invalid Blob download size");
            }
            // Producer downloads never fetch this URL: it identifies the source
            // document, which may itself be a blob or local page.
            if (!blob_size.has_value() && !url->scheme().is_one_of("http"sv, "https"sv))
                return Error::from_string_literal("Unsupported userscript download URL");
            RefPtr<HTTP::HeaderList> headers;
            if (options.has("headers"sv)) {
                auto fields = options.get_object("headers"sv);
                if (!fields.has_value() || fields->size() > 128) return Error::from_string_literal("Invalid download headers");
                headers = HTTP::HeaderList::create();
                Optional<Error> error;
                fields->for_each_member([&](auto const& name, auto const& value) {
                    if (error.has_value()) return;
                    if (!value.is_string() || !HTTP::is_header_name(name) || !HTTP::is_header_value(value.as_string())) {
                        error = Error::from_string_literal("Invalid download header"); return;
                    }
                    HTTP::Header header { name.to_byte_string(), value.as_string().to_byte_string() };
                    // Byte ranges and validators are owned by FileDownloader.
                    if (HTTP::is_forbidden_request_header(header) || header.name.equals_ignoring_ascii_case("Range"sv)
                        || header.name.equals_ignoring_ascii_case("If-Range"sv) || header.name.equals_ignoring_ascii_case("If-Match"sv)
                        || header.name.equals_ignoring_ascii_case("If-None-Match"sv) || header.name.equals_ignoring_ascii_case("If-Modified-Since"sv)
                        || header.name.equals_ignoring_ascii_case("If-Unmodified-Since"sv)) {
                        error = Error::from_string_literal("Forbidden or downloader-owned header"); return;
                    }
                    headers->set(move(header));
                });
                if (error.has_value()) return error.release_value();
            }
            auto conflict = options.get_string("conflictAction"sv).value_or("uniquify"_string);
            if (!conflict.is_one_of("uniquify"sv, "overwrite"sv, "prompt"sv)) return Error::from_string_literal("Invalid download conflict mode");
            if (options.has("saveAs"sv) && !options.get_bool("saveAs"sv).has_value()) return Error::from_string_literal("Invalid saveAs option");
            auto suggested = url->basename();
            if (suggested.is_empty()) suggested = "download";
            auto name = options.get_string("name"sv).value_or(TRY(String::from_utf8(suggested.view())));
            auto bytes = name.bytes_as_string_view();
            if (bytes.is_empty() || bytes.length() > 240 || bytes == "."sv || bytes == ".."sv
                || bytes.contains('/') || bytes.contains('\\')) return Error::from_string_literal("Download name must be a filename");
            for (auto ch : bytes) if (static_cast<u8>(ch) < 32 || ch == 127) return Error::from_string_literal("Invalid download filename");
            auto entry = std::make_shared<Entry>(move(valid));
            entry->blob_size = blob_size;
            entry->downloader = downloader.make_weak_ptr();
            m_entries.insert(key, entry);
            bool chosen = options.get_bool("saveAs"sv).value_or(false) || conflict == "prompt"sv;
            auto start = [self = QPointer(this), entry, key, headers = move(headers), url = *url, chosen, overwrite = conflict == "overwrite"sv, is_private = page.client().session().is_private()](ErrorOr<LexicalPath> path) mutable {
                if (!self || !entry->valid() || entry->canceled) return;
                if (path.is_error()) {
                    entry->failure = MUST(String::formatted("{}", path.error()));
                    self->deliver(key, entry, true);
                    return;
                }
                auto replace = (chosen || overwrite) && QFileInfo::exists(QString::fromUtf8(path.value().string().characters()))
                    ? WebView::FileDownloader::MayReplaceDestination::Yes : WebView::FileDownloader::MayReplaceDestination::No;
                auto& downloader = WebView::Application::the().file_downloader();
                entry->id = entry->blob_size.has_value()
                    ? downloader.start_download(is_private, url, path.release_value(), entry->blob_size, replace)
                    : downloader.download_file(is_private, url, path.release_value(), replace, move(headers));
                if (entry->blob_size.has_value()) downloader.enable_producer_pause(*entry->id);
                self->deliver(key, entry, true);
            };
            // Acknowledge the handle before awaiting a potentially long-lived picker.
            auto lifetime = QPointer(this);
            completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
            if (!lifetime || !entry->valid() || entry->canceled) return {};
            if (chosen) {
                auto shared = std::make_shared<decltype(start)>(move(start));
                auto picker = choose_path ? choose_path(ByteString(bytes)) : app.path_for_downloaded_file(ByteString(bytes));
                picker->when_resolved([shared](auto const& path) { (*shared)(path); })
                    .when_rejected([shared](auto const&) { (*shared)(Error::from_string_literal("Download canceled by user")); });
            } else {
                auto path = app.default_path_for_downloaded_file(ByteString(bytes));
                if (!path.is_error() && conflict == "overwrite"sv)
                    path = LexicalPath::join(path.value().dirname(), bytes);
                start(move(path));
            }
            return {};
        }
        auto entry = m_entries.value(key);
        if (!entry) return Error::from_string_literal("Unknown userscript download");
        if (request.method == Protocol::Method::DownloadAbort) {
            entry->canceled = true;
            if (entry->id.has_value()) downloader.cancel_download(*entry->id);
            deliver(key, entry, true);
            completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
            return {};
        }
        if (request.method == Protocol::Method::DownloadChunk || request.method == Protocol::Method::DownloadFinish) {
            if (!entry->blob_size.has_value() || !entry->id.has_value()) return Error::from_string_literal("Blob transfer is not ready");
            auto status = downloader.download(*entry->id);
            // Resuming makes the last segment's end explicit. Appending its
            // final bytes can therefore complete the native download before
            // the producer sends Finish. Acknowledge that exact completed
            // transfer; never accept extra chunks or incomplete producers.
            if (request.method == Protocol::Method::DownloadFinish && status.has_value()
                && status->status == WebView::FileDownloader::DownloadStatus::Completed
                && entry->written == *entry->blob_size) {
                completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
                return {};
            }
            if (status.has_value() && status->status == WebView::FileDownloader::DownloadStatus::Paused) {
                // Do not retain chunks or hold an IPC request while a user pauses indefinitely.
                completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 1 }));
                return {};
            }
            if (!status.has_value() || status->status != WebView::FileDownloader::DownloadStatus::InProgress)
                return Error::from_string_literal("Blob download is no longer active");
            if (request.method == Protocol::Method::DownloadChunk) {
                auto bytes = request.updates[0].value->bytes();
                if (bytes.size() > *entry->blob_size - entry->written) return Error::from_string_literal("Blob download exceeds declared size");
                downloader.append_download_data(*entry->id, bytes);
                entry->written += bytes.size();
            } else {
                if (entry->written != *entry->blob_size) return Error::from_string_literal("Incomplete Blob download");
                downloader.finish_download(*entry->id);
            }
            completion(Protocol::encode(Protocol::Response { Protocol::Kind::Revision, {}, {}, {}, 0 }));
            return {};
        }
        if (entry->pending) return Error::from_string_literal("Download already has a progress observer");
        entry->pending = move(completion);
        if (deliver(key, entry)) return {};
        // Bound the long poll below the IPC deadline. No periodic JS polling.
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        entry->timer = timer;
        connect(timer, &QTimer::timeout, timer, [this, key, weak = std::weak_ptr<Entry>(entry)] {
            if (auto item = weak.lock(); item && item->pending) deliver(key, item, true);
        });
        timer->start(20000);
        return {};
    }
private:
    struct Entry {
        explicit Entry(Function<bool()> valid) : valid(move(valid)) { }
        ~Entry() {
            if (timer) timer->deleteLater();
            if (blob_size.has_value() && id.has_value() && downloader) downloader->cancel_download(*id);
        }
        WeakPtr<WebView::FileDownloader> downloader;
        Optional<u64> blob_size;
        u64 written { 0 };
        bool canceled { false };
        Optional<String> failure;
        Function<bool()> valid;
        QPointer<QTimer> timer;
        Optional<u64> id;
        Completion pending;
        String last;
    };
    bool deliver(QString const& key, std::shared_ptr<Entry> const& entry, bool force = false)
    {
        if (!entry->pending) return false;
        if (!entry->valid()) { m_entries.remove(key); entry->pending = nullptr; return true; }
        auto download = entry->id.has_value() ? WebView::Application::the().file_downloader().download(*entry->id)
            : Optional<WebView::FileDownloader::Download const&> {};
        JsonObject state;
        bool terminal = true;
        if (download.has_value()) {
            using Status = WebView::FileDownloader::DownloadStatus;
            auto const& item = *download;
            auto status = item.status == Status::Completed ? "completed"sv : item.status == Status::Failed ? "failed"sv
                : item.status == Status::Canceled ? "canceled"sv : item.status == Status::Paused ? "paused"sv : "progress"sv;
            terminal = !WebView::FileDownloader::status_is_active(item.status);
            state.set("status"sv, status);
            state.set("loaded"sv, item.downloaded_size);
            state.set("total"sv, item.total_size.value_or(0));
            state.set("lengthComputable"sv, item.total_size.has_value());
            if (item.error.has_value()) state.set("message"sv, *item.error);
        } else {
            // A handle with an assigned ID was already started. Removing it
            // from the download manager must finish the observer, not turn it
            // back into an indefinitely pending file picker.
            bool removed = entry->id.has_value();
            terminal = removed || entry->canceled || entry->failure.has_value();
            state.set("status"sv, entry->canceled || removed ? "canceled"sv : entry->failure.has_value() ? "failed"sv : "waiting"sv);
            if (entry->failure.has_value()) state.set("message"sv, *entry->failure);
        }
        auto json = JsonValue(move(state)).serialized();
        if (!force && json == entry->last) return false;
        entry->last = json;
        if (entry->timer) { entry->timer->stop(); entry->timer->deleteLater(); entry->timer = nullptr; }
        auto done = move(entry->pending);
        if (terminal) m_entries.remove(key);
        done(Protocol::encode(Protocol::Response { Protocol::Kind::Network, {}, MUST(ByteBuffer::copy(json.bytes())), {}, 0 }));
        return true;
    }
    void download_updated(WebView::FileDownloader::Download const& download) override { changed(download.id); }
    void download_removed(u64 id, WebView::IsPrivate) override { changed(id); }
    void changed(u64 id)
    {
        // FileDownloader can notify during cancel/start: don't re-enter its maps.
        (void)id;
        if (m_delivery_queued) return;
        m_delivery_queued = true;
        QTimer::singleShot(50, this, [this] {
            m_delivery_queued = false;
            auto entries = m_entries;
            auto lifetime = QPointer(this);
            for (auto const& key : entries.keys()) {
                if (!lifetime) return;
                auto entry = entries.value(key);
                if (entry->id.has_value()) deliver(key, entry);
            }
        });
    }
    bool m_delivery_queued { false };
    QHash<QString, std::shared_ptr<Entry>> m_entries;
};
}
