#include <UI/Qt/DerpBirdActivityOverlay.h>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <UI/Qt/PlainText.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <UI/Qt/Actions.h>
#include <LibWebView/BrowsingSession.h>
#include <UI/Qt/SessionStore.h>
#include <LibWebView/Profile.h>
#include <LibWebView/WebUI.h>
#include <UI/Qt/DerpBirdImport.h>
#include <UI/Qt/DerpBirdFilePicker.h>
#include <UI/Qt/DerpBirdProxy.h>
#include <UI/Qt/DerpBirdPageSources.h>
#include <QFileDialog>
#include <QRegion>
#include <QLayout>
#include <QScopedValueRollback>
#include <cmath>
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
#include <UI/Qt/DerpBirdAutomation.h>
#include <UI/Qt/DerpBirdWindowScreenshot.h>
#include <QToolTip>
#include <UI/Qt/DerpBirdDevTools.h>
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

class NavigationObserver final : public Action::Observer {
public:
    explicit NavigationObserver(DerpBirdChrome& chrome)
        : m_chrome(&chrome)
    {
    }
    void on_enabled_state_changed(Action&) override
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
    , WebView::SettingsObserver(Application::settings())
    , m_window(window)
{
    if (auto result = Application::the().derpbird_style_store(); result.is_error())
        warnln("Unable to load user styles: {}", result.error());
    if (auto result = Application::the().derpbird_script_store(); result.is_error())
        warnln("Unable to load userscripts: {}", result.error());
    m_shell_preferences = std::make_unique<QSettings>(QString::fromUtf8(Application::the().derpbird_profile_paths().config.characters()) + "/DerpBirdChrome.ini", QSettings::IniFormat);
    load_workspaces();
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
    new QShortcut(QKeySequence(Qt::Key_F12), &window, [this] { if (auto* tab = m_window.current_tab()) { if (auto result = inspect_tab(*tab); result.is_error()) warnln("{}", result.error()); } });
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
        m_shell->send_derpbird_shell_message("derpbird.sources-changed"_string, JsonValue {});
    });
    m_autocomplete = std::make_unique<WebView::Autocomplete>(Application::the(), window.is_private());
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
                if (m_window.is_private() == WebView::IsPrivate::Yes || Application::the().derpbird_profile_is_temporary()) {
                    completion(Error::from_string_literal("Edit proxy settings in a regular persistent window")); return;
                }
                if (type == "proxy.site"sv) {
                    auto* tab = m_window.current_tab();
                    auto requested = data.as_object().get_string("url"sv);
                    if (!tab || !tab->is_loaded() || !requested.has_value() || tab->view().url().serialize() != *requested) {
                        completion(Error::from_string_literal("The active page changed; reopen the proxy switcher")); return;
                    }
                    tab->view().derpbird_command("proxy.site"_string, data, AK::move(completion)); return;
                }
                if (type == "proxy.get"sv) {
                    auto state = DerpBirdProxy::state();
                    if (state.is_error()) completion(state.release_error());
                    else completion(JsonValue(state.release_value()));
                    return;
                }
                if (type == "proxy.replace"sv) {
                    if (auto current_url = data.as_object().get_string("currentURL"sv); current_url.has_value()) {
                        auto* tab = m_window.current_tab();
                        if (!tab || !tab->is_loaded() || tab->view().url().serialize() != *current_url) {
                            completion(Error::from_string_literal("The active page changed; reopen the proxy switcher")); return;
                        }
                    }
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
                if (m_window.is_private() == WebView::IsPrivate::Yes || Application::the().derpbird_profile_is_temporary()) {
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
                auto shell_document = m_shell->derpbird_document();
                auto channel = m_shell->derpbird_shell_channel();
                auto top_document = tab->view().derpbird_document();
                auto valid = [self = QPointer { this }, target = QPointer { tab }, view = QPointer { &tab->view() }, shell_document, channel, top_document, revision] {
                    if (!self || !target || !view || self->m_window.current_tab() != target || !target->is_loaded()
                        || &target->view() != view || view->crash_overlay_active()
                        || view->derpbird_document() != top_document
                        || !self->m_shell->derpbird_page_open() || self->m_shell->crash_overlay_active()
                        || self->m_shell->derpbird_shell_channel() != channel
                        || self->m_shell->derpbird_document() != shell_document) return false;
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
                auto path = QString::fromUtf8(Application::the().derpbird_profile_paths().data.characters())
                    + (scripts ? "/DerpBirdScriptFolders.json" : "/DerpBirdStyleFolders.json");
                // Only the native picker/config mutation touches the GUI thread.
                if (data.as_object().get_string("action"sv) != "list"sv) {
                    auto configured = derpbird_source_folders(this, path, scripts, data.as_object(), false);
                    if (configured.is_error()) { completion(configured.release_error()); return; }
                }
                m_source_scans.insert(scripts);
                auto document = m_shell->derpbird_document();
                auto channel = m_shell->derpbird_shell_channel();
                struct ScanResult { Optional<ErrorOr<JsonValue>> result; QStringList watches; };
                auto scan = std::make_shared<ScanResult>();
                auto* thread = QThread::create([scan, path, scripts] {
                    JsonObject request; request.set("action"sv, "list"sv);
                    scan->result = derpbird_source_folders(nullptr, path, scripts, request, true, &scan->watches);
                });
                connect(thread, &QThread::finished, this, [this, scan, scripts, document, channel, completion = AK::move(completion)]() mutable {
                    m_source_scans.remove(scripts);
                    if (!m_shell->derpbird_page_open() || m_shell->derpbird_shell_channel() != channel
                        || m_shell->derpbird_document() != document) {
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
                auto document = m_shell->derpbird_document();
                auto channel = m_shell->derpbird_shell_channel();
                m_source_download_token = *token;
                m_source_download = new DerpBirdSourceDownload(this, *address,
                    [self = QPointer { this }, document, channel, completion = AK::move(completion)](ErrorOr<JsonValue> result) mutable {
                        if (self) self->m_source_download = nullptr;
                        if (!self || !self->m_shell->derpbird_page_open() || self->m_shell->derpbird_shell_channel() != channel
                            || self->m_shell->derpbird_document() != document) {
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
                auto document = m_shell->derpbird_document();
                auto channel = m_shell->derpbird_shell_channel();
                m_script_install = new DerpBirdScriptInstall(this, prepared.release_value(),
                    [self = QPointer { this }, document, channel, revision = *revision, completion = AK::move(completion)](ErrorOr<JsonArray> result) mutable {
                        if (self) self->m_script_install = nullptr;
                        if (!self || !self->m_shell->derpbird_page_open() || self->m_shell->crash_overlay_active()
                            || self->m_shell->derpbird_shell_channel() != channel
                            || self->m_shell->derpbird_document() != document
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
                    }, {
                        .request = [](JsonValue payload, Function<void(ErrorOr<JsonValue>)> done) {
                            return HubConnection::the().request_derpbird(0, "network.fetch"_string, AK::move(payload), AK::move(done));
                        },
                        .cancel = [](u64 id) { if (auto* hub = HubConnection::the_if_connected()) hub->cancel_derpbird_request(id); },
                    }, QString::fromUtf8(Application::the().derpbird_profile_paths().data.characters()) + "/DerpBirdScriptFolders.json");
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
                auto shell_document = m_shell->derpbird_document();
                auto channel = m_shell->derpbird_shell_channel();
                auto top_document = tab->view().derpbird_document();
                auto valid = [self = QPointer { this }, target = QPointer { tab }, view = QPointer { &tab->view() }, shell_document, channel, top_document] {
                    return self && target && view && self->m_window.current_tab() == target && target->is_loaded()
                        && &target->view() == view && !view->crash_overlay_active()
                        && view->derpbird_document() == top_document
                        && self->m_shell->derpbird_page_open() && !self->m_shell->crash_overlay_active()
                        && self->m_shell->derpbird_shell_channel() == channel
                        && self->m_shell->derpbird_document() == shell_document
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
                auto document = m_shell->derpbird_document();
                auto channel = m_shell->derpbird_shell_channel();
                auto ticket = m_secret_clipboard.ticket();
                auto payload = data;
                payload.as_object().set("type"sv, "credentials.reveal"sv);
                Application::the().derpbird_credentials_request(payload, m_window.is_private() == WebView::IsPrivate::Yes,
                    [self = QPointer { this }, document, channel, ticket, completion = AK::move(completion)](ErrorOr<JsonValue> result) mutable {
                        if (!self || !self->m_shell->derpbird_page_open() || self->m_shell->crash_overlay_active()
                            || self->m_shell->derpbird_shell_channel() != channel
                            || self->m_shell->derpbird_document() != document
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
    m_content->layout()->setEnabled(false);
    connect(&window.tab_widget(), &TabWidget::current_tab_changed, this,
        [this] { workspace_tab_changed(); clear_autofill(); if (m_credential_fill) m_credential_fill->cancel(); m_credential_proposal.clear(); place_content(); queue_snapshot(); });
    connect(window.tab_widget().tab_bar(), &QTabBar::tabMoved, this,
        [this] { queue_snapshot(); });
    for (int i = 0; i < window.tab_count(); ++i)
        observe_tab(*window.tab_at(i));
    m_automation_window_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    DerpBirdAutomation::the().add_window(m_automation_window_id, {
        .owner = this,
        .state = [this] {
            JsonObject state;
            state.set("activityFeedback"sv, true);
            state.set("agentId"sv, ak_string_from_qstring(m_window.property("derpbirdAgentId").toString()));
            state.set("private"sv, m_window.is_private() == WebView::IsPrivate::Yes);
            state.set("profile"sv, MUST(String::from_utf8(Application::the().derpbird_profile_paths().data.view())));
            JsonArray tabs;
            for (int i = 0; i < m_window.tab_count(); ++i) {
                auto& tab = *m_window.tab_at(i);
                JsonObject item;
                item.set("id"sv, tab_id(tab));
                item.set("url"sv, tab.url().serialize());
                item.set("title"sv, ak_string_from_qstring(tab.title()));
                item.set("workspace"sv, ak_string_from_qstring(tab_workspace(tab)));
                item.set("active"sv, m_window.current_tab() == &tab);
                item.set("fileChooser"sv, tab.has_file_picker());
                if (tab.is_loaded()) item.set("context"sv, tab.view().handle());
                MUST(tabs.append(AK::move(item)));
            }
            state.set("tabs"sv, AK::move(tabs));
            return JsonValue { AK::move(state) };
        },
        .request = [this](JsonValue const& data, auto done) {
            if (data.is_object() && data.as_object().get_string("type"sv) == "automation.activity"sv) {
                auto const& obj = data.as_object();
                auto id = obj.get_string("tabId"sv), kind = obj.get_string("kind"sv);
                if (!id.has_value() || !kind.has_value()) { done(Error::from_string_literal("Missing activity target")); return; }
                Tab* target = nullptr;
                for (int i = 0; i < m_window.tab_count(); ++i)
                    if (tab_id(*m_window.tab_at(i)) == *id) target = m_window.tab_at(i);
                if (!target || !target->is_loaded()) { done(JsonValue {}); return; }
                auto key = qstring_from_ak_string(*id);
                auto& overlay = m_activity_overlays[key];
                if (!overlay) overlay = new DerpBirdActivityOverlay(target->view());
                auto point = [&](StringView name) -> Optional<QPointF> {
                    auto value = obj.get_object(name);
                    if (!value.has_value()) return {};
                    auto x = value->get("x"sv), y = value->get("y"sv);
                    if (!x.has_value() || !y.has_value() || !x->is_number() || !y->is_number()) return {};
                    auto px=x->get_double_with_precision_loss().value(), py=y->get_double_with_precision_loss().value();
                    if (!std::isfinite(px) || !std::isfinite(py)) return {};
                    return QPointF(px, py);
                };
                Optional<QRectF> field;
                auto bounds = obj.get_object("field"sv);
                if (bounds.has_value()) {
                    auto x=bounds->get("x"sv), y=bounds->get("y"sv), w=bounds->get("width"sv), h=bounds->get("height"sv);
                    if (x.has_value() && y.has_value() && w.has_value() && h.has_value() && x->is_number() && y->is_number() && w->is_number() && h->is_number())
                        field = QRectF(x->get_double_with_precision_loss().value(), y->get_double_with_precision_loss().value(), w->get_double_with_precision_loss().value(), h->get_double_with_precision_loss().value());
                }
                overlay->activity(qstring_from_ak_string(*kind), point("point"sv), point("from"sv), field);
                done(JsonValue {}); return;
            }
            if (data.is_object() && data.as_object().get_string("type"sv) == "automation.screenshot"sv) {
                DerpBirdWindowScreenshot::capture(m_window, AK::move(done));
                return;
            }
            if (data.is_object() && data.as_object().get_string("type"sv) == "automation.resize"sv) {
                auto width = data.as_object().get_integer<int>("width"sv);
                auto height = data.as_object().get_integer<int>("height"sv);
                if (!width.has_value() || !height.has_value() || *width < 1 || *height < 1 || *width > 8192 || *height > 8192) { done(Error::from_string_literal("Invalid window size")); return; }
                m_window.resize(*width, *height);
                done(JsonValue {}); return;
            }
            if (data.is_object() && data.as_object().get_string("type"sv) == "automation.files"sv) {
                auto id = data.as_object().get_string("tabId"sv);
                auto paths = data.as_object().get_array("paths"sv);
                if (!id.has_value() || !paths.has_value() || paths->size() > 100) { done(Error::from_string_literal("Invalid file selection")); return; }
                QStringList files;
                for (auto const& path : paths->values()) {
                    if (!path.is_string()) { done(Error::from_string_literal("Invalid file path")); return; }
                    files.append(qstring_from_ak_string(path.as_string()));
                }
                for (int i = 0; i < m_window.tab_count(); ++i) {
                    auto& tab = *m_window.tab_at(i);
                    if (tab_id(tab) != *id) continue;
                    auto result = tab.complete_file_picker(files);
                    if (result.is_error()) done(result.release_error()); else done(JsonValue {});
                    return;
                }
                done(Error::from_string_literal("File chooser tab closed")); return;
            }
            if (data.is_object() && data.as_object().get_string("type"sv) == "automation.devtools"sv) {
                auto id = data.as_object().get_string("tabId"sv);
                auto request = data.as_object().get_object("request"sv);
                auto type = request.has_value() ? request->get_string("type"sv) : Optional<String> {};
                if (!id.has_value() || !type.has_value() || !type->is_one_of("devtools.snapshot"sv, "devtools.response"sv, "devtools.source"sv, "devtools.refresh"sv, "devtools.clear"sv)) {
                    done(Error::from_string_literal("Invalid automation developer tools request")); return;
                }
                for (int i = 0; i < m_window.tab_count(); ++i) {
                    auto& tab = *m_window.tab_at(i);
                    if (tab_id(tab) != *id) continue;
                    if (auto result = inspect_tab(tab); result.is_error()) { done(result.release_error()); return; }
                    tab.view().derpbird_command("devtools"_string, JsonValue { *request }, AK::move(done)); return;
                }
                done(Error::from_string_literal("Inspected tab closed")); return;
            }
            m_shell->on_derpbird_request(data, AK::move(done));
        },
        .changed = [this] { queue_snapshot(); },
    });
    // Same-user local automation is available for every browser by default.
    if (auto result = DerpBirdAutomation::the().enable(); result.is_error()) warnln("Automation: {}", result.error());
    m_shell->load(URL::Parser::basic_parse("about:derpbird"sv).release_value());
}

DerpBirdChrome::~DerpBirdChrome()
{
    DerpBirdAutomation::the().remove_window(m_automation_window_id);
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

ErrorOr<void> DerpBirdChrome::inspect_tab(Tab& tab, Optional<Web::UniqueNodeID> node)
{
    tab.ensure_loaded();
    auto& tools = m_devtools[&tab];
    if (!tools) {
        auto& view = tab.view();
        if (view.on_derpbird_devtools)
            return Error::from_string_literal("This tab is already attached to another debugger");
        tools = new DerpBirdDevTools(view, &m_window, *m_shell_preferences);
    }
    tools->inspect(node);
    return {};
}

void DerpBirdChrome::observe_tab(Tab& tab)
{
    if (m_observed_tabs.contains(&tab))
        return;
    if (tab.session_tab_id().has_value()) {
        m_tree_tabs.insert(*tab.session_tab_id(), &tab);
        auto parent = m_window.session().tab_tree_parent(*tab.session_tab_id());
        if (!parent.is_error()) tab.setProperty("derpbirdTreeParent", static_cast<qlonglong>(parent.value().value_or(0)));
    }
    auto workspace = m_workspace;
    if (m_window.is_private() == WebView::IsPrivate::No && tab.session_tab_id().has_value())
        workspace = m_shell_preferences->value(QString("workspaceTabs/%1").arg(*tab.session_tab_id()), workspace).toString();
    if (std::none_of(m_workspaces.begin(), m_workspaces.end(), [&](auto const& w) { return w.id == workspace; })) workspace = "default";
    assign_workspace(tab, workspace);
    if (m_window.current_tab() == &tab) workspace_tab_changed();
    auto* scope = new QObject(this);
    m_observed_tabs.insert(&tab, Observation { .scope = scope });
    if (tab.property("derpbirdId").isNull())
        tab.setProperty("derpbirdId",
            QUuid::createUuid().toString(QUuid::WithoutBraces));
    connect(&tab, &Tab::title_changed, scope, [this] { queue_snapshot(); });
    connect(&tab, &Tab::favicon_changed, scope, [this] { queue_snapshot(); });
    connect(&tab, &Tab::loading_changed, scope, [this] { queue_snapshot(); });
    connect(&tab, &Tab::audio_state_changed, scope, [this] { queue_snapshot(); });
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
        tab.view().on_blocking_log_change = [this] { if (m_blocking_log_visible) queue_snapshot(); };
        tab.view().installEventFilter(this);
        tab.view().on_derpbird_inspect = [self = QPointer(this), target = QPointer(&tab)](Web::UniqueNodeID node) {
            if (self && target) { if (auto result = self->inspect_tab(*target, node); result.is_error()) warnln("{}", result.error()); }
        };
        tab.view().on_derpbird_credential_focus = [self = QPointer(this), target = QPointer(&tab), view = QPointer(&tab.view())](auto& document, bool available, Gfx::IntRect bounds) {
            if (!self || !target || !view || self->m_window.current_tab() != target) return;
            self->clear_autofill();
            if (!available || self->m_credential_fill || self->m_window.is_private() == WebView::IsPrivate::Yes || !target->is_loaded()) return;
            self->m_autofill_anchor = QRect(bounds.x(), bounds.y(), bounds.width(), bounds.height());
            auto generation = self->m_autofill_generation;
            auto document_id = document.id();
            auto origin = document.origin().serialize();
            auto valid = [self, target, view, document_id, origin, generation] {
                if (!self || !target || !view || !view->derpbird_page_open() || self->m_window.current_tab() != target
                    || self->m_autofill_generation != generation || !target->is_loaded() || view->crash_overlay_active()) return false;
                return view->derpbird_document_is_current(document_id, origin);
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
            auto shell_document = self->m_shell->derpbird_document();
            auto channel = self->m_shell->derpbird_shell_channel();
            auto authority = [self, target, view, shell_document, channel] {
                // The source was validated by WebContentPage at receipt. Login
                // navigation may replace it; keep its captured origin immutable.
                return self && target && view && self->m_window.current_tab() == target
                    && self->m_window.is_private() == WebView::IsPrivate::No
                    && target->is_loaded() && &target->view() == view
                    && view->derpbird_page_open() && !view->crash_overlay_active()
                    && self->m_shell->derpbird_page_open() && !self->m_shell->crash_overlay_active()
                    && self->m_shell->derpbird_shell_channel() == channel
                    && self->m_shell->derpbird_document() == shell_document
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
        connect(&tab.location_edit(), &QLineEdit::textChanged, scope, [this] {
            if (m_content->isHidden()) place_content();
            queue_snapshot();
        });
        // Native surfaces can change while QStackedWidget is removing a page,
        // before its stack and tab bar have matching indices again.
        connect(&tab.view(), &WebContentView::native_surface_changed, scope, [this] { place_content(); }, Qt::QueuedConnection);
        auto back = make<NavigationObserver>(*this);
        observation.back = back.ptr();
        tab.view().navigate_back_action().add_observer(AK::move(back));
        auto forward = make<NavigationObserver>(*this);
        observation.forward = forward.ptr();
        tab.view().navigate_forward_action().add_observer(AK::move(forward));
        WebView::ViewImplementation::NavigationListener listener;
        listener.on_load_start = [self = QPointer(this), view = QPointer(&tab.view())](auto const&) { if (self && self->m_autofill_view == view) self->clear_autofill(); };
        listener.on_load_finish = [self = QPointer(this)](auto const&) {
            if (self) {
                if (self->m_content->isHidden()) self->place_content();
                self->queue_snapshot();
            }
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
    if (auto tools = m_devtools.take(tab)) delete tools;
    m_favicon_cache.remove(tab);
    auto found = m_observed_tabs.find(tab);
    if (found == m_observed_tabs.end())
        return;
    auto observation = found.value();
    m_observed_tabs.erase(found);
    delete observation.scope;
    if (auto view = observation.view) {
        view->on_blocking_log_change = nullptr;
        view->on_derpbird_inspect = nullptr;
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
    auto overlay = m_activity_overlays.take(qstring_from_ak_string(tab_id(tab)));
    if (overlay) delete overlay;
    if (tab.session_tab_id().has_value()) m_tree_tabs.remove(*tab.session_tab_id());
    remove_split(&tab);
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
    Optional<SessionTabId> id = parent ? parent->session_tab_id() : Optional<SessionTabId> {};
    TRY(m_window.session().set_tab_tree_parent(*tab.session_tab_id(), id));
    tab.setProperty("derpbirdTreeParent", static_cast<qlonglong>(id.value_or(0)));
    queue_snapshot();
    return {};
}

ErrorOr<void> DerpBirdChrome::detach_tree_tab(Tab& tab)
{
    // Closing or extracting a half leaves its partner selected as an ordinary tab.
    if (auto* split = split_for(&tab)) {
        auto* survivor = split->first == &tab ? split->second.data() : split->first.data();
        if (m_window.current_tab() == &tab && survivor) m_window.activate_tab(m_window.tab_index(survivor));
        remove_split(&tab);
        place_content();
    }
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
    QSet<Tab*> paired;
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto* tab = m_window.tab_at(i);
        if (tree_parent(*tab) || paired.contains(tab)) continue;
        if (auto* split = split_for(tab)) {
            // Keep the pair and its children together even when the source tabs
            // originally occupied different positions in the strip.
            for (auto* member : { split->first.data(), split->second.data() }) {
                ordered.append(member);
                ordered.append(children.value(member));
                paired.insert(member);
            }
        } else {
            ordered.append(tab);
            ordered.append(children.value(tab));
        }
    }
    m_reordering_tree = true;
    for (int i = 0; i < ordered.size(); ++i)
        if (m_window.tab_at(i) != ordered[i]) m_window.move_tab(m_window.tab_index(ordered[i]), i);
    m_reordering_tree = false;
}

void DerpBirdChrome::group_tab_from_view(Tab& child, Optional<u64> source_view_id)
{
    if (!source_view_id.has_value()) return;
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto* source = m_window.tab_at(i);
        if (source == &child || !source->is_loaded() || source->view().view_id() != *source_view_id || source->is_pinned()) continue;
        if (auto result = move_tree_tab(child, *source, "inside"sv); result.is_error())
            warnln("Cannot group tab opened from page: {}", result.error());
        return;
    }
}

ErrorOr<void> DerpBirdChrome::move_tree_tab(Tab& source, Tab& target, StringView placement)
{
    if (tab_workspace(source) != tab_workspace(target)) return Error::from_string_literal("Tabs belong to different workspaces");
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
    if (placement == "inside"sv) {
        // Either half opens children under the same split-tab item.
        auto* split = split_for(&target);
        parent = split ? split->first.data() : parent ? parent : &target;
    }
    if (parent == &source) return Error::from_string_literal("A group cannot be moved into itself");
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
    remove_split(&source);

    for (int i = 0; i < block.size(); ++i) remaining.insert(destination + i, block[i]);
    m_reordering_tree = true;
    for (int i = 0; i < remaining.size(); ++i)
        if (m_window.tab_at(i) != remaining[i]) m_window.move_tab(m_window.tab_index(remaining[i]), i);
    m_reordering_tree = false;
    place_content();
    queue_snapshot();
    return {};
}

JsonValue DerpBirdChrome::snapshot()
{
    restore_splits();
    normalize_tree_order();
    JsonArray tabs;
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto& tab = *m_window.tab_at(i);
        if (tab_workspace(tab) != m_workspace) continue;
        JsonObject entry;
        entry.set("id"sv, tab_id(tab));
        entry.set("url"sv, tab.url().serialize());
        if (tab.is_loaded() && tab.url() == URL::about_blank() && tab.url_is_hidden())
            entry.set("blankNewTab"sv, true);
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
    JsonObject workspaces;
    JsonArray workspace_items;
    for (auto const& w : m_workspaces) {
        JsonObject item;
        item.set("id"sv, ak_string_from_qstring(w.id));
        item.set("name"sv, ak_string_from_qstring(w.name));
        item.set("icon"sv, ak_string_from_qstring(w.icon));
        item.set("color"sv, ak_string_from_qstring(w.color));
        workspace_items.must_append(AK::move(item));
    }
    workspaces.set("active"sv, ak_string_from_qstring(m_workspace));
    workspaces.set("items"sv, AK::move(workspace_items));
    state.set("workspaces"sv, AK::move(workspaces));
    state.set("addressSuggestions"sv, m_address_suggestions);
    StringBuilder dynamic_rules;
    for (auto line : Application::settings().custom_content_blocker_filters().bytes_as_string_view().split_view('\n')) {
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
    state.set("scriptCommands"sv, active && active->is_loaded() ? active->view().derpbird_script_commands() : JsonArray {});
    auto* logged_view = m_blocking_log_visible && active && active->is_loaded() ? &active->view() : nullptr;
    if (m_request_log_view.data() != logged_view) {
        if (m_request_log_view) m_request_log_view->set_request_logging(false);
        m_request_log_view = logged_view;
        if (m_request_log_view) m_request_log_view->set_request_logging(true);
    }
    if (m_blocking_log_visible && active && active->is_loaded()) {
        JsonObject log;
        auto entries = active->view().derpbird_blocking_log();
        log.set("tabId"sv, tab_id(*active));
        log.set("entries"sv, AK::move(entries));
        state.set("blockingLog"sv, AK::move(log));
    }
    auto proxy_profile = "direct"_string;
    if (auto* tab = m_window.current_tab(); tab && tab->is_loaded()) {
        auto effective = DerpBirdProxy::profile_for(tab->view().url());
        if (!effective.is_error()) proxy_profile = effective.release_value();
    }
    state.set("proxyProfile"sv, proxy_profile);
    JsonObject automation;
    automation.set("enabled"sv, DerpBirdAutomation::the().enabled());
    automation.set("clients"sv, DerpBirdAutomation::the().client_count());
    automation.set("endpoint"sv, ak_string_from_qstring(DerpBirdAutomation::the().endpoint()));
    state.set("automation"sv, AK::move(automation));
    state.set("version"sv, 1);
    state.set("revision"sv, ++m_revision);
    JsonObject preferences;
    auto const& settings = Application::settings().tab_settings();
    preferences.set("layout"sv, settings.vertical_tabs_enabled ? "vertical"sv : "horizontal"sv);
    JsonObject chrome_layout;
    chrome_layout.set("preset"sv, settings.vertical_tabs_enabled ? (settings.everything_in_sidebar ? "compact"sv : "sidebar"sv) : (settings.address_bar_on_bottom ? "bottom"sv : "classic"sv));
    chrome_layout.set("side"sv, settings.vertical_tabs_position == WebView::VerticalTabsPosition::Right ? "right"sv : "left"sv);
    chrome_layout.set("address"sv, settings.address_bar_on_bottom ? "bottom"sv : "top"sv);
    preferences.set("chromeLayout"sv, AK::move(chrome_layout));
    preferences.set("sidebarWidth"sv, clamp<int>(settings.vertical_tabs_expanded_width.value_or(196), 120, 600));
    preferences.set("theme"sv, m_shell_preferences->value("theme", "dark").toString() == "light" ? "light"sv : "dark"sv);
    preferences.set("alwaysClose"sv, m_shell_preferences->value("alwaysClose", true).toBool());
    preferences.set("wrapTabs"sv, m_shell_preferences->value("wrapTabs", false).toBool());
    state.set("preferences"sv, AK::move(preferences));
    state.set("tabs"sv, AK::move(tabs));
    JsonArray splits;
    for (auto const& split : m_splits) {
        if (!split.first || !split.second || tab_workspace(*split.first) != m_workspace) continue;
        JsonObject item;
        item.set("id"sv, tab_id(*split.first));
        item.set("firstTabId"sv, tab_id(*split.first));
        item.set("secondTabId"sv, tab_id(*split.second));
        item.set("axis"sv, split.vertical ? "vertical"sv : "horizontal"sv);
        item.set("ratio"sv, split.ratio);
        splits.must_append(AK::move(item));
    }
    state.set("splits"sv, AK::move(splits));
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
        auto state = snapshot();
        if (m_focus_address_pending) {
            m_focus_address_pending = false;
            state.as_object().set("focusAddress"sv, true);
        }
        m_shell->send_derpbird_shell_message("derpbird.snapshot"_string, state);
    });
}

void DerpBirdChrome::clear_autofill()
{
    ++m_autofill_generation; m_autofill_valid = {}; m_autofill_view = nullptr;
    if (!m_credential_autofill.is_null()) { m_credential_autofill = JsonValue {}; queue_snapshot(); }
}

bool DerpBirdChrome::eventFilter(QObject* watched, QEvent* event)
{
    // The native tab bar still owns all workspaces. Keep its keyboard cycling
    // inside the set shown by the web shell, including when page content owns focus.
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers().testFlag(Qt::ControlModifier) && !key->modifiers().testFlag(Qt::AltModifier)
            && (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab || key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown)) {
            key->accept();
            if (event->type() == QEvent::ShortcutOverride) return true;
            int direction = key->key() == Qt::Key_PageUp || key->key() == Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier) ? -1 : 1;
            auto count = m_window.tab_count();
            auto current = m_window.tab_index(m_window.current_tab());
            for (int offset = 1; offset <= count; ++offset) {
                auto index = (current + direction * offset + count) % count;
                if (tab_workspace(*m_window.tab_at(index)) == m_workspace) { m_window.activate_tab(index); break; }
            }
            return true;
        }
    }

    if (watched == m_shell && event->type() == QEvent::MouseButtonPress) {
        // A pending tab tooltip must not cover the context menu or take its input.
        if (m_shell->on_leave_tooltip_area) m_shell->on_leave_tooltip_area();
        hide_tooltip();
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn) {
        for (auto it = m_observed_tabs.begin(); it != m_observed_tabs.end(); ++it) {
            if (it.value().view == watched && it.key() != m_window.current_tab() && split_for(it.key()) == split_for(m_window.current_tab()) && split_for(it.key())) {
                m_window.activate_tab(m_window.tab_index(it.key()));
                break;
            }
        }
    }
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
    if (watched == m_shell && event->type() == QEvent::FocusOut && m_shell->derpbird_page_open()) {
        // Release shell interaction state before the deferred system-focus update.
        // The shell owns DOM focus and popup policy; native code names no controls.
        m_shell->run_javascript("window.dispatchEvent(new Event('derpbird-shell-deactivate'))"_string);
    }
    return QWidget::eventFilter(watched, event);
}

void DerpBirdChrome::focus_address()
{
    clear_autofill();
    place_content();
    m_shell->setFocus();
    m_focus_address_pending = true;
    queue_snapshot();
}

ErrorOr<JsonValue> DerpBirdChrome::request(JsonValue const& data)
{
    if (!data.is_object())
        return Error::from_string_literal("Invalid request");
    auto const& object = data.as_object();
    auto type = object.get_string("type"sv);
    if (type == "devtools.bootstrap"sv) return JsonValue {};
    if (type == "snapshot"sv)
        return snapshot();
    if (type == "sources.page"sv || type == "sources.toggle"sv) {
        auto* tab = m_window.current_tab();
        if (!tab) return Error::from_string_literal("No active page");
        auto& styles = *TRY(Application::the().derpbird_style_store());
        auto& scripts = *TRY(Application::the().derpbird_script_store());
        if (type == "sources.toggle"sv) {
            if (m_window.is_private() == WebView::IsPrivate::Yes || m_script_install)
                return Error::from_string_literal("Change sources in a regular window after installation finishes");
            if (object.get_string("tabId"sv) != tab_id(*tab) || object.get_string("url"sv) != tab->view().url().serialize())
                return Error::from_string_literal("The active page changed; reopen the list");
            auto kind = object.get_string("kind"sv);
            auto id = object.get_string("id"sv);
            auto enabled = object.get_bool("enabled"sv);
            auto revision = object.get_integer<u64>("revision"sv);
            if (!kind.has_value() || !kind->is_one_of("styles"sv, "scripts"sv) || !id.has_value() || !enabled.has_value() || !revision.has_value())
                return Error::from_string_literal("Invalid source toggle");
            bool style = *kind == "styles"sv;
            if (*revision != (style ? styles.revision() : scripts.revision()))
                return Error::from_string_literal("Sources changed in another window; refresh the list");
            auto items = style ? styles.items() : scripts.items();
            bool found = false;
            for (auto& raw : items.values()) {
                if (raw.as_object().get_string("id"sv) != *id) continue;
                raw.as_object().set("enabled"sv, *enabled);
                found = true;
                break;
            }
            if (!found) return Error::from_string_literal("Source no longer exists");
            if (style) {
                TRY(styles.replace(AK::move(items)));
                TRY(Application::the().set_derpbird_user_style_sections(styles.enabled_sections()));
            } else {
                TRY(scripts.replace(AK::move(items), *revision));
                TRY(Application::the().set_derpbird_user_scripts(JsonValue { scripts.items() }));
            }
        }
        return JsonValue { TRY(derpbird_page_sources(tab->view().url(), tab_id(*tab), m_window.is_private() == WebView::IsPrivate::Yes, styles, scripts)) };
    }
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
    if (type == "automation.set-enabled"sv) {
        auto enabled = data.get_bool("enabled"sv);
        if (!enabled.has_value()) return Error::from_string_literal("Missing automation state");
        if (*enabled) TRY(DerpBirdAutomation::the().enable());
        else DerpBirdAutomation::the().disable();
        return {};
    }
    if (type == "blocking.log"sv) {
        auto visible = data.get_bool("visible"sv);
        if (!visible.has_value()) return Error::from_string_literal("Missing log visibility");
        m_blocking_log_visible = *visible;
        return {};
    }
    if (type == "shell.set-chrome-layout"sv) {
        auto layout = data.get_object("layout"sv);
        if (!layout.has_value()) return Error::from_string_literal("Invalid chrome layout");
        auto preset = layout->get_string("preset"sv);
        auto side = layout->get_string("side"sv);
        auto address = layout->get_string("address"sv);
        if (!preset.has_value() || !preset->is_one_of("classic"sv, "sidebar"sv, "bottom"sv, "compact"sv)
            || !side.has_value() || !side->is_one_of("left"sv, "right"sv)
            || !address.has_value() || !address->is_one_of("top"sv, "bottom"sv))
            return Error::from_string_literal("Invalid chrome layout");
        auto settings = Application::settings().tab_settings();
        settings.vertical_tabs_enabled = preset->is_one_of("sidebar"sv, "compact"sv);
        settings.everything_in_sidebar = *preset == "compact"sv;
        settings.vertical_tabs_position = *side == "right"sv ? WebView::VerticalTabsPosition::Right : WebView::VerticalTabsPosition::Left;
        settings.address_bar_on_bottom = *preset == "bottom"sv || (*preset != "classic"sv && *address == "bottom"sv);
        Application::settings().set_tab_settings(settings);
        return {};
    }
    if (type == "shell.set-layout"sv) {
        auto layout = data.get_string("layout"sv);
        if (!layout.has_value() || !layout->is_one_of("vertical"sv, "horizontal"sv))
            return Error::from_string_literal("Invalid tab layout");
        auto settings = Application::settings().tab_settings();
        settings.vertical_tabs_enabled = *layout == "vertical"sv;
        settings.everything_in_sidebar = false;
        settings.address_bar_on_bottom = false;
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
    if (type == "shell.set-appearance"sv) {
        auto theme = data.get_string("theme"sv);
        auto always_close = data.get_bool("alwaysClose"sv);
        if (!theme.has_value() || !theme->is_one_of("dark"sv, "light"sv) || !always_close.has_value())
            return Error::from_string_literal("Invalid appearance setting");
        m_shell_preferences->setValue("theme", *theme == "light"sv ? "light" : "dark");
        m_shell_preferences->setValue("alwaysClose", *always_close);
        m_shell_preferences->sync();
        if (m_shell_preferences->status() != QSettings::NoError) return Error::from_string_literal("Cannot save appearance setting");
        Application::settings().set_tab_settings(Application::settings().tab_settings());
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
    if (type.bytes_as_string_view().starts_with("workspace."sv)) return workspace_command(data, type);
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
        auto capture_input = data.get_bool("captureInput"sv);
        if (data.has("captureInput"sv) && !capture_input.has_value()) return Error::from_string_literal("Invalid input capture");
        pending.accept(viewport, AK::move(exclusions), capture_input.value_or(false));
        m_pending_surface_state = AK::move(pending);
        return {};
    }
    if (type == "browser.open-page"sv) {
        auto name = data.get_string("page"sv);
        if (!name.has_value() || !name->is_one_of("history"sv, "downloads"sv, "bookmarks"sv, "settings"sv))
            return Error::from_string_literal("Unknown browser page");
        auto url = URL::Parser::basic_parse(TRY(String::formatted("about:{}", *name))).release_value();
        // Reuse a page only within this window's session, including private mode.
        for (int i = 0; i < m_window.tab_count(); ++i) {
            auto* candidate = m_window.tab_at(i);
            if (tab_workspace(*candidate) == m_workspace && candidate->url() == url) { m_window.activate_tab(i); return {}; }
        }
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
                *is_private ? WebView::IsPrivate::Yes : WebView::IsPrivate::No, {}, ShowWindow::Yes, {},
                data.get_string("agentId"sv).has_value() ? qstring_from_ak_string(*data.get_string("agentId"sv)) : QString {});
        } else if (!url.has_value()) {
            m_window.open_new_tab();
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
        auto& settings = Application::settings();
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
    if (type == "tab.inspect"sv) return inspect_tab(*tab);
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
        return Application::the().invoke_derpbird_script_command(tab->view(), *command_id, event);
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
                else { target->view().highlight_dom_node(node, {}); target->view().request_derpbird_filter_selector(node); }
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
            auto& settings = Application::settings();
            settings.set_custom_content_blocker_filters(MUST(String::formatted("{}\n{}", settings.custom_content_blocker_filters(), *rule)));
            tab->view().reload();
        }
        tab->view().clear_node_picker();
        m_filter_tab = nullptr;
        m_filter_proposal = JsonValue {};
        return {};
    }
    if (type == "tab.split"sv) {
        auto target_id = data.get_string("targetId"sv);
        auto edge = data.get_string("edge"sv);
        if (!target_id.has_value() || !edge.has_value() || !edge->is_one_of("left"sv, "right"sv, "top"sv, "bottom"sv)) return Error::from_string_literal("Invalid split target");
        Tab* target = nullptr;
        for (int i = 0; i < m_window.tab_count(); ++i) if (tab_id(*m_window.tab_at(i)) == *target_id) target = m_window.tab_at(i);
        if (!target || tab_workspace(*target) != tab_workspace(*tab) || target == tab || target->is_pinned() || tab->is_pinned()) return Error::from_string_literal("Split requires two different unpinned tabs");
        remove_split(tab); remove_split(target);
        // A split is a single tab-strip item; detach the dragged member from a tree group.
        TRY(detach_tree_tab(*tab));
        TRY(detach_tree_tab(*target));
        bool before = edge->is_one_of("left"sv, "top"sv);
        m_splits.append({ before ? tab : target, before ? target : tab, edge->is_one_of("top"sv, "bottom"sv), .5 });
        save_split(m_splits.last());
        tab->ensure_loaded(); target->ensure_loaded();
        m_window.activate_tab(m_window.tab_index(tab)); place_content();
        return {};
    }
    if (type == "split.switch"sv || type == "split.resize"sv) {
        auto* split = split_for(tab);
        if (!split) return Error::from_string_literal("Tab is not split");
        if (type == "split.switch"sv) split->vertical = !split->vertical;
        else {
            auto value = data.get("ratio"sv);
            if (!value.has_value() || !value->is_number()) return Error::from_string_literal("Invalid split ratio");
            auto ratio = value->get_double_with_precision_loss().value();
            if (!std::isfinite(ratio) || ratio < .2 || ratio > .8) return Error::from_string_literal("Invalid split ratio");
            split->ratio = ratio;
        }
        save_split(*split); place_content(); return {};
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
            if (current && current != target && current->property("derpbirdWorkspace") == target->property("derpbirdWorkspace") && !current->is_pinned()
                && (!right_only || window->tab_index(current) > target_index))
                window->activate_tab(target_index);
            for (int i = window->tab_count() - 1; window && target && i >= 0; --i) {
                if (right_only && i <= window->tab_index(target)) break;
                auto* candidate = window->tab_at(i);
                if (candidate != target && !candidate->is_pinned() && candidate->property("derpbirdWorkspace") == target->property("derpbirdWorkspace")) window->request_to_close_tab(i);
            }
        });
        return {};
    }
    if (type == "tab.pin"sv) {
        auto pinned = data.get_bool("value"sv);
        if (!pinned.has_value())
            return Error::from_string_literal("Invalid pin state");
        if (*pinned) remove_split(tab);
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
        auto parsed = type == "tab.navigate-input"sv ? WebView::sanitize_url(*url, Application::settings().search_engine_settings().engine) : URL::Parser::basic_parse(*url);
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
    m_shell->set_display_metadata(display_id, refresh_rate, false);
}

void DerpBirdChrome::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_shell->set_device_pixel_ratio(devicePixelRatioF());
    m_shell->setGeometry(rect());
    place_content();
}

DerpBirdChrome::Split* DerpBirdChrome::split_for(Tab* tab)
{
    if (!tab) return nullptr;
    for (auto& split : m_splits) if (split.first && split.second && (split.first == tab || split.second == tab)) return &split;
    return nullptr;
}

void DerpBirdChrome::save_split(Split const& split)
{
    if (m_window.is_private() == WebView::IsPrivate::Yes || !split.first || !split.second || !split.first->session_tab_id().has_value() || !split.second->session_tab_id().has_value()) return;
    auto key = QString("splits/%1").arg(*split.first->session_tab_id());
    m_shell_preferences->setValue(key, QStringList { QString::number(*split.second->session_tab_id()), split.vertical ? "vertical" : "horizontal", QString::number(split.ratio) });
}

void DerpBirdChrome::remove_split(Tab* tab)
{
    for (int i = m_splits.size() - 1; i >= 0; --i) {
        auto const& split = m_splits[i];
        if (split.first != tab && split.second != tab && split.first && split.second) continue;
        if (split.first && split.first->session_tab_id().has_value() && m_window.is_private() == WebView::IsPrivate::No)
            m_shell_preferences->remove(QString("splits/%1").arg(*split.first->session_tab_id()));
        m_splits.removeAt(i);
    }
}

void DerpBirdChrome::restore_splits()
{
    if (m_window.is_private() == WebView::IsPrivate::Yes) return;
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto* first = m_window.tab_at(i);
        if (!first->session_tab_id().has_value() || first->is_pinned() || split_for(first)) continue;
        auto saved = m_shell_preferences->value(QString("splits/%1").arg(*first->session_tab_id())).toStringList();
        if (saved.size() != 3 || (saved[1] != "vertical" && saved[1] != "horizontal")) continue;
        bool ok = false; auto ratio = saved[2].toDouble(&ok);
        if (!ok || !std::isfinite(ratio) || ratio < .2 || ratio > .8) continue;
        for (int j = 0; j < m_window.tab_count(); ++j) {
            auto* second = m_window.tab_at(j);
            if (second != first && second->session_tab_id().has_value() && QString::number(*second->session_tab_id()) == saved[0] && !second->is_pinned() && !split_for(second)) {
                m_splits.append({ first, second, saved[1] == "vertical", ratio }); place_content(); break;
            }
        }
    }
}

void DerpBirdChrome::place_content()
{
    if (!m_content || m_placing_content) return;
    QScopedValueRollback placing(m_placing_content, true);
    auto placement = m_surface_state.placement(rect());
    auto* active = m_window.current_tab();
    auto* split = split_for(active);
    // The shell paints the empty new tab. The native web canvas can already
    // be white before the shell receives its first snapshot for this tab.
    bool shell_new_tab = active && !split && (active->url() == WebView::Settings::default_new_tab_page_url()
        || (active->is_loaded() && active->url_is_hidden() && (active->url().scheme().is_empty() || active->url() == URL::about_blank())));
    if (!placement.visible() || shell_new_tab) {
        m_shell->set_compositing_above(nullptr);
        m_content->hide();
        return;
    }
    if (m_content->geometry() != placement.viewport) m_content->setGeometry(placement.viewport);
    m_content->setAttribute(Qt::WA_TransparentForMouseEvents, m_surface_state.captures_input());
    if (m_content->mask() != placement.visible_region) m_content->setMask(placement.visible_region);
    QPointer<Tab> first = split ? split->first.data() : active;
    QPointer<Tab> second = split ? split->second : nullptr;
    QRect first_rect(QPoint(), placement.viewport.size()), second_rect;
    if (split) {
        int available = std::max(0, (split->vertical ? first_rect.height() : first_rect.width()) - 6);
        int size = static_cast<int>(std::floor(available * split->ratio));
        second_rect = first_rect;
        if (split->vertical) { first_rect.setHeight(size); second_rect.setTop(size + 6); }
        else { first_rect.setWidth(size); second_rect.setLeft(size + 6); }
    }
    // These pages have explicit geometry, even before the window is shown.
    // Otherwise TabWidget's startup sizing copies the active half's size into
    // the other restored half while its first child layout is still pending.
    if (first) first->did_get_real_size();
    if (second) second->did_get_real_size();
    if (first) first->ensure_loaded();
    if (second) second->ensure_loaded();
    for (int i = 0; i < m_window.tab_count(); ++i) {
        auto* tab = m_window.tab_at(i);
        if (!tab) continue;
        bool visible = tab == first || tab == second;
        if (tab->is_loaded()) tab->view().set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
        if (visible) {
            auto geometry = tab == first ? first_rect : second_rect;
            if (tab->geometry() != geometry) tab->setGeometry(geometry);
            tab->show();
        } else tab->hide();
    }
    if (first && first->is_loaded()) first->view().set_compositing_above(nullptr);
    if (second && second->is_loaded()) second->view().set_compositing_above(first && first->is_loaded() ? &first->view() : nullptr);
    auto* top = second ? second.data() : first.data();
    m_shell->set_compositing_above(top && top->is_loaded() ? &top->view() : nullptr);
    m_content->show();
    if (m_surface_state.captures_input()) m_shell->raise();
    else m_content->raise();
}

} // namespace Ladybird

namespace Ladybird {
QString DerpBirdChrome::workspace_key() const
{
    return QString("workspaces/%1").arg(m_window.session_window_id().value_or(0));
}
void DerpBirdChrome::load_workspaces()
{
    m_workspaces.append({ "default", "Default", "research", "sage", {} });
    if (m_window.is_private() == WebView::IsPrivate::Yes) return;
    auto rows = QJsonDocument::fromJson(m_shell_preferences->value(workspace_key()).toByteArray()).array();
    for (auto row : rows) {
        auto w = row.toObject();
        auto id = w["id"].toString(), name = w["name"].toString(), icon = w["icon"].toString(), color = w["color"].toString();
        if (id.isEmpty() || id.size() > 128 || name.trimmed().isEmpty() || name.size() > 32 || m_workspaces.size() >= 64
            || !QStringList { "research", "home", "code", "leaf" }.contains(icon) || !QStringList { "sage", "peach", "lilac", "blue" }.contains(color)) continue;
        auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(), [&](auto const& item) { return item.id == id; });
        if (found != m_workspaces.end()) *found = { id, name, icon, color, {} };
        else m_workspaces.append({ id, name, icon, color, {} });
    }
}
void DerpBirdChrome::save_workspaces()
{
    if (m_window.is_private() == WebView::IsPrivate::Yes) return;
    QJsonArray rows;
    for (auto const& w : m_workspaces) rows.append(QJsonObject { { "id", w.id }, { "name", w.name }, { "icon", w.icon }, { "color", w.color } });
    m_shell_preferences->setValue(workspace_key(), QJsonDocument(rows).toJson(QJsonDocument::Compact));
    m_shell_preferences->sync();
}
QString DerpBirdChrome::tab_workspace(Tab const& tab) const
{
    return tab.property("derpbirdWorkspace").toString();
}
void DerpBirdChrome::assign_workspace(Tab& tab, QString const& id)
{
    tab.setProperty("derpbirdWorkspace", id);
    if (m_window.is_private() == WebView::IsPrivate::No && tab.session_tab_id().has_value())
        m_shell_preferences->setValue(QString("workspaceTabs/%1").arg(*tab.session_tab_id()), id);
}
void DerpBirdChrome::workspace_tab_changed()
{
    auto* tab = m_window.current_tab();
    if (!tab) return;
    if (tab_workspace(*tab).isEmpty()) assign_workspace(*tab, m_workspace);
    if (tab_workspace(*tab) != m_workspace) {
        auto previous = std::find_if(m_workspaces.begin(), m_workspaces.end(), [&](auto const& w) { return w.id == m_workspace; });
        if (previous != m_workspaces.end() && previous->last_tab && m_window.tab_index(previous->last_tab) < 0) {
            previous->last_tab = nullptr;
            for (int i = 0; i < m_window.tab_count(); ++i) {
                if (tab_workspace(*m_window.tab_at(i)) == m_workspace) { m_window.activate_tab(i); return; }
            }
            m_window.new_tab_from_url(Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
            return;
        }
    }
    m_workspace = tab_workspace(*tab);
    for (auto& w : m_workspaces) if (w.id == m_workspace) w.last_tab = tab;
    m_address_suggestions = JsonValue {};
    m_pending_suggestions = JsonValue {};
    ++m_suggestion_query;
}
ErrorOr<void> DerpBirdChrome::workspace_command(JsonObject const& data, StringView type)
{
    if (type == "workspace.switch"sv) {
        auto id = data.get_string("id"sv);
        if (!id.has_value()) return Error::from_string_literal("Missing workspace");
        auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(), [&](auto const& w) { return ak_string_from_qstring(w.id) == *id; });
        if (found == m_workspaces.end()) return Error::from_string_literal("Unknown workspace");
        if (found->id == m_workspace) return {};
        m_workspace = found->id;
        auto* target = found->last_tab.data();
        if (!target) for (int i = 0; i < m_window.tab_count(); ++i) if (tab_workspace(*m_window.tab_at(i)) == m_workspace) { target = m_window.tab_at(i); break; }
        if (target) m_window.activate_tab(m_window.tab_index(target));
        else m_window.new_tab_from_url(Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
        queue_snapshot();
        return {};
    }
    if (type != "workspace.create"sv && type != "workspace.update"sv) return Error::from_string_literal("Unknown workspace command");
    auto name = data.get_string("name"sv), icon = data.get_string("icon"sv), color = data.get_string("color"sv);
    if (!name.has_value() || !icon.has_value() || !color.has_value() || !icon->is_one_of("research"sv, "home"sv, "code"sv, "leaf"sv) || !color->is_one_of("sage"sv, "peach"sv, "lilac"sv, "blue"sv))
        return Error::from_string_literal("Invalid workspace");
    auto title = qstring_from_ak_string(*name).trimmed();
    if (title.isEmpty() || title.size() > 32) return Error::from_string_literal("Invalid workspace name");
    if (type == "workspace.create"sv) {
        if (m_workspaces.size() >= 64) return Error::from_string_literal("Workspace limit reached");
        m_workspace = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_workspaces.append({ m_workspace, title, qstring_from_ak_string(*icon), qstring_from_ak_string(*color), {} });
        save_workspaces();
        m_window.new_tab_from_url(Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::end());
    } else {
        auto id = data.get_string("id"sv);
        auto found = std::find_if(m_workspaces.begin(), m_workspaces.end(), [&](auto const& w) { return id.has_value() && ak_string_from_qstring(w.id) == *id; });
        if (found == m_workspaces.end()) return Error::from_string_literal("Unknown workspace");
        found->name = title; found->icon = qstring_from_ak_string(*icon); found->color = qstring_from_ak_string(*color);
        save_workspaces();
    }
    if (m_window.is_private() == WebView::IsPrivate::No && m_shell_preferences->status() != QSettings::NoError) return Error::from_string_literal("Cannot save workspaces");
    queue_snapshot();
    return {};
}
}
