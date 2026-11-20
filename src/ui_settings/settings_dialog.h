#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <QDialog>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>

#include "base/json.h"
#include "ui_settings/settings_binder.h"

class QLabel;
class QMenu;
class QPushButton;
class QScrollArea;
class QShortcut;
class QSplitter;
class QStackedWidget;
class QTimer;

namespace fv {

class AppContext;
class Button;
class NavRail;
class SettingsDeepLinks;
class SettingsPage;
class SettingsPageModule;
struct SettingsPageSpec;
class WrappedLabel;

// The one non-modal window for settings, model choices and downloads
// (ui_qt/dialogs/settings_dialog.py SettingsDialog, object name "settingsDialog"): a header with
// the app mark, the product name, "Settings", the narrow-window page menu and the search button;
// the rail (NavRail) beside the page area (chapter crumb, title, subtitle, the stacked pages);
// and a footer with "Changes save automatically", the shared message line and "Done".
//
// The pages are the context's SettingsPageRegistry, read once when the window is built: the rail
// lists Home, then each chapter of destinations::rail with at least one registered page (a page
// the rail does not list sits after Home). A page is built on first selection, then loaded with
// the loading flag set, so nothing it does while loading is saved. The window opens on Home (on
// the first registered page when Home is not registered) and is reused, never stacked.
//
// Built pages that derive from SettingsPageModule get its hooks: loadSettings when built and on
// refresh(), refreshPage on refresh() and refreshPage(key) (Home's on a rail redraw while it is
// shown, as Python rebuilds Home there), cancelCapture when another page is selected and on
// close, saveDrafts on switching (silently) and on Esc or close (asking while visible),
// onShown when selected, namedControl for control(). Whatever a hook throws is logged and
// swallowed, so one page's failure cannot stop Settings opening, switching or closing.
// Affinity: GUI thread.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    // Client area; with the title bar about 1317x882 on a 1080p screen.
    static constexpr QSize kDefaultSize{1315, 850};
    static constexpr QSize kMinimumSize{940, 520};
    // Narrower than this at 100 % text, the rail folds into the header's page menu.
    static constexpr int kNarrowWidth = 800;
    // Room left above the window for a title bar the window manager has not drawn yet.
    static constexpr int kTitleBarAllowance = 32;
    static constexpr int kSearchFlashMs = 1600;

    explicit SettingsDialog(AppContext& context, QWidget* parent = nullptr);
    ~SettingsDialog() override;

    AppContext& context() const { return context_; }

    // --- Navigation ---
    // Shows one destination by key or legacy alias. basic_dictation, basic_meetings and
    // basic_app select Home and reveal its card (Home's namedControl of that key); an unknown
    // key is ignored, and so is anything but Backup while a backup runs.
    void selectDestination(const QString& key);
    // Home, scrolled to the top.
    void showHome();
    // API keys, on the credential env names ("" keeps the current one).
    void focusApiKeys(const QString& env = {});
    // Downloads with its Hugging Face policy control (named "hf_policy_combo") focused.
    void focusHfPolicy();
    // AI cleanup, revealing its model tile (named "cleanup_model_tile").
    void focusCleanupModel();
    // Downloads, filtered to one backend and/or focused on one component.
    void showDownloads(const QString& backend = {}, const QString& componentId = {});
    QString currentDestination() const;

    // --- Pages ---
    // Builds one destination once, then loads its saved values; returns its SettingsPage. A
    // page built while another builds loads only its own bindings and never saves. Throws
    // std::invalid_argument for a destination with no registered page; what the page's create
    // throws propagates and leaves the page unbuilt.
    SettingsPage* ensurePage(std::string_view key);
    // Builds every registered page (full-window layout checks).
    void ensureAllPages();
    // The control a page exposes under name (CONTROL_DESTINATIONS, then every built page),
    // building its page first; null when none does or its page is still being built.
    QWidget* control(std::string_view name);
    // The registered destinations, in rail order.
    std::vector<std::string> pageKeys() const;
    bool isRegistered(std::string_view key) const;
    bool isBuilt(std::string_view key) const;
    // The page container of a destination; null when it is not registered.
    SettingsPage* page(std::string_view key) const;
    // The shared scroll area holding the page; null for a page that scrolls itself.
    QScrollArea* pageScroll(std::string_view key) const;
    // The widget the page's create returned; null until built.
    QWidget* pageContent(std::string_view key) const;
    // The page's title and subtitle as registered.
    QString pageTitle(std::string_view key) const;
    QString pageSubtitle(std::string_view key) const;

    // --- Saving ---
    // _persist: does nothing while loading (false). When the stored raw value already equals
    // value (Python ==) only the rail is redrawn; otherwise the key is saved and message shown
    // (cleared when empty). A failure shows "Couldn't save setting: <error>" and returns false.
    bool persist(std::string_view key, const Json& value, const QString& message = {});
    // _persist_many: update(updates) dropping drops; "Couldn't save settings: <error>".
    bool persistMany(const Json& updates, std::vector<std::string> drops = {});
    // The binders whose controls only save their key (_bindings for every page,
    // _meeting_bindings for the Meeting Mode pages); both persist through persist().
    SettingsBinder& bindings() { return bindings_; }
    SettingsBinder& meetingBindings() { return meetingBindings_; }
    bool isLoading() const { return loading_; }
    // Whether key's page loads now: built, and either nothing or key is being built.
    bool isPageLoading(std::string_view key) const;
    // The snapshot the current refresh or rail redraw reads, else the store's.
    std::shared_ptr<const Json> settingsSnapshot() const;

    // --- Messages and refresh ---
    void showMessage(const QString& text);
    QString messageText() const;
    // Reloads every built page from one snapshot, refreshes the built pages, then redraws the
    // rail once.
    void refresh();
    // Re-reads one built page's own state (its refreshPage), then redraws the rail.
    void refreshPage(std::string_view key);
    // Redraws the rail values; inside a batch, once when the outermost batch ends.
    void refreshRailValues();
    void beginRailBatch();
    void endRailBatch();
    // Redraws the rail, then says what changed: "transforms", "hotkeys", "dictionary",
    // "snippets", "styles", "languages", "microphones", "updates", "actions".
    void notifyChanged(const QString& kind);

    // --- Locks, reveal, search and hotkeys ---
    // While a backup or restore runs: Backup is selected, the rail, the search button, the page
    // menu and the search shortcuts are disabled, the palette is closed, and navigation
    // anywhere else is ignored.
    void setBackupBusy(bool busy);
    bool isBackupBusy() const { return backupBusy_; }
    // Scrolls target into view once the event loop has laid the page out, tints its card
    // (settingsTile, textModelFootnoteCard or homeCard) through searchHit for 1.6 s, and
    // focuses the first enabled, visible, tab-focusable widget in it.
    void reveal(QWidget* target);
    // The search palette's opener (Ctrl+K, Find, the search button); none until one is set.
    void setSearchOpener(std::function<void(QString)> opener);
    // Closes the palette (on close and when a backup starts).
    void setSearchCloser(std::function<void()> closer);
    void openSearch(const QString& text = {});
    // The Hotkeys page's save path for one standard shortcut: returns "" once saved, else the
    // reason, which the handler also shows. Without a handler the Hotkeys page is built first
    // (it installs one); still none, the reason says the shortcut could not be saved.
    void setStandardHotkeyHandler(std::function<QString(QString, QString)> handler);
    QString setStandardHotkey(const QString& action, const QString& hotkey);
    // Pauses global hotkeys while a page captures a shortcut, and resumes them.
    void setHotkeyCaptureSuspended(bool suspended);
    // While a page holds something Esc should cancel (a shortcut capture, a dictated rule), it
    // sets what Esc does instead of closing Settings; {} gives Esc back.
    void setEscapeOverride(std::function<void()> cancel);
    bool hasEscapeOverride() const { return static_cast<bool>(escapeOverride_); }

    // --- Shell parts, for pages and tests ---
    NavRail* rail() const { return rail_; }
    QStackedWidget* stack() const { return stack_; }
    QLabel* pageCrumb() const { return crumb_; }
    QLabel* pageTitleLabel() const { return title_; }
    WrappedLabel* pageSubtitleLabel() const { return subtitle_; }
    WrappedLabel* messageLabel() const { return message_; }
    QPushButton* searchButton() const { return searchButton_; }
    QPushButton* pageMenuButton() const { return pageMenuButton_; }
    QSplitter* splitter() const { return splitter_; }
    QWidget* railPane() const { return railPane_; }
    Button* closeButton() const { return closeButton_; }
    QLabel* brandLabel() const { return brand_; }
    QLabel* settingsLabel() const { return settingsLabel_; }
    QLabel* searchText() const { return searchText_; }
    bool isNarrow() const { return narrow_.value_or(false); }
    // Lists every page by chapter under the page menu button: Home, then per chapter a
    // separator, the disabled chapter name and checkable pages, the current one checked.
    QMenu* showPageMenu();
    QMenu* pageMenu() const;
    // The window's first-show fit (_fit_to_screen) and placement (_place_on_screen); available
    // defaults to availableScreenRect(), else to the available geometry of the parent's screen,
    // else of the window's own.
    void fitToScreen();
    void placeOnScreen(std::optional<QRect> available = std::nullopt);
    // Stands in for the screen's available geometry when set (a test's screen).
    std::function<QRect()> availableScreenRect;

    bool event(QEvent* event) override;

public Q_SLOTS:
    void reject() override;

Q_SIGNALS:
    // notifyChanged's kind (on_settings_changed).
    void settingsChanged(const QString& kind);
    // setHotkeyCaptureSuspended (on_profile_hotkey_capture).
    void hotkeyCaptureSuspended(bool suspended);
    // A destination was shown.
    void destinationChanged(const QString& key);
    // The window closed (Done, Esc, the frame's close).
    void closed();
    // The window was shown other than by the system: pages that rescan on each opening (models,
    // downloads, recordings usage) do it here, inside one rail batch.
    void windowShown();

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct Page;

    QWidget* buildHeader();
    QWidget* buildRailPane();
    QWidget* buildBody();
    void addPage(const SettingsPageSpec& spec);
    Page* findPage(std::string_view key) const;
    void onDestinationChanged(const QString& key);
    void loadSettings();
    void renderRailValues(const Json& settings);
    void cancelPageCaptures(std::string_view keep = {});
    bool savePageDrafts(std::string_view keep = {}, bool ask = false);
    void runReveal();
    std::optional<QRect> availableGeometry() const;
    void clearSearchFlash();
    void applyPageMenuIcon();
    SettingsPageModule* moduleOf(std::string_view key) const;
    SettingsDeepLinks* linksOf(std::string_view key) const;

    AppContext& context_;
    SettingsBinder bindings_;
    SettingsBinder meetingBindings_;

    QLabel* brand_ = nullptr;
    QLabel* settingsLabel_ = nullptr;
    QPushButton* pageMenuButton_ = nullptr;
    QPushButton* searchButton_ = nullptr;
    QLabel* searchText_ = nullptr;
    QLabel* searchHint_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QWidget* railPane_ = nullptr;
    NavRail* rail_ = nullptr;
    QLabel* crumb_ = nullptr;
    QLabel* title_ = nullptr;
    WrappedLabel* subtitle_ = nullptr;
    WrappedLabel* message_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    Button* closeButton_ = nullptr;
    QPointer<QMenu> pageMenu_;
    std::vector<QShortcut*> searchShortcuts_;
    QTimer* flashTimer_ = nullptr;
    QTimer* revealTimer_ = nullptr;
    QPointer<QWidget> flash_;
    QPointer<QWidget> revealTarget_;

    std::vector<std::unique_ptr<Page>> pages_;
    std::set<std::string, std::less<>> building_;
    bool loading_ = false;
    std::optional<std::string> initializing_;
    int bindingStart_ = 0;
    int meetingBindingStart_ = 0;
    std::shared_ptr<const Json> refreshSnapshot_;
    int railBatchDepth_ = 0;
    bool railRefreshPending_ = false;
    bool backupBusy_ = false;
    bool pagesReady_ = false;
    bool closing_ = false;
    std::optional<bool> narrow_;

    std::function<void(QString)> searchOpener_;
    std::function<void()> searchCloser_;
    std::function<QString(QString, QString)> standardHotkeyHandler_;
    std::function<void()> escapeOverride_;
};

} // namespace fv
