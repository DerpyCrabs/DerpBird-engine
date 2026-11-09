#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <LibWebView/Menu.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/SessionStore.h>
#include <LibWebView/Profile.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI.h>
#include <UI/Qt/DerpBirdImport.h>
#include <UI/Qt/DerpBirdFilePicker.h>
#include <UI/Qt/DerpBirdProxy.h>
#include <QFileDialog>
#include <QRegion>
#include <QBuffer>
#include <LibWebView/URL.h>
#include <QResizeEvent>
#include <QTimer>
#include <QShortcut>
#include <QKeyEvent>
#include <QUuid>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/DerpBirdChrome.h>
#include <UI/Qt/DerpBirdCredentialFill.h>
#include <UI/Qt/DerpBirdStyleStore.h>
#include <UI/Qt/DerpBirdScriptStore.h>
#include <UI/Qt/DerpBirdScriptInstall.h>
#include <UI/Qt/DerpBirdSourceFolders.h>
#include <QThread>
#include <memory>
#include <LibWebCommon/WebView/PageInfo.h>
#include <UI/Qt/DerpBirdSourceDownload.h>
#include <UI/Qt/NavigationPolicy.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Tab.h>
#include <UI/Qt/WebContentView.h>

namespace Ladybird {

class NavigationObserver final : public WebView::Action::Observer {
public:
    explicit NavigationObserver(DerpBirdChrome& chrome)
        : m_chrome(&chrome)
    {
    }
    void on_enabled_state_changed(WebView::Action&) override
    {
        if (m_chrome)
            m_chrome->queue_snapshot();
    }

private:
    QPointer<DerpBirdChrome> m_chrome;
};

static String tab_id(Tab const& tab)
{
    return ak_string_from_qstring(tab.property("derpbirdId").toString());
}

DerpBirdChrome::DerpBirdChrome(BrowserWindow& window)
    : QWidget(&window)
    , m_window(window)
{
    if (auto result = Application::the().derpbird_style_store(); result.is_error())
        warnln("Unable to load user styles: {}", result.error());
    if (auto result = Application::the().derpbird_script_store(); result.is_error())
        warnln("Unable to load userscripts: {}", result.error());
    m_shell_preferences = std::make_unique<QSettings>(QString::fromUtf8(Application::the().profile().paths().config.characters()) + "/DerpBirdChrome.ini", QSettings::IniFormat);
    // Browser commands must also work while focus belongs to a web page.
    // The native menu/toolbar is hidden when this chrome is active.
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_H), &window, [this] {
        JsonObject request;
        request.set("type"sv, "browser.open-page"sv);
        request.set("page"sv, "history"sv);
        if (auto result = command(request); result.is_error())
            warnln("Unable to open history: {}", result.error());
    });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), &window, [this] {
        if (auto* tab = m_window.current_tab()) {
            tab->ensure_loaded();
            tab->show_bookmark_popover();
        }
    });
    WebContentViewInitialState initial;
    initial.is_private = window.is_private();
    initial.maximum_frames_per_second = window.refresh_rate();
    initial.display_id = window.display_id();
    m_shell = new WebContentView(this, {}, initial);
    m_shell->set_transparent_background(true);
    m_shell->installEventFilter(this);
    m_file_picker = new DerpBirdFilePicker(this);
    m_shell->on_request_file_picker = [this](auto const& types, auto multiple) {
        m_file_picker->open(types, multiple, [this](auto files) { m_shell->file_picker_closed(AK::move(files)); });
    };
    connect(m_shell, &WebContentView::ready_to_paint, this, [this] {
        // Apply the input region with the frame that shows the corresponding controls.
        if (m_pending_surface_state.has_value()) {
            m_surface_state = m_pending_surface_state.release_value();
        }
        place_content();
    });
    m_source_watcher = new QFileSystemWatcher(this);
    auto* source_timer = new QTimer(this);
    source_timer->setSingleShot(true);
    source_timer->setInterval(500);
    connect(m_source_watcher, &QFileSystemWatcher::directoryChanged, source_timer, [source_timer] { source_timer->start(); });
    connect(m_source_watcher, &QFileSystemWatcher::fileChanged, source_timer, [source_timer] { source_timer->start(); });
    connect(source_timer, &QTimer::timeout, this, [this] {
        if (auto channel = m_shell->page().web_ui(); channel && channel->host() == "derpbird"sv)
            channel->async_send_message("derpbird.sources-changed"sv, JsonValue {});
    });
    m_autocomplete = std::make_unique<WebView::Autocomplete>(window.is_private());
    m_suggestion_timer = new QTimer(this);
    m_suggestion_timer->setSingleShot(true);
    m_suggestion_timer->setInterval(250);
    auto publish_suggestions = [this] {
        m_suggestion_timer->stop();
        if (m_pending_suggestions.is_null())
            return;
        m_address_suggestions = AK::move(m_pending_suggestions);
        m_pending_suggestions = JsonValue {};
        m_suggestions_published = true;
        queue_snapshot();
    };
    connect(m_suggestion_timer, &QTimer::timeout, this, publish_suggestions);
    m_autocomplete->on_autocomplete_query_complete = [this, publish_suggestions](u64 query, auto suggestions, auto kind) {
        if (query != m_suggestion_query || m_suggestions_published)
            return;
        JsonArray items;
        for (auto const& suggestion : suggestions) {
            if (suggestion.text == "about:derpbird"sv) continue;
            if (items.size() >= 8) break;
            JsonObject item;
            item.set("text"sv, suggestion.text);
            item.set("title"sv, suggestion.title.value_or(suggestion.text));
            if (suggestion.favicon_png.has_value()) {
                auto const& bytes = *suggestion.favicon_png;
                auto icon = QImage::fromData(bytes.data(), static_cast<int>(bytes.size()), "PNG");
                if (!icon.isNull()) {
                    QByteArray png;
                    QBuffer buffer(&png);
                    buffer.open(QIODevice::WriteOnly);
                    if (icon.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG"))
                        item.set("favicon"sv, ak_string_from_qstring("data:image/png;base64," + QString::fromLatin1(png.toBase64())));
                }
            }
            if (suggestion.subtitle.has_value()) item.set("subtitle"sv, *suggestion.subtitle);
            using Source = WebView::AutocompleteSuggestionSource;
            switch (suggestion.source) {
            case Source::Search: item.set("source"sv, "search"sv); break;
            case Source::History:
            case Source::Adaptive: item.set("source"sv, "history"sv); break;
            case Source::Bookmark: item.set("source"sv, "bookmark"sv); break;
            case Source::ChooseSearchEngine: item.set("source"sv, "settings"sv); break;
            default: item.set("source"sv, "page"sv); break;
            }
            items.must_append(AK::move(item));
        }
        JsonObject result;
        result.set("query"sv, query);
        result.set("items"sv, AK::move(items));
        m_pending_suggestions = AK::move(result);
        if (kind == WebView::AutocompleteResultKind::Final)
            publish_suggestions();
        else if (!m_suggestion_timer->isActive())
            m_suggestion_timer->start();
    };
    m_credential_proposal.on_change = [this] { queue_snapshot(); };
    m_shell->on_derpbird_request = [this](auto const& data, auto completion) {
        if (data.is_object()) {
            auto type = data.as_object().get_string("type"sv);
            if (type.has_value() && type->bytes_as_string_view().starts_with("proxy."sv)) {
                if (m_window.is_private() == WebView::IsPrivate::Yes || Application::profile().is_temporary()) {
                    completion(Error::from_string_literal("Edit proxy settings in a regular persistent window")); return;
                }
                if (type == "proxy.get"sv) {
                    auto state = DerpBirdProxy::state();
                    if (state.is_error()) completion(state.release_error());
                    else completion(JsonValue(state.release_value()));
                    return;
                }
                if (type == "proxy.replace"sv) {
                    auto result = DerpBirdProxy::replace(data.as_object());
                    if (!result.is_error()) {
                        for (auto* widget : QApplication::topLevelWidgets())
                            if (auto* window = as_if<BrowserWindow>(widget); window && window->derpbird_chrome())
                                window->derpbird_chrome()->queue_snapshot();
                    }
                    completion(AK::move(result)); return;
                }
                completion(Error::from_string_literal("Unknown proxy operation")); return;
            }
            if (type.has_value() && (type->bytes_as_string_view().starts_with("import."sv) || type == "credentials.import-csv"sv)) {
                if (m_window.is_private() == WebView::IsPrivate::Yes || Application::profile().is_temporary()) {
                    completion(Error::from_string_literal("Import requires a regular persistent profile")); return;
                }
                if (type == "import.status"sv) { completion(DerpBirdImport::status()); return; }
                if (type == "import.dismiss"sv) { completion(DerpBirdImport::dismiss()); return; }
                if (type == "import.passwords"sv) {
                    if (m_password_import) { completion(Error::from_string_literal("Password import is already running")); return; }
                    m_password_import = true;
                    auto* picker = new QFileDialog(&m_window, "Import Chrome passwords", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), "Password CSV (*.csv)");
                    picker->setFileMode(QFileDialog::ExistingFile);
                    picker->setAttribute(Qt::WA_DeleteOnClose);
                    auto done = std::make_shared<decltype(completion)>(AK::move(completion));
                    connect(picker, &QFileDialog::finished, this, [this, picker, done](int result) mutable {
                        if (result != QDialog::Accepted || picker->selectedFiles().isEmpty()) {
                            m_password_import = false; JsonObject cancelled; cancelled.set("cancelled"sv, true); (*done)(JsonValue { AK::move(cancelled) }); return;
                        }
                        JsonObject request; request.set("type"sv, "credentials.import-csv"sv);
                        request.set("path"sv, ak_string_from_qstring(picker->selectedFiles().first()));
                        Application::the().derpbird_credentials_request(JsonValue { AK::move(request) }, false,
                            [self = QPointer(this), done](auto result) mutable { if (self) self->m_password_import = false; (*done)(AK::move(result)); });
                    });
                    picker->open(); return;
                }
                if (type == "import.chrome"sv) {
                    if (m_import) { completion(Error::from_string_literal("Import is already running")); return; }
                    auto profile = data.as_object().get_string("profile"sv);
                    bool found = false;
                    QString selected;
                    if (profile.has_value()) selected = QString::fromUtf8(profile->bytes_as_string_view().characters_without_null_termination(), profile->bytes().size());
                    for (auto const& item : DerpBirdImport::profiles()) if (item.toObject()["id"].toString() == selected) found = true;
                    if (!found) { completion(Error::from_string_literal("Chrome profile no longer exists")); return; }
                    auto serialized = data.serialized();
                    auto request = QJsonDocument::fromJson(QByteArray(serialized.bytes_as_string_view().characters_without_null_termination(), serialized.bytes().size())).object();
                    m_import = new DerpBirdImport(this);
                    auto result = m_import->start(AK::move(request), [this](QString const& text, bool pinned) {
                        auto parsed = URL::Parser::basic_parse(ak_string_from_qstring(text));
                        if (!parsed.has_value()) return;
                        auto& tab = m_window.new_tab_from_url(*parsed, Web::HTML::ActivateTab::No, BrowserWindow::TabLocation::end());
                        if (pinned) tab.set_pinned(true);
                    }, AK::move(completion));
                    if (result.is_error()) warnln("{}", "Import could not start");
                    return;
                }
                completion(Error::from_string_literal("Unknown import operation")); return;
            }
            if (type == "styles.status"sv) {
                auto* tab = m_window.current_tab();
                if (!tab || !tab->is_loaded()) {
                    completion(Error::from_string_literal("Откройте страницу для просмотра состояния стилей.")); return;
                }
                auto store = Application::the().derpbird_style_store();
                if (store.is_error()) { completion(store.release_error()); return; }
                auto revision = store.value()->revision();
                auto shell_document = m_shell->page().traversable().active_document().id();
                auto channel = m_shell->page().web_ui();
                auto top_document = tab->view().page().traversable().active_document().id();
                auto valid = [self = QPointer { this }, target = QPointer { tab }, view = QPointer { &tab->view() }, shell_document, channel, top_document, revision] {
                    if (!self || !target || !view || self->m_window.current_tab() != target || !target->is_loaded()
                        || &target->view() != view || view->crash_overlay_active()
                        || view->page().traversable().active_document().id() != top_document
                        || !self->m_shell->page().is_open() || self->m_shell->crash_overlay_active()
                        || self->m_shell->page().web_ui() != channel
                        || self->m_shell->page().traversable().active_document().id() != shell_document) return false;
                    auto current = Application::the().derpbird_style_store();
                    return !current.is_error() && current.value()->revision() == revision;
                };
                auto reply = std::make_shared<decltype(completion)>(AK::move(completion));
                tab->view().request_internal_page_info(WebView::PageInfoType::DerpBirdStyleStatus)
                    ->when_resolved([reply, valid, revision](auto const& text) {
                        if (!valid()) { (*reply)(Error::from_string_literal("Страница или стили изменились; запросите состояние заново.")); return; }
                        auto value = JsonValue::from_string(text);
                        if (value.is_error() || !value.value().is_object()) {
                            (*reply)(Error::from_string_literal("Состояние стилей пока недоступно.")); return;
                        }
                        auto status = value.release_value().as_object();
                        status.set("revision"sv, revision);
                        (*reply)(JsonValue(AK::move(status)));
                    })
                    .when_rejected([reply](auto const&) { (*reply)(Error::from_string_literal("Не удалось получить состояние стилей страницы.")); });
                return;
            }
            if (type == "scripts.folders"sv || type == "styles.folders"sv) {
                bool scripts = type == "scripts.folders"sv;
                if (m_window.is_private() == WebView::IsPrivate::Yes || m_source_scans.contains(scripts)) {
                    completion(Error::from_string_literal("Source directories require a regular window and no pending scan")); return;
                }
                auto path = QString::fromUtf8(Application::profile().paths().data.characters())
                    + (scripts ? "/DerpBirdScriptFolders.json" : "/DerpBirdStyleFolders.json");
                // Only the native picker/config mutation touches the GUI thread.
                if (data.as_object().get_string("action"sv) != "list"sv) {
                    auto configured = derpbird_source_folders(this, path, scripts, data.as_object(), false);
                    if (configured.is_error()) { completion(configured.release_error()); return; }
                }
                m_source_scans.insert(scripts);
                auto document = m_shell->page().traversable().active_document().id();
                auto channel = m_shell->page().web_ui();
                struct ScanResult { Optional<ErrorOr<JsonValue>> result; QStringList watches; };
                auto scan = std::make_shared<ScanResult>();
                auto* thread = QThread::create([scan, path, scripts] {
                    JsonObject request; request.set("action"sv, "list"sv);
                    scan->result = derpbird_source_folders(nullptr, path, scripts, request, true, &scan->watches);
                });
                connect(thread, &QThread::finished, this, [this, scan, scripts, document, channel, completion = AK::move(completion)]() mutable {
                    m_source_scans.remove(scripts);
                    if (!m_shell->page().is_open() || m_shell->page().web_ui() != channel
                        || m_shell->page().traversable().active_document().id() != document) {
                        completion(Error::from_string_literal("Source editor document expired")); return;
                    }
                    // The worker already enumerated all paths; avoid a second tree walk here.
                    m_source_watch_paths.insert(scripts, scan->watches);
                    QSet<QString> desired;
                    for (auto const& paths : m_source_watch_paths) for (auto const& path : paths) desired.insert(path);
                    auto current = m_source_watcher->directories() + m_source_watcher->files();
                    QSet<QString> existing(current.begin(), current.end());
                    auto removed = existing - desired;
                    auto added = desired - existing;
                    if (!removed.isEmpty()) m_source_watcher->removePaths(removed.values());
                    if (!added.isEmpty()) m_source_watcher->addPaths(added.values());
                    VERIFY(scan->result.has_value());
                    completion(scan->result.release_value());
                });
                connect(thread, &QThread::finished, thread, &QObject::deleteLater);
                thread->start();
                return;
            }
            if (type == "sources.cancel"sv) {
                auto token = data.as_object().get_string("downloadToken"sv);
                if (token.has_value() && *token == m_source_download_token && m_source_download)
                    m_source_download->cancel();
                completion(JsonValue {});
                return;
            }
            if (type == "scripts.fetch"sv || type == "styles.fetch"sv) {
                auto address = data.as_object().get_string("url"sv);
                auto token = data.as_object().get_string("downloadToken"sv);
                if (!token.has_value() || token->is_empty() || token->bytes().size() > 32 || !address.has_value() || address->bytes().size() > 8192 || m_source_download || m_window.is_private() == WebView::IsPrivate::Yes) {
                    completion(Error::from_string_literal("Source download requires a regular window and no pending download")); return;
                }
                auto document = m_shell->page().traversable().active_document().id();
                auto channel = m_shell->page().web_ui();
                m_source_download_token = *token;
                m_source_download = new DerpBirdSourceDownload(this, *address,
                    [self = QPointer { this }, document, channel, completion = AK::move(completion)](ErrorOr<JsonValue> result) mutable {
                        if (self) self->m_source_download = nullptr;
                        if (!self || !self->m_shell->page().is_open() || self->m_shell->page().web_ui() != channel
                            || self->m_shell->page().traversable().active_document().id() != document) {
                            completion(Error::from_string_literal("Source editor document expired")); return;
                        }
                        completion(AK::move(result));
                    });
                m_source_download->start();
                return;
            }
            if (type == "scripts.replace"sv) {
                if (m_window.is_private() == WebView::IsPrivate::Yes || m_script_install) {
                    completion(Error::from_string_literal("Edit userscripts from a regular window; wait for the current installation")); return;
                }
                auto revision = data.as_object().template get_integer<u64>("revision"sv);
                auto items = data.as_object().get_array("items"sv);
                if (!revision.has_value() || !items.has_value()) {
                    completion(Error::from_string_literal("Missing userscripts or revision")); return;
                }
                auto store = Application::the().derpbird_script_store();
                if (store.is_error()) { completion(store.release_error()); return; }
                auto prepared = store.value()->prepare_from_editor(*items, *revision);
                if (prepared.is_error()) { completion(prepared.release_error()); return; }
                auto document = m_shell->page().traversable().active_document().id();
                auto channel = m_shell->page().web_ui();
                m_script_install = new DerpBirdScriptInstall(this, prepared.release_value(),
                    [self = QPointer { this }, document, channel, revision = *revision, completion = AK::move(completion)](ErrorOr<JsonArray> result) mutable {
                        if (self) self->m_script_install = nullptr;
                        if (!self || !self->m_shell->page().is_open() || self->m_shell->crash_overlay_active()
                            || self->m_shell->page().web_ui() != channel
                            || self->m_shell->page().traversable().active_document().id() != document
                            || self->m_shell->url().serialize() != "about:derpbird"sv) {
                            completion(Error::from_string_literal("Userscript editor document expired")); return;
                        }
                        if (result.is_error()) { completion(result.release_error()); return; }
                        auto committed = [&]() -> ErrorOr<JsonValue> {
                            auto& store = *TRY(Application::the().derpbird_script_store());
                            TRY(store.replace(result.release_value(), revision));
                            TRY(Application::the().set_derpbird_user_scripts(JsonValue { store.items() }));
                            JsonObject snapshot;
                            snapshot.set("revision"sv, store.revision());
                            snapshot.set("items"sv, store.items());
                            return snapshot;
                        }();
                        completion(AK::move(committed));
                    }, QString::fromUtf8(Application::profile().paths().data.characters()) + "/DerpBirdScriptFolders.json");
                m_script_install->start();
                return;
            }
            if (type == "credentials.autofill-dismiss"sv) { clear_autofill(); completion(JsonValue {}); return; }
            if (type == "credentials.autofill"sv) {
                auto token = data.as_object().get_string("token"sv);
                auto id = data.as_object().get_string("id"sv);
                if (!token.has_value() || !id.has_value() || *token != String::number(m_autofill_generation)
                    || !m_autofill_valid || !m_autofill_valid() || !m_credential_autofill.is_object()) {
                    completion(Error::from_string_literal("Login suggestion expired; focus the field again")); return;
                }
                auto items = m_credential_autofill.as_object().get_array("items"sv);
                if (items.has_value()) for (auto const& item : items->values()) {
                    if (!item.is_object() || item.as_object().get_string("id"sv) != id) continue;
                    JsonObject request = item.as_object(); request.set("type"sv, "credentials.fill"sv);
                    m_credential_autofill = JsonValue {}; queue_snapshot();
                    m_shell->on_derpbird_request(JsonValue { AK::move(request) },
                        [self = QPointer(this), completion = AK::move(completion)](auto result) mutable {
                            if (self && !result.is_error() && self->m_autofill_view) self->m_autofill_view->setFocus();
                            if (self) self->clear_autofill();
                            completion(AK::move(result));
                        }); return;
                }
                completion(Error::from_string_literal("Login suggestion unavailable")); return;
            }
            if (type == "credentials.capture-save"sv || type == "credentials.capture-cancel"sv || type == "credentials.capture-never"sv) {
                auto token = data.as_object().get_string("token"sv);
                if (!token.has_value() || token->bytes().size() > 64 || m_window.is_private() == WebView::IsPrivate::Yes) {
                    completion(Error::from_string_literal("Invalid credential proposal"));
                    return;
                }
                if (type == "credentials.capture-cancel"sv) {
                    m_credential_proposal.cancel(*token);
                    completion(JsonValue {});
                    return;
                }
                if (type == "credentials.capture-never"sv) {
                    auto metadata = m_credential_proposal.metadata();
                    if (!metadata.is_object() || metadata.as_object().get_string("token"sv) != *token) {
                        completion(Error::from_string_literal("Credential proposal expired")); return;
                    }
                    JsonObject policy;
                    policy.set("type"sv, "credentials.block-origin"sv);
                    policy.set("origin"sv, metadata.as_object().get_string("origin"sv).value());
                    policy.set("id"sv, ""sv); policy.set("revision"sv, "0"sv);
                    Application::the().derpbird_credentials_request(JsonValue { AK::move(policy) }, false,
                        [self = QPointer(this), token = *token, completion = AK::move(completion)](ErrorOr<JsonValue> result) mutable {
                            if (!result.is_error() && self) self->m_credential_proposal.cancel(token);
                            completion(AK::move(result));
                        });
                    return;
                }
                auto id = data.as_object().get_string("id"sv);
                auto revision = data.as_object().get_string("revision"sv);
                if (!id.has_value() || !revision.has_value()) { completion(Error::from_string_literal("Missing credential update identity")); return; }
                auto payload = m_credential_proposal.take(*token, *id, *revision);
                if (payload.is_error()) { completion(payload.release_error()); return; }
                Application::the().derpbird_credentials_request(payload.release_value(), false, AK::move(completion));
                return;
            }
            if (type == "credentials.fill"sv || type == "credentials.capture"sv) {
                auto* tab = m_window.current_tab();
                if (m_window.is_private() == WebView::IsPrivate::Yes || !tab || !tab->is_loaded() || m_credential_fill) {
                    completion(Error::from_string_literal("Откройте обычную вкладку и выберите поле пароля."));
                    return;
                }
                auto shell_document = m_shell->page().traversable().active_document().id();
                auto channel = m_shell->page().web_ui();
                auto top_document = tab->view().page().traversable().active_document().id();
                auto valid = [self = QPointer { this }, target = QPointer { tab }, view = QPointer { &tab->view() }, shell_document, channel, top_document] {
                    return self && target && view && self->m_window.current_tab() == target && target->is_loaded()
                        && &target->view() == view && !view->crash_overlay_active()
                        && view->page().traversable().active_document().id() == top_document
                        && self->m_shell->page().is_open() && !self->m_shell->crash_overlay_active()
                        && self->m_shell->page().web_ui() == channel
                        && self->m_shell->page().traversable().active_document().id() == shell_document
                        && self->m_shell->url().serialize() == "about:derpbird"sv;
                };
                bool capture = type == "credentials.capture"sv;
                if (capture) {
                    m_submitted_credential_token = {};
                    m_credential_proposal.clear();
                    auto finish_capture = [self = QPointer { this }, authority = valid, completion = AK::move(completion)](ErrorOr<JsonValue> result) mutable {
                        if (!self || !authority()) { completion(Error::from_string_literal("Credential capture document expired")); return; }
                        if (result.is_error()) { completion(result.release_error()); return; }
                        auto value = result.release_value();
                        if (!value.is_object()) { completion(Error::from_string_literal("Invalid credential capture")); return; }
                        auto origin = value.as_object().get_string("origin"sv);
                        auto username = value.as_object().get_string("username"sv);
                        auto password = value.as_object().get_string("password"sv);
                        if (!origin.has_value() || !username.has_value() || !password.has_value()) { completion(Error::from_string_literal("Invalid credential capture")); return; }
                        if (!self->m_credential_fill) { completion(Error::from_string_literal("Credential capture expired")); return; }
                        auto document_authority = self->m_credential_fill->document_authority();
                        completion(self->m_credential_proposal.propose(*origin, *username, *password,
                            [authority = AK::move(authority), document_authority = AK::move(document_authority)] { return authority() && document_authority(); }));
                    };
                    m_credential_fill = new DerpBirdCredentialFill(this, tab->view(), data, AK::move(valid), AK::move(finish_capture), DerpBirdCredentialFill::Mode::Capture);
                } else {
                    m_credential_fill = new DerpBirdCredentialFill(this, tab->view(), data, AK::move(valid), AK::move(completion));
                }
                m_credential_fill->start();
                return;
            }
            if (type == "credentials.for-origin"sv || type == "credentials.for-fill"sv || type == "credentials.matches"sv || type == "credentials.block-origin"sv) {
                completion(Error::from_string_literal("Internal credential operation"));
                return;
            }
            if (type == "credentials.copy"sv) {
                auto document = m_shell->page().traversable().active_document().id();
                auto channel = m_shell->page().web_ui();
                auto ticket = m_secret_clipboard.ticket();
                auto payload = data;
                payload.as_object().set("type"sv, "credentials.reveal"sv);
                Application::the().derpbird_credentials_request(payload, m_window.is_private() == WebView::IsPrivate::Yes,
                    [self = QPointer { this }, document, channel, ticket, completion = AK::move(completion)](ErrorOr<JsonValue> result) mutable {
                        if (!self || !self->m_shell->page().is_open() || self->m_shell->crash_overlay_active()
                            || self->m_shell->page().web_ui() != channel
                            || self->m_shell->page().traversable().active_document().id() != document
                            || self->m_shell->url().serialize() != "about:derpbird"sv) {
                            completion(Error::from_string_literal("Password copy document expired"));
                            return;
                        }
                        if (result.is_error()) { completion(result.release_error()); return; }
                        auto value = result.release_value();
                        auto password = value.is_object() ? value.as_object().get_string("password"sv) : Optional<String> {};
                        if (!password.has_value() || !self->m_secret_clipboard.publish(qstring_from_ak_string(*password), ticket)) {
                            completion(Error::from_string_literal("Clipboard changed or could not accept password; copy again"));
                            return;
                        }
                        // Do not send the copied password into the web UI.
                        completion(JsonValue {});
                    });
                return;
            }
            if (type.has_value() && type->bytes_as_string_view().starts_with("credentials."sv)) {
                Application::the().derpbird_credentials_request(data, m_window.is_private() == WebView::IsPrivate::Yes, AK::move(completion));
                return;
            }
        }
        completion(request(data));
    };
    m_shell->on_load_start = [this] {
        m_file_picker->cancel();
        if (m_source_download) m_source_download->cancel();
        m_credential_proposal.clear();
        m_pending_surface_state.clear();
        m_surface_state.invalidate();
        m_content->hide();
    };
    connect(m_shell, &WebContentView::crash_overlay_changed, this, [this] {
        if (m_shell->crash_overlay_active()) {
            m_file_picker->dismiss();
            if (m_source_download) m_source_download->cancel();
            m_pending_surface_state.clear();
            m_surface_state.invalidate();
            m_content->hide();
        }
    });
    m_shell->on_load_finish = [this](auto const&) { queue_snapshot(); };
    m_content = window.tab_widget().detach_page_stack(this);
    m_content->hide();
    connect(&window.tab_widget(), &TabWidget::current_tab_changed, this,
        [this] { clear_autofill(); if (m_credential_fill) m_credential_fill->cancel(); m_credential_proposal.clear(); queue_snapshot(); });
    connect(window.tab_widget().tab_bar(), &QTabBar::tabMoved, this,
        [this] { queue_snapshot(); });
    for (int i = 0; i < window.tab_count(); ++i)
        observe_tab(*window.tab_at(i));
    m_shell->load(URL::Parser::basic_parse("about:derpbird"sv).release_value());
}

DerpBirdChrome::~DerpBirdChrome()
{
    if (m_request_log_view) m_request_log_view->set_request_logging(false);
    if (m_autocomplete) { m_autocomplete->on_autocomplete_query_complete = nullptr; m_autocomplete->cancel_pending_query(); }
    if (m_source_download) delete m_source_download.data();
    m_credential_proposal.on_change = {};
    if (m_credential_fill) delete m_credential_fill.data();
    if (m_script_install) delete m_script_install.data();
    m_shell->on_derpbird_request = nullptr;
    while (!m_observed_tabs.isEmpty())
        release_observation(m_observed_tabs.begin().key());
}

void DerpBirdChrome::observe_tab(Tab& tab)
{
    if (m_observed_tabs.contains(&tab))
        return;
    if (tab.session_tab_id().has_value()) {
        m_tree_tabs.insert(*tab.session_tab_id(), &tab);
        auto parent = m_window.session().session_store->tab_tree_parent(*tab.session_tab_id());
        if (!parent.is_error()) tab.setProperty("derpbirdTreeParent", static_cast<qlonglong>(parent.value().value_or(0)));
    }
    auto* scope = new QObject(this);
    m_observed_tabs.insert(&tab, Observation { .scope = scope });
    if (tab.property("derpbirdId").isNull())
        tab.setProperty("derpbirdId",
            QUuid::createUuid().toString(QUuid::WithoutBraces));
    connect(&tab, &Tab::title_changed, scope, [this] { queue_snapshot(); });
    connect(&tab, &Tab::favicon_changed, scope, [this] { queue_snapshot(); });
    connect(&tab, &Tab::loading_changed, scope, [this] { queue_snapshot(); });
    connect(&tab, &Tab::playing_audio_indicator_changed, scope, [this] { queue_snapshot(); });
    connect(&tab, &QObject::destroyed, scope, [this, pointer = &tab, session_id = tab.session_tab_id()] {
        if (session_id.has_value()) m_tree_tabs.remove(*session_id);
        // Tab's derived lifetime has ended. Only release the stored observation;
        // do not call any Tab methods from QObject::destroyed.
        release_observation(pointer);
        queue_snapshot();
    });
    auto observe_view = [this, &tab, scope] {
        auto& observation = m_observed_tabs[&tab];
        observation.view = &tab.view();
        tab.view().installEventFilter(this);
        tab.view().on_derpbird_credential_focus = [self = QPointer(this), target = QPointer(&tab), view = QPointer(&tab.view())](auto& document, bool available, Gfx::IntRect bounds) {
            if (!self || !target || !view || self->m_window.current_tab() != target) return;
            self->clear_autofill();
            if (!available || self->m_credential_fill || self->m_window.is_private() == WebView::IsPrivate::Yes || !target->is_loaded()) return;
            self->m_autofill_anchor = QRect(bounds.x(), bounds.y(), bounds.width(), bounds.height());
            auto generation = self->m_autofill_generation;
            auto document_id = document.id();
            auto page = document.host();
            auto origin = document.origin().serialize();
            auto valid = [self, target, view, page, document_id, origin, generation] {
                if (!self || !target || !view || !page || !page->is_open() || self->m_window.current_tab() != target
                    || self->m_autofill_generation != generation || !target->is_loaded() || view->crash_overlay_active()) return false;
                auto document = page->hosted_document_with_id(document_id);
                return document && !document->browsing_context().has_been_discarded()
                    && document->browsing_context().active_document().ptr() == document.ptr()
                    && document->origin().serialize() == origin;
            };
            self->m_autofill_valid = [valid] { return valid(); }; self->m_autofill_view = view;
            JsonObject request; request.set("type"sv, "credentials.for-origin"sv); request.set("origin"sv, origin);
            request.set("id"sv, ""sv); request.set("revision"sv, "0"sv);
            Application::the().derpbird_credentials_request(JsonValue { AK::move(request) }, false,
                [self, valid, generation, origin](ErrorOr<JsonValue> result) mutable {
                    if (!valid() || result.is_error()) return;
                    auto value = result.release_value();
                    auto items = value.is_object() ? value.as_object().get_array("items"sv) : Optional<JsonArray const&> {};
                    if (!items.has_value() || items->is_empty()) return;
                    JsonObject suggestion; suggestion.set("token"sv, String::number(generation)); suggestion.set("origin"sv, origin);
                    suggestion.set("items"sv, *items);
                    self->m_credential_autofill = JsonValue { AK::move(suggestion) }; self->queue_snapshot();
                });
        };
        tab.view().on_derpbird_credential_submission = [self = QPointer(this), target = QPointer(&tab), view = QPointer(&tab.view())](auto& document, String username, String password) {
            if (!self || !target || !view || self->m_credential_fill
                || self->m_window.is_private() == WebView::IsPrivate::Yes
                || self->m_window.current_tab() != target || !target->is_loaded()) return;
            auto shell_document = self->m_shell->page().traversable().active_document().id();
            auto channel = self->m_shell->page().web_ui();
            auto authority = [self, target, view, shell_document, channel] {
                // The source was validated by WebContentPage at receipt. Login
                // navigation may replace it; keep its captured origin immutable.
                return self && target && view && self->m_window.current_tab() == target
                    && self->m_window.is_private() == WebView::IsPrivate::No
                    && target->is_loaded() && &target->view() == view
                    && view->page().is_open() && !view->crash_overlay_active()
                    && self->m_shell->page().is_open() && !self->m_shell->crash_overlay_active()
                    && self->m_shell->page().web_ui() == channel
                    && self->m_shell->page().traversable().active_document().id() == shell_document
                    && self->m_shell->url().serialize() == "about:derpbird"sv;
            };
            auto proposal = self->m_credential_proposal.propose(document.origin().serialize(), AK::move(username), password, AK::move(authority));
            if (proposal.is_error()) return;
            auto token = proposal.value().as_object().get_string("token"sv).value();
            self->m_submitted_credential_token = {};
            JsonObject comparison;
            comparison.set("type"sv, "credentials.matches"sv);
            comparison.set("id"sv, ""sv);
            comparison.set("revision"sv, "0"sv);
            comparison.set("origin"sv, proposal.value().as_object().get_string("origin"sv).value());
            comparison.set("username"sv, proposal.value().as_object().get_string("username"sv).value());
            comparison.set("password"sv, AK::move(password));
            Application::the().derpbird_credentials_request(JsonValue { AK::move(comparison) }, false,
                [self, token](ErrorOr<JsonValue> result) {
                    if (!self) return;
                    auto current = self->m_credential_proposal.metadata();
                    // A tab change, newer submission, manual capture or expiry
                    // can revoke this proposal while the keyring is responding.
                    if (!current.is_object() || current.as_object().get_string("token"sv) != token) return;
                    if (!result.is_error() && result.value().is_object()
                        && (result.value().as_object().get_bool("matches"sv).value_or(false)
                            || result.value().as_object().get_bool("blocked"sv).value_or(false))) {
                        self->m_credential_proposal.cancel(token);
                        return;
                    }
                    // Provider failure is not proof that a credential matches.
                    // Preserve explicit save choice; never write automatically.
                    self->m_submitted_credential_token = token;
                    self->queue_snapshot();
                });
        };
        connect(&tab.location_edit(), &QLineEdit::textChanged, scope, [this] { queue_snapshot(); });
        connect(&tab.view(), &WebContentView::native_surface_changed, scope, [this] { place_content(); });
        auto back = make<NavigationObserver>(*this);
        observation.back = back.ptr();
        tab.view().navigate_back_action().add_observer(AK::move(back));
        auto forward = make<NavigationObserver>(*this);
        observation.forward = forward.ptr();
        tab.view().navigate_forward_action().add_observer(AK::move(forward));
        WebView::ViewImplementation::NavigationListener listener;
        listener.on_load_start = [self = QPointer(this), view = QPointer(&tab.view())](auto const&) { if (self && self->m_autofill_view == view) self->clear_autofill(); };
        listener.on_load_finish = [self = QPointer(this)](auto const&) {
            if (self)
                self->queue_snapshot();
        };
        observation.navigation_listener = tab.view().add_navigation_listener(AK::move(listener));
        place_content();
    };
    if (tab.is_loaded())
        observe_view();
    else
        connect(&tab, &Tab::loaded, scope, observe_view, Qt::SingleShotConnection);
    queue_snapshot();
}

void DerpBirdChrome::release_observation(Tab* tab)
{
    m_favicon_cache.remove(tab);
    auto found = m_observed_tabs.find(tab);
    if (found == m_observed_tabs.end())
        return;
    auto observation = found.value();
    m_observed_tabs.erase(found);
    delete observation.scope;
    if (auto view = observation.view) {
        view->on_derpbird_credential_focus = nullptr;
        view->removeEventFilter(this);
        view->on_derpbird_credential_submission = nullptr;
        if (observation.back)
            view->navigate_back_action().remove_observer(*observation.back);
        if (observation.forward)
            view->navigate_forward_action().remove_observer(*observation.forward);
        if (observation.navigation_listener.has_value())
            view->remove_navigation_listener(*observation.navigation_listener);
    }
}

void DerpBirdChrome::unobserve_tab(Tab& tab)
{
    if (tab.session_tab_id().has_value()) m_tree_tabs.remove(*tab.session_tab_id());
    m_favicon_cache.remove(&tab);
    release_observation(&tab);
    queue_snapshot();
}

Tab* DerpBirdChrome::tree_parent(Tab& tab) const
{
    auto id = tab.property("derpbirdTreeParent").toLongLong();
    if (!id || tab.is_pinned()) return nullptr;
    auto* candidate = m_tree_tabs.value(id).data();
    if (candidate && candidate != &tab && !candidate->is_pinned()
        && !candidate->property("derpbirdTreeParent").toLongLong()) return candidate;
    return nullptr;
}

ErrorOr<void> DerpBirdChrome::set_tree_parent(Tab& tab, Tab* parent)
{
    if (!tab.session_tab_id().has_value() || (parent && !parent->session_tab_id().has_value()))
        return Error::from_string_literal("Tab session is unavailable");
    Optional<WebView::SessionTabId> id = parent ? parent->session_tab_id() : Optional<WebView::SessionTabId> {};
    TRY(m_window.session().session_store->set_tab_tree_parent(*tab.session_tab_id(), id));
    tab.setProperty("derpbirdTreeParent", static_cast<qlonglong>(id.value_or(0)));
    queue_snapshot();
    return {};
}

ErrorOr<void> DerpBirdChrome::detach_tree_tab(Tab& tab)
{
    // Every close path (including Ctrl+W, script close and native menus) promotes children.
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto* child = m_window.tab_at(i);
        if (tree_parent(*child) == &tab) TRY(set_tree_parent(*child, nullptr));
    }
    if (tab.property("derpbirdTreeParent").toLongLong()) TRY(set_tree_parent(tab, nullptr));
    return {};
}

void DerpBirdChrome::normalize_tree_order()
{
    if (m_reordering_tree) return;
    QVector<Tab*> ordered;
    QHash<Tab*, QVector<Tab*>> children;
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto* tab = m_window.tab_at(i);
        if (auto* parent = tree_parent(*tab)) children[parent].append(tab);
    }
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto* tab = m_window.tab_at(i);
        if (tree_parent(*tab)) continue;
        ordered.append(tab);
        ordered.append(children.value(tab));
    }
    m_reordering_tree = true;
    for (int i = 0; i < ordered.size(); ++i)
        if (m_window.tab_at(i) != ordered[i]) m_window.move_tab(m_window.tab_index(ordered[i]), i);
    m_reordering_tree = false;
}

ErrorOr<void> DerpBirdChrome::move_tree_tab(Tab& source, Tab& target, StringView placement)
{
    if (&source == &target) return {};
    if (!placement.is_one_of("before"sv, "after"sv, "inside"sv)) return Error::from_string_literal("Invalid drop position");
    if (source.is_pinned() != target.is_pinned()) return Error::from_string_literal("Unpin the tab before grouping it");
    QVector<Tab*> block { &source };
    QVector<Tab*> remaining;
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto* tab = m_window.tab_at(i);
        if (tree_parent(*tab) == &source) block.append(tab);
        else if (tab != &source) remaining.append(tab);
    }
    if (block.contains(&target)) return Error::from_string_literal("A group cannot be moved into itself");
    auto* parent = tree_parent(target);
    if (placement == "inside"sv) parent = parent ? parent : &target;
    if (parent && (source.is_pinned() || block.size() > 1)) return Error::from_string_literal("Tab groups support two levels only");
    int destination = remaining.indexOf(&target);
    if (placement == "inside"sv) {
        destination = remaining.indexOf(parent) + 1;
        while (destination < remaining.size() && tree_parent(*remaining[destination]) == parent) ++destination;
    } else if (placement == "after"sv) {
        ++destination;
        if (!parent) while (destination < remaining.size() && tree_parent(*remaining[destination]) == &target) ++destination;
    }
    if (parent && parent->property("derpbirdTreeParent").toLongLong()) TRY(set_tree_parent(*parent, nullptr));
    TRY(set_tree_parent(source, parent));
    for (int i = 0; i < block.size(); ++i) remaining.insert(destination + i, block[i]);
    m_reordering_tree = true;
    for (int i = 0; i < remaining.size(); ++i)
        if (m_window.tab_at(i) != remaining[i]) m_window.move_tab(m_window.tab_index(remaining[i]), i);
    m_reordering_tree = false;
    queue_snapshot();
    return {};
}

JsonValue DerpBirdChrome::snapshot()
{
    normalize_tree_order();
    JsonArray tabs;
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto& tab = *m_window.tab_at(i);
        JsonObject entry;
        entry.set("id"sv, tab_id(tab));
        entry.set("url"sv, tab.url().serialize());
        entry.set("title"sv, ak_string_from_qstring(tab.title()));
        auto icon_key = tab.favicon().cacheKey();
        auto cached = m_favicon_cache.find(&tab);
        if (cached == m_favicon_cache.end() || cached->first != icon_key) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (!tab.favicon().isNull()) tab.favicon().pixmap(24, 24).save(&buffer, "PNG");
            auto value = png.isEmpty() ? String {} : ak_string_from_qstring("data:image/png;base64," + QString::fromLatin1(png.toBase64()));
            m_favicon_cache.insert(&tab, { icon_key, AK::move(value) });
        }
        entry.set("favicon"sv, m_favicon_cache.value(&tab).second);
        entry.set("loading"sv, tab.is_loading());
        entry.set("canGoBack"sv,
            tab.is_loaded() && tab.view().navigate_back_action().enabled());
        entry.set("canGoForward"sv,
            tab.is_loaded() && tab.view().navigate_forward_action().enabled());
        entry.set("zoom"sv, tab.is_loaded() ? tab.view().zoom_level() : 1.0);
        entry.set("pinned"sv, tab.is_pinned());
        if (auto* parent = tree_parent(tab)) entry.set("parentId"sv, tab_id(*parent));
        entry.set("attention"sv, tab.needs_attention());
        entry.set("audible"sv, tab.is_loaded() && tab.view().is_playing_audio());
        entry.set("muted"sv, tab.is_loaded() && tab.view().page_mute_state() == Web::HTML::MuteState::Muted);
        entry.set("private"sv, m_window.is_private() == WebView::IsPrivate::Yes);
        tabs.must_append(AK::move(entry));
    }
    JsonObject state;
    state.set("addressSuggestions"sv, m_address_suggestions);
    StringBuilder dynamic_rules;
    for (auto line : WebView::Application::settings().custom_content_blocker_filters().bytes_as_string_view().split_view('\n')) {
        if (line.starts_with("!#derpbird-dynamic "sv)) { dynamic_rules.append(line.substring_view(19)); dynamic_rules.append('\n'); }
    }
    state.set("dynamicRules"sv, MUST(dynamic_rules.to_string()));
    // Snapshots also occur while the picker is waiting for a click/selector.
    // Only invalidate an existing proposal, not the pending picker authority.
    if (m_filter_proposal.is_object()) {
        if (m_filter_tab && m_filter_tab->url().serialize() == m_filter_proposal.as_object().get_string("url"sv))
            state.set("filterProposal"sv, m_filter_proposal);
        else { m_filter_proposal = JsonValue {}; m_filter_tab = nullptr; }
    }
    if (m_credential_autofill.is_object()) {
        if (!m_autofill_valid || !m_autofill_valid() || !m_autofill_view) {
            m_credential_autofill = JsonValue {};
        } else {
            double scale = m_autofill_view->devicePixelRatioF();
            auto position = m_shell->mapFromGlobal(m_autofill_view->mapToGlobal(QPoint(
                m_autofill_anchor.x() / scale, (m_autofill_anchor.y() + m_autofill_anchor.height()) / scale + 4)));
            m_credential_autofill.as_object().set("x"sv, position.x());
            m_credential_autofill.as_object().set("y"sv, position.y());
        }
    }
    state.set("credentialAutofill"sv, m_credential_autofill);
    auto proposal = m_credential_proposal.metadata();
    if (!proposal.is_object() || proposal.as_object().get_string("token"sv) != m_submitted_credential_token)
        proposal = JsonValue {};
    state.set("credentialProposal"sv, AK::move(proposal));
    auto* active = m_window.current_tab();
    state.set("scriptCommands"sv, active && active->is_loaded() ? Application::the().derpbird_script_commands(active->view().page()) : JsonArray {});
    auto* logged_view = m_blocking_log_visible && active && active->is_loaded() ? &active->view() : nullptr;
    if (m_request_log_view.ptr() != logged_view) {
        if (m_request_log_view) m_request_log_view->set_request_logging(false);
        m_request_log_view = logged_view ? logged_view->make_weak_ptr<WebView::ViewImplementation>() : WeakPtr<WebView::ViewImplementation> {};
        if (m_request_log_view) m_request_log_view->set_request_logging(true);
    }
    if (m_blocking_log_visible && active && active->is_loaded()) {
        JsonObject log;
        JsonArray entries;
        for (auto const& entry : active->view().blocking_log()) {
            JsonObject item;
            item.set("id"sv, entry.sequence);
            item.set("url"sv, entry.url);
            item.set("kind"sv, entry.kind);
            item.set("rule"sv, entry.rule);
            entries.must_append(AK::move(item));
        }
        log.set("tabId"sv, tab_id(*active));
        log.set("entries"sv, AK::move(entries));
        state.set("blockingLog"sv, AK::move(log));
    }
    auto const& proxy_profile = Application::request_server_options().proxy_configuration.active_profile();
    state.set("proxyProfile"sv, proxy_profile.is_empty() ? "system"_string : proxy_profile);
    state.set("version"sv, 1);
    state.set("revision"sv, ++m_revision);
    JsonObject preferences;
    auto const& settings = Application::settings().tab_settings();
    preferences.set("layout"sv, settings.vertical_tabs_enabled ? "vertical"sv : "horizontal"sv);
    preferences.set("sidebarWidth"sv, clamp<int>(settings.vertical_tabs_expanded_width.value_or(196), 120, 600));
    preferences.set("wrapTabs"sv, m_shell_preferences->value("wrapTabs", false).toBool());
    state.set("preferences"sv, AK::move(preferences));
    state.set("tabs"sv, AK::move(tabs));
    state.set("activeTabId"sv, m_window.current_tab() ? JsonValue(tab_id(*m_window.current_tab())) : JsonValue {});
    return state;
}

void DerpBirdChrome::queue_snapshot()
{
    if (m_snapshot_queued)
        return;
    m_snapshot_queued = true;
    QTimer::singleShot(0, this, [this] {
        m_snapshot_queued = false;
        if (auto channel = m_shell->page().web_ui();
            channel && channel->host() == "derpbird"sv)
            channel->async_send_message("derpbird.snapshot"sv, snapshot());
    });
}

void DerpBirdChrome::clear_autofill()
{
    ++m_autofill_generation; m_autofill_valid = {}; m_autofill_view = nullptr;
    if (!m_credential_autofill.is_null()) { m_credential_autofill = JsonValue {}; queue_snapshot(); }
}

bool DerpBirdChrome::eventFilter(QObject* watched, QEvent* event)
{
    if (m_autofill_view && watched == m_autofill_view && m_credential_autofill.is_object()) {
        if (event->type() == QEvent::Wheel || event->type() == QEvent::MouseButtonPress) clear_autofill();
        else if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) { clear_autofill(); return true; }
            if (key->key() == Qt::Key_Down) {
                m_shell->setFocus();
                m_shell->run_javascript("document.querySelector('.credential-autofill button[data-credential-id]')?.focus()"_string);
                return true;
            }
        }
    }
    if (watched == m_shell && event->type() == QEvent::FocusOut && m_shell->page().is_open()) {
        // Queue the DOM blur before WebContentView defers its system-focus update.
        // Once system focus is gone, HTML unfocusing steps see an empty focus chain.
        m_shell->run_javascript("document.getElementById('location')?.blur()"_string);
    }
    return QWidget::eventFilter(watched, event);
}

void DerpBirdChrome::focus_address()
{
    clear_autofill();
    m_shell->setFocus();
    // This constant program only runs in the dedicated shell view.
    m_shell->run_javascript(
        "document.getElementById('location')?.select()"_string);
}

ErrorOr<JsonValue> DerpBirdChrome::request(JsonValue const& data)
{
    if (!data.is_object())
        return Error::from_string_literal("Invalid request");
    auto const& object = data.as_object();
    auto type = object.get_string("type"sv);
    if (type == "snapshot"sv)
        return snapshot();
    if (type == "styles.list"sv || type == "styles.replace"sv) {
        auto& store = *TRY(Application::the().derpbird_style_store());
        if (type == "styles.replace"sv) {
            if (m_window.is_private() == WebView::IsPrivate::Yes)
                return Error::from_string_literal("Edit styles from a regular browser window");
            auto revision = object.get_integer<u64>("revision"sv);
            auto items = object.get_array("items"sv);
            if (!revision.has_value() || *revision != store.revision())
                return Error::from_string_literal("Styles changed in another window; reload before saving");
            if (!items.has_value())
                return Error::from_string_literal("Missing user styles");
            TRY(store.replace(*items));
            TRY(Application::the().set_derpbird_user_style_sections(store.enabled_sections()));
        }
        JsonObject result;
        result.set("revision"sv, store.revision());
        result.set("items"sv, store.items());
        return result;
    }
    if (type == "scripts.list"sv || type == "scripts.file-access"sv) {
        auto& store = *TRY(Application::the().derpbird_script_store());
        if (type == "scripts.file-access"sv) {
            if (m_window.is_private() == WebView::IsPrivate::Yes || m_script_install)
                return Error::from_string_literal("Change script permissions in a regular window after installation finishes");
            auto id = object.get_string("id"sv);
            auto granted = object.get_bool("granted"sv);
            auto revision = object.get_integer<u64>("revision"sv);
            if (!id.has_value() || !granted.has_value() || !revision.has_value())
                return Error::from_string_literal("Missing script permission or revision");
            TRY(store.set_file_access(*id, *granted, *revision));
            TRY(Application::the().set_derpbird_user_scripts(JsonValue { store.items() }));
        }
        JsonObject result;
        result.set("revision"sv, store.revision());
        result.set("items"sv, store.items());
        return result;
    }
    if (type != "command"sv)
        return Error::from_string_literal("Unknown request");
    auto payload = object.get("command"sv);
    if (!payload.has_value())
        return Error::from_string_literal("Missing command");
    TRY(command(*payload));
    if (payload->as_object().get_string("type"sv) != "shell.viewport"sv)
        queue_snapshot();
    return JsonValue {};
}

static ErrorOr<QRect> decode_rect(JsonValue const& value)
{
    if (!value.is_object())
        return Error::from_string_literal("Invalid rectangle");
    auto const& rect = value.as_object();
    auto x = rect.get_integer<int>("x"sv);
    auto y = rect.get_integer<int>("y"sv);
    auto width = rect.get_integer<int>("width"sv);
    auto height = rect.get_integer<int>("height"sv);
    if (!x.has_value() || !y.has_value() || !width.has_value() || !height.has_value() || *x < 0 || *y < 0 || *width < 0 || *height < 0 || *x > 65536 || *y > 65536 || *width > 65536 || *height > 65536)
        return Error::from_string_literal("Invalid rectangle bounds");
    return QRect(*x, *y, *width, *height);
}

ErrorOr<void> DerpBirdChrome::command(JsonValue const& value)
{
    if (!value.is_object())
        return Error::from_string_literal("Invalid command");
    auto const& data = value.as_object();
    auto type = data.get_string("type"sv).value_or(String {});
    if (type == "blocking.log"sv) {
        auto visible = data.get_bool("visible"sv);
        if (!visible.has_value()) return Error::from_string_literal("Missing log visibility");
        m_blocking_log_visible = *visible;
        return {};
    }
    if (type == "shell.set-layout"sv) {
        auto layout = data.get_string("layout"sv);
        if (!layout.has_value() || !layout->is_one_of("vertical"sv, "horizontal"sv))
            return Error::from_string_literal("Invalid tab layout");
        auto settings = Application::settings().tab_settings();
        settings.vertical_tabs_enabled = *layout == "vertical"sv;
        Application::settings().set_tab_settings(settings);
        return {};
    }
    if (type == "shell.set-sidebar-width"sv) {
        auto width = data.get_integer<int>("width"sv);
        if (!width.has_value() || *width < 120 || *width > 600) return Error::from_string_literal("Invalid sidebar width");
        auto settings = Application::settings().tab_settings();
        settings.vertical_tabs_expanded_width = static_cast<u16>(*width);
        Application::settings().set_tab_settings(settings);
        return {};
    }
    if (type == "shell.set-wrap-tabs"sv) {
        auto value = data.get_bool("value"sv);
        if (!value.has_value()) return Error::from_string_literal("Invalid tab wrapping setting");
        m_shell_preferences->setValue("wrapTabs", *value);
        m_shell_preferences->sync();
        if (m_shell_preferences->status() != QSettings::NoError) return Error::from_string_literal("Cannot save tab wrapping setting");
        // Notify all browser windows through the existing preference observer.
        Application::settings().set_tab_settings(Application::settings().tab_settings());
        return {};
    }
    if (type == "shell.viewport"sv) {
        auto rect = data.get("rect"sv);
        auto overlays = data.get_array("overlays"sv);
        if (!rect.has_value() || !overlays.has_value() || overlays->size() > 8)
            return Error::from_string_literal("Invalid viewport");
        auto viewport = TRY(decode_rect(*rect));
        QVector<QRect> exclusions;
        for (auto const& overlay : overlays->values())
            exclusions.append(TRY(decode_rect(overlay)));
        ContentSurfaceState pending;
        pending.accept(viewport, AK::move(exclusions));
        m_pending_surface_state = AK::move(pending);
        return {};
    }
    if (type == "browser.open-page"sv) {
        auto name = data.get_string("page"sv);
        if (!name.has_value() || !name->is_one_of("history"sv, "downloads"sv, "bookmarks"sv, "settings"sv))
            return Error::from_string_literal("Unknown browser page");
        auto url = URL::Parser::basic_parse(TRY(String::formatted("about:{}", *name))).release_value();
        // Reuse a page only within this window's session, including private mode.
        if (!m_window.activate_tab_with_url(url, URL::ExcludeFragment::Yes))
            m_window.new_tab_from_url(url, Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
        return {};
    }
    if (type == "tab.create"sv || type == "window.new"sv) {
        auto url = data.get_string("url"sv);
        auto is_private = data.get_bool("private"sv);
        if ((data.has("url"sv) && !url.has_value()) || !is_private.has_value())
            return Error::from_string_literal("Invalid new tab");
        auto parsed = url.has_value() ? URL::Parser::basic_parse(*url) : Optional<URL::URL> { Application::settings().new_tab_page_url() };
        if (!parsed.has_value() || !is_public_navigation_url(*parsed))
            return Error::from_string_literal("Unsupported navigation URL");
        if (type == "window.new"sv || *is_private != (m_window.is_private() == WebView::IsPrivate::Yes)) {
            Application::the().new_window(
                { *parsed }, {}, BrowserWindow::IsPopupWindow::No,
                *is_private ? WebView::IsPrivate::Yes : WebView::IsPrivate::No);
        } else {
            m_window.new_tab_from_url(*parsed, Web::HTML::ActivateTab::Yes,
                BrowserWindow::TabLocation::end());
        }
        return {};
    }
    if (type == "tab.reopen"sv) {
        Application::the().reopen_recently_closed_tab(&m_window);
        return {};
    }
    if (type == "window.minimize"sv) {
        m_window.showMinimized();
        return {};
    }
    if (type == "window.maximize"sv) {
        m_window.isMaximized() ? m_window.showNormal() : m_window.showMaximized();
        return {};
    }
    if (type == "window.close"sv) {
        QTimer::singleShot(0, &m_window, &QWidget::close);
        return {};
    }
    auto id = data.get_string("tabId"sv);
    if (type == "blocking.dynamic-save"sv) {
        if (m_window.is_private() == WebView::IsPrivate::Yes) return Error::from_string_literal("Edit persistent rules from a regular window");
        auto text = data.get_string("rules"sv);
        if (!text.has_value() || text->bytes().size() > 64 * KiB) return Error::from_string_literal("Dynamic rules exceed 64 KiB");
        StringBuilder rules;
        size_t count = 0;
        for (auto line : text->bytes_as_string_view().split_view('\n')) {
            line = line.trim_whitespace();
            if (line.is_empty() || line.starts_with('!')) continue;
            auto columns = QString::fromUtf8(line.characters_without_null_termination(), line.length()).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (++count > 1024 || columns.size() != 4
                || !QStringList { "*", "inline-script", "1p-script", "3p-script", "3p-frame", "image", "3p" }.contains(columns[2])
                || !QStringList { "block", "allow", "noop" }.contains(columns[3])
                || (columns[1] != "*" && columns[2] != "*")) return Error::from_string_literal("Invalid dynamic rule: source destination type action");
            for (int i = 0; i < 2; ++i) {
                if (columns[i].size() > 253 || !QRegularExpression("^[a-zA-Z0-9*._:\\[\\]%-]+$").match(columns[i]).hasMatch())
                    return Error::from_string_literal("Invalid dynamic rule hostname");
            }
            rules.append("!#derpbird-dynamic "sv);
            rules.append(ak_string_from_qstring(columns.join(' ')));
            rules.append('\n');
        }
        auto& settings = WebView::Application::settings();
        for (auto line : settings.custom_content_blocker_filters().bytes_as_string_view().split_view('\n')) {
            if (!line.starts_with("!#derpbird-dynamic "sv)) { rules.append(line); rules.append('\n'); }
        }
        settings.set_custom_content_blocker_filters(MUST(rules.to_string()));
        return {};
    }
    if (type == "browser.suggest"sv) {
        auto input = data.get_string("input"sv);
        auto query = data.get_integer<u64>("query"sv);
        if (!input.has_value() || input->bytes().size() > 4096 || !query.has_value()) return Error::from_string_literal("Invalid address query");
        m_suggestion_timer->stop();
        m_pending_suggestions = JsonValue {};
        m_suggestion_query = *query;
        m_suggestions_published = false;
        if (input->is_empty()) { m_autocomplete->cancel_pending_query(); m_address_suggestions = JsonValue {}; }
        else m_autocomplete->query_autocomplete_engine(*query, *input, 8);
        return {};
    }
    Tab* tab = nullptr;
    if (id.has_value()) {
        for (int i = 0; i < m_window.tab_count(); ++i)
            if (tab_id(*m_window.tab_at(i)) == *id)
                tab = m_window.tab_at(i);
    }
    if (!tab)
        return Error::from_string_literal("Unknown tab");
    if (type == "blocking.refresh"sv || type == "blocking.clear"sv) {
        if (tab != m_window.current_tab()) return Error::from_string_literal("Log target changed");
        if (type == "blocking.clear"sv && tab->is_loaded()) tab->view().clear_blocking_log();
        return {};
    }
    auto index = m_window.tab_index(tab);
    if (type == "script.command"sv) {
        auto command_id = data.get_string("id"sv);
        auto input = data.get_object("event"sv);
        if (tab != m_window.current_tab() || !tab->is_loaded() || !command_id.has_value() || !input.has_value())
            return Error::from_string_literal("Userscript command target changed");
        JsonObject event;
        for (auto flag : { "keyboard"sv, "ctrlKey"sv, "altKey"sv, "shiftKey"sv, "metaKey"sv }) {
            auto value = input->get_bool(flag);
            if (!value.has_value()) return Error::from_string_literal("Invalid command event");
            event.set(flag, *value);
        }
        auto key = input->get_string("key"sv);
        if (!key.has_value() || key->bytes().size()>64) return Error::from_string_literal("Invalid command key");
        event.set("key"sv, *key);
        return Application::the().invoke_derpbird_script_command(tab->view().page(), *command_id, event);
    }
    if (type == "blocking.pick"sv || type == "blocking.zap"sv) {
        tab->ensure_loaded();
        if (!tab->url().scheme().is_one_of("http"sv, "https"sv)) return Error::from_string_literal("Pick an element on an HTTP(S) page");
        bool zap = type == "blocking.zap"sv;
        m_filter_tab = tab;
        m_filter_proposal = JsonValue {};
        auto target = QPointer(tab);
        tab->view().on_derpbird_filter_selector = [self = QPointer(this), target](String url, String selector) {
            if (!self || !target || self->m_filter_tab != target || target->url().serialize() != url || selector.is_empty() || selector.bytes().size() > 4096) return;
            JsonObject proposal;
            proposal.set("tabId"sv, tab_id(*target));
            proposal.set("url"sv, url);
            proposal.set("rule"sv, MUST(String::formatted("{}##{}", target->url().serialized_host(), selector)));
            self->m_filter_proposal = AK::move(proposal);
            self->queue_snapshot();
        };
        tab->view().start_node_picker([self = QPointer(this), target, zap](auto event) {
            if (!self || !target || !target->is_loaded()) return;
            using Type = DevTools::DevToolsDelegate::NodePickerEvent::Type;
            if (event.type == Type::Canceled) {
                self->m_filter_proposal = JsonValue {}; self->m_filter_tab = nullptr; self->queue_snapshot();
                QTimer::singleShot(0, self, [target] { if (target && target->is_loaded()) target->view().stop_node_picker(); });
                return;
            }
            if (event.type == Type::Hovered && event.node_id.has_value()) { target->view().highlight_dom_node(*event.node_id, {}); return; }
            if (event.type != Type::Picked || !event.node_id.has_value()) return;
            auto node = *event.node_id;
            // Stop on the next event-loop turn: stopping destroys this callback.
            QTimer::singleShot(0, self, [self, target, node, zap] {
                if (!self || !target || !target->is_loaded()) return;
                target->view().stop_node_picker();
                if (zap) { target->view().remove_dom_node(node); target->view().clear_node_picker(); self->m_filter_tab = nullptr; }
                else { target->view().highlight_dom_node(node, {}); target->view().page().async_derpbird_filter_selector(node); }
            });
        });
        m_window.activate_tab(index);
        tab->view().setFocus();
        return {};
    }
    if (type == "blocking.picker-cancel"sv || type == "blocking.picker-save"sv) {
        if (!m_filter_tab || m_filter_tab != tab || !m_filter_proposal.is_object()
            || tab->url().serialize() != m_filter_proposal.as_object().get_string("url"sv))
            return Error::from_string_literal("Selected page changed");
        if (type == "blocking.picker-save"sv) {
            if (m_window.is_private() == WebView::IsPrivate::Yes) return Error::from_string_literal("Save filters from a regular window");
            auto rule = data.get_string("rule"sv);
            auto prefix = MUST(String::formatted("{}##", tab->url().serialized_host()));
            if (!rule.has_value() || !rule->bytes_as_string_view().starts_with(prefix.bytes_as_string_view()) || rule->bytes().size() > 8192 || rule->contains('\n') || rule->contains('\r'))
                return Error::from_string_literal("Invalid cosmetic rule");
            auto& settings = WebView::Application::settings();
            settings.set_custom_content_blocker_filters(MUST(String::formatted("{}\n{}", settings.custom_content_blocker_filters(), *rule)));
            tab->view().reload();
        }
        tab->view().clear_node_picker();
        m_filter_tab = nullptr;
        m_filter_proposal = JsonValue {};
        return {};
    }
    if (type == "tab.activate"sv) {
        auto focus = data.get_string("focus"sv);
        if (focus.has_value() && *focus != "tab"sv)
            return Error::from_string_literal("Invalid tab focus target");
        m_window.activate_tab(index);
        // Arrow/Home/End navigation keeps subsequent keys on the tab strip.
        // Ordinary mouse/shortcut activation retains the native content focus.
        if (focus.has_value() && m_shell)
            m_shell->setFocus(Qt::OtherFocusReason);
        return {};
    }
    if (type == "tab.tree-move"sv) {
        auto target_id = data.get_string("targetId"sv);
        auto placement = data.get_string("placement"sv);
        if (!target_id.has_value() || !placement.has_value()) return Error::from_string_literal("Missing drop target");
        for (int i = 0; i < m_window.tab_count(); ++i)
            if (tab_id(*m_window.tab_at(i)) == *target_id) return move_tree_tab(*tab, *m_window.tab_at(i), *placement);
        return Error::from_string_literal("Drop target no longer exists");
    }
    if (type == "tab.ungroup"sv) {
        if (auto* parent = tree_parent(*tab)) return move_tree_tab(*tab, *parent, "after"sv);
        return detach_tree_tab(*tab);
    }
    if (type == "tab.new-child"sv) {
        if (tab->is_pinned()) return Error::from_string_literal("Unpin the tab before creating a group");
        auto* parent = tree_parent(*tab);
        if (!parent) parent = tab;
        auto& child = m_window.new_tab_from_url(Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
        return move_tree_tab(child, *parent, "inside"sv);
    }
    if (type == "tab.close-group"sv) {
        if (tree_parent(*tab)) return Error::from_string_literal("Close group is available on its parent tab");
        QVector<QPointer<Tab>> members;
        for (int i = m_window.tab_count() - 1; i >= 0; --i)
            if (tree_parent(*m_window.tab_at(i)) == tab) members.append(m_window.tab_at(i));
        members.append(tab);
        QTimer::singleShot(0, &m_window, [window = QPointer(&m_window), members] {
            for (auto member : members) {
                if (!window) break;
                if (!member) continue;
                auto index = window->tab_index(member);
                if (index >= 0) window->request_to_close_tab(index);
            }
        });
        return {};
    }
    if (type == "tab.close"sv) {
        // Closing the final tab can delete the window. Reply before dispatching.
        QTimer::singleShot(
            0, &m_window, [window = QPointer(&m_window), target = QPointer(tab)] {
                if (window && target) {
                    auto index = window->tab_index(target);
                    if (index >= 0)
                        window->request_to_close_tab(index);
                }
            });
        return {};
    }
    if (type == "tab.duplicate"sv) {
        m_window.duplicate_tab(*tab);
        return {};
    }
    if (type == "tab.close-others"sv || type == "tab.close-right"sv) {
        bool right_only = type == "tab.close-right"sv;
        QTimer::singleShot(0, &m_window, [window = QPointer(&m_window), target = QPointer(tab), right_only] {
            if (!window || !target) return;
            auto target_index = window->tab_index(target);
            if (target_index < 0) return;
            // Select the survivor first, avoiding lazy loads of tabs about to close.
            auto* current = window->current_tab();
            if (current && current != target && !current->is_pinned()
                && (!right_only || window->tab_index(current) > target_index))
                window->activate_tab(target_index);
            for (int i = window->tab_count() - 1; window && target && i >= 0; --i) {
                if (right_only && i <= window->tab_index(target)) break;
                auto* candidate = window->tab_at(i);
                if (candidate != target && !candidate->is_pinned()) window->request_to_close_tab(i);
            }
        });
        return {};
    }
    if (type == "tab.pin"sv) {
        auto pinned = data.get_bool("value"sv);
        if (!pinned.has_value())
            return Error::from_string_literal("Invalid pin state");
        *pinned ? m_window.pin_tab(*tab) : m_window.unpin_tab(*tab);
        return {};
    }
    if (type == "tab.move"sv) {
        auto position = data.get_integer<int>("index"sv);
        if (!position.has_value() || *position < 0 || *position >= m_window.tab_count())
            return Error::from_string_literal("Invalid tab position");
        m_window.move_tab(index, *position);
        return {};
    }
    tab->ensure_loaded();
    if (type == "tab.navigate"sv || type == "tab.navigate-input"sv) {
        auto url = data.get_string("url"sv);
        if (!url.has_value())
            return Error::from_string_literal("Missing URL");
        auto parsed = type == "tab.navigate-input"sv ? WebView::sanitize_url(*url, WebView::Application::settings().search_engine_settings().engine) : URL::Parser::basic_parse(*url);
        if (!parsed.has_value() || !is_public_navigation_url(*parsed))
            return Error::from_string_literal("Unsupported navigation URL");
        tab->view().set_next_history_visit_transition(WebView::HistoryVisitTransition::Omnibox);
        tab->navigate(*parsed);
    } else if (type == "tab.back"sv)
        tab->view().traverse_the_history_by_delta(-1);
    else if (type == "tab.forward"sv)
        tab->view().traverse_the_history_by_delta(1);
    else if (type == "tab.reload"sv)
        tab->view().reload();
    else if (type == "tab.stop"sv)
        tab->view().stop_loading();
    else if (type == "tab.find"sv)
        tab->show_find_in_page();
    else if (type == "tab.bookmark"sv)
        tab->show_bookmark_popover();
    else if (type == "tab.site-settings"sv)
        tab->show_site_settings_popover();
    else if (type == "tab.zoom-in"sv)
        tab->view().zoom_in();
    else if (type == "tab.zoom-out"sv)
        tab->view().zoom_out();
    else if (type == "tab.zoom-reset"sv)
        tab->view().reset_zoom();
    else if (type == "tab.mute"sv) {
        auto muted = data.get_bool("value"sv);
        if (!muted.has_value())
            return Error::from_string_literal("Invalid mute state");
        if (*muted != (tab->view().page_mute_state() == Web::HTML::MuteState::Muted))
            tab->view().toggle_page_mute_state();
    } else
        return Error::from_string_literal("Unknown command");
    return {};
}

void DerpBirdChrome::update_display_metadata(double device_pixel_ratio, Optional<u64> display_id, double refresh_rate)
{
    m_shell->set_device_pixel_ratio(device_pixel_ratio);
    m_shell->set_display_metadata(display_id, refresh_rate);
}

void DerpBirdChrome::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_shell->set_device_pixel_ratio(devicePixelRatioF());
    m_shell->setGeometry(rect());
    place_content();
}

void DerpBirdChrome::place_content()
{
    auto placement = m_surface_state.placement(rect());
    if (!placement.visible()) {
        m_shell->set_compositing_above(nullptr);
        m_content->hide();
        return;
    }
    if (m_content->geometry() != placement.viewport)
        m_content->setGeometry(placement.viewport);
    if (m_content->mask() != placement.visible_region)
        m_content->setMask(placement.visible_region);
    auto* tab = m_window.current_tab();
    auto* content_view = tab && tab->is_loaded() ? &tab->view() : nullptr;
    m_shell->set_compositing_above(content_view);
    m_content->show();
    m_content->raise();
}

} // namespace Ladybird
