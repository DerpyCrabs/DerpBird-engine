#pragma once
#include <memory>

#include <AK/JsonValue.h>
#include <AK/WeakPtr.h>
#include <LibWebView/ViewImplementation.h>
#include <UI/Qt/Actions.h>
#include <LibWebView/Settings.h>
#include <LibWebView/Autocomplete.h>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QRect>
#include <QSettings>
#include <QVector>
#include <QWidget>
#include <UI/Qt/ContentSurfaceState.h>
#include <UI/Qt/DerpBirdSecretClipboard.h>
#include <UI/Qt/DerpBirdCredentialProposal.h>

namespace Ladybird {

class DerpBirdImport;
class DerpBirdActivityOverlay;
class DerpBirdDevTools;
class DerpBirdFilePicker;
class BrowserWindow;
class DerpBirdCredentialFill;
class DerpBirdScriptInstall;
class DerpBirdSourceDownload;
class Tab;
class WebContentView;

class DerpBirdChrome final : public QWidget
    , public WebView::SettingsObserver {
public:
    explicit DerpBirdChrome(BrowserWindow&);
    ~DerpBirdChrome() override;
    void observe_tab(Tab&);
    void unobserve_tab(Tab&);
    void queue_snapshot();
    void focus_address();
    ErrorOr<void> inspect_tab(Tab&, Optional<Web::UniqueNodeID> = {});
    void update_display_metadata(double device_pixel_ratio, Optional<u64> display_id, double refresh_rate);
    Tab* tree_parent(Tab&) const;
    void group_tab_from_view(Tab& child, Optional<u64> source_view_id);
    ErrorOr<void> detach_tree_tab(Tab&);
    ErrorOr<void> move_tree_tab(Tab&, Tab& target, StringView placement);
    bool reordering_tree() const { return m_reordering_tree; }


private:
    QString m_automation_window_id;
    QHash<QString, QPointer<DerpBirdActivityOverlay>> m_activity_overlays;
    struct Workspace { QString id; QString name; QString icon; QString color; QPointer<Tab> last_tab; };
    QVector<Workspace> m_workspaces;
    QString m_workspace { "default" };
    QString workspace_key() const;
    void load_workspaces();
    void save_workspaces();
    void assign_workspace(Tab&, QString const&);
    QString tab_workspace(Tab const&) const;
    void workspace_tab_changed();
    ErrorOr<void> workspace_command(JsonObject const&, StringView);
    QPointer<DerpBirdImport> m_import;
    DerpBirdFilePicker* m_file_picker { nullptr };
    bool m_password_import { false };
    ErrorOr<void> set_tree_parent(Tab&, Tab*);
    void normalize_tree_order();
    bool m_reordering_tree { false };
    QHash<qlonglong, QPointer<Tab>> m_tree_tabs;
    void tab_settings_changed() override { queue_snapshot(); }
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    std::unique_ptr<QSettings> m_shell_preferences;
    std::unique_ptr<WebView::Autocomplete> m_autocomplete;
    JsonValue m_address_suggestions;
    JsonValue m_pending_suggestions;
    QTimer* m_suggestion_timer { nullptr };
    u64 m_suggestion_query { 0 };
    bool m_suggestions_published { false };
    QHash<Tab*, QPair<qint64, String>> m_favicon_cache;
    JsonValue m_credential_autofill;
    u64 m_autofill_generation { 0 };
    Function<bool()> m_autofill_valid;
    QPointer<WebContentView> m_autofill_view;
    QRect m_autofill_anchor;
    void clear_autofill();
    JsonValue m_filter_proposal;
    QPointer<Tab> m_filter_tab;
    JsonValue snapshot();
    ErrorOr<JsonValue> request(JsonValue const&);
    ErrorOr<void> command(JsonValue const&);
    void place_content();
    struct Split { QPointer<Tab> first; QPointer<Tab> second; bool vertical { false }; double ratio { .5 }; };
    QVector<Split> m_splits;
    Split* split_for(Tab*);
    void remove_split(Tab*);
    void save_split(Split const&);
    void restore_splits();
    bool m_placing_content { false };
    void release_observation(Tab*);
    QSet<bool> m_source_scans;
    QHash<bool, QStringList> m_source_watch_paths;
    QFileSystemWatcher* m_source_watcher { nullptr };
    bool m_blocking_log_visible { false };
    QPointer<WebContentView> m_request_log_view;
    QHash<Tab*, QPointer<DerpBirdDevTools>> m_devtools;
    BrowserWindow& m_window;
    WebContentView* m_shell { nullptr };
    QWidget* m_content { nullptr };
    ContentSurfaceState m_surface_state;
    Optional<ContentSurfaceState> m_pending_surface_state;
    DerpBirdSecretClipboard m_secret_clipboard;
    DerpBirdCredentialProposal m_credential_proposal;
    String m_submitted_credential_token;
    QPointer<DerpBirdCredentialFill> m_credential_fill;
    QPointer<DerpBirdScriptInstall> m_script_install;
    QPointer<DerpBirdSourceDownload> m_source_download;
    String m_source_download_token;
    struct Observation {
        QObject* scope { nullptr };
        QPointer<WebContentView> view {};
        Action::Observer* back { nullptr };
        Action::Observer* forward { nullptr };
        Optional<u64> navigation_listener {};
    };
    QHash<Tab*, Observation> m_observed_tabs;
    u64 m_revision { 0 };
    bool m_snapshot_queued { false };
};

} // namespace Ladybird
