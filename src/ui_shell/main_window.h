#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QMainWindow>
#include <QMetaMethod>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include "base/json.h"
#include "ui_shell/title_bar.h"

class QAction;
class QHBoxLayout;
class QTimer;
class QVBoxLayout;

namespace fv {

class AppContext;
class CompactMode;
class FooterBar;
class PageFit;
class SidePanelHost;
class TabbedContentWidget;
class UpdateNoticeButton;

// Menu item ids of the title bar's menus, registered by ShellFeature in appkit/order.h's
// order; MainWindow connects the ones whose behaviour is its own.
namespace menu_id {
inline constexpr char settings[] = "file.settings";
inline constexpr char guidedSetup[] = "file.guided_setup";
inline constexpr char downloads[] = "file.downloads";
inline constexpr char hotkeys[] = "file.hotkeys";
inline constexpr char minimizeToTray[] = "file.minimize_to_tray";
inline constexpr char exit[] = "file.exit";
inline constexpr char sidePanel[] = "view.side_panel";
inline constexpr char historyCalendar[] = "view.history_calendar";
inline constexpr char compactMode[] = "view.compact_mode";
inline constexpr char hostMode[] = "view.host_mode";
inline constexpr char scratchpad[] = "view.scratchpad";
inline constexpr char stats[] = "view.stats";
inline constexpr char meetingDashboard[] = "view.open_meeting_dashboard";
inline constexpr char checkForUpdates[] = "help.check_for_updates";
inline constexpr char about[] = "help.about";
} // namespace menu_id

// The ScreenRegistry keys of the screens the window relays state to (MainWindow._screen_updates).
namespace screen_key {
inline constexpr char upload[] = "upload";
inline constexpr char walkthrough[] = "walkthrough";
inline constexpr char meeting[] = "meeting";
} // namespace screen_key

// The ScreenRegistry group of the tabs that share engine controls and the transcript's collapsed
// state (Quick Record and Upload File: MainWindow.existing_transcription_tabs).
inline constexpr char kTranscriptionGroup[] = "transcription";

// The frameless main window (ui_qt/main_window.py MainWindow): the title bar with File, View
// and Help, the four tabs, and the footer. Tabs come from the context's ScreenRegistry: a
// registered screen whose order is one of appkit/order.h's tab orders fills that tab, built on
// first show when it has a key (and replaying its queued updates), at once otherwise; a tab
// nothing registered keeps an empty placeholder. Menus come from the MenuRegistry.
//
// It remembers its geometry in window_geometry (500 ms after a move or resize, never before
// the first show, never while maximized or minimized) and restores it on any screen the saved
// rectangle touches, clamped to that screen. Without a frame it moves by its title bar and
// resizes from an 8 px margin on every edge. Closing it hides it to the tray when
// minimize_tray is on and a tray exists.
//
// Features plug their widgets in through host APIs rather than editing the window: a context
// side panel beside the tabs (setSidePanel), the compact recording controller
// (setCompactController), elastic pages (setPageElastic). The window fits its height to the open
// page, relays the recording state to the screens, and shows transient statuses.
// Affinity: UI actor.
class MainWindow : public QMainWindow, public FramelessWindowHost {
    Q_OBJECT
    friend class CompactMode;
    friend class PageFit;
    friend class SidePanelHost;

public:
    // Window-local shortcuts; the global hotkeys work even when the app is unfocused.
    static constexpr char kHistoryShortcut[] = "Ctrl+H";
    static constexpr char kHistoryCalendarShortcut[] = "Ctrl+Shift+D";
    static constexpr char kCompactShortcut[] = "Ctrl+Shift+C";
    static constexpr char kHostShortcut[] = "Ctrl+Shift+H";
    static constexpr char kQuitShortcut[] = "Ctrl+Q";
    // The edge band a press resizes the window from, in pixels.
    static constexpr int kResizeMargin = 8;
    static constexpr int kGeometrySaveDelayMs = 500;
    // What the status returns to after a transient one.
    static constexpr char kReadyStatus[] = "Ready to record";

    // Builds the window and fills its tabs and menus from the context's registries at once.
    explicit MainWindow(AppContext& context, QWidget* parent = nullptr);
    // Builds the chrome only, so features can reach the window (ctx.service<MainWindow>()) while
    // they install; completeSetup() fills the tabs and menus once every feature has registered.
    struct DeferSetup {};
    MainWindow(AppContext& context, DeferSetup, QWidget* parent = nullptr);
    ~MainWindow() override;
    // Fills the tabs and menus from the registries, restores the geometry and Compact Mode. Runs
    // once; later calls do nothing.
    void completeSetup();
    bool setupComplete() const { return setupComplete_; }

    AppContext& context() const { return context_; }
    TabbedContentWidget* tabbedContent() const { return tabbedContent_; }
    TitleBar* titleBar() const { return titleBar_; }
    FooterBar* footer() const { return footer_; }
    UpdateNoticeButton* updateNotice() const { return updateNotice_; }
    // The page the stack holds at tab index (a placeholder until its screen is built).
    QWidget* page(int tab) const;

    // MainWindow.update_screen: kept until the screen is built, applied now when it exists.
    void updateScreen(std::string_view key, std::string method, std::function<void(QWidget*)>);
    // The status line of Quick Record and of the compact controller: each gets
    // setStatus(QString) when it declares that method (a slot or Q_INVOKABLE); Quick Record
    // through the ScreenRegistry when its tab is keyed, so a page built later replays it.
    void setStatus(const QString& status);
    QString status() const { return status_; }
    // Shows status, then "Ready to record" after `duration` (the History panel's "Copied to
    // clipboard" and "Entry deleted"). Each call schedules its own revert, as Python's did.
    void showTransientStatus(const QString& status,
                             std::chrono::milliseconds duration = std::chrono::milliseconds{2000});

    // --- Context side panel (A01-47) ---
    // Inserts edgeTab and panel beside the tabs. panel must implement SidePanel and edgeTab (may
    // be null) SidePanelEdgeTab (ui_shell/side_panel_host.h); a QAbstractButton edge tab toggles
    // the panel when clicked. The window does not reach into the panel otherwise: the panel's
    // owner connects its per-frame width signal to sidePanelWidthAnimated, sidePanelRefreshDue
    // to its refresh and sidePanelPageRequested to its page choice, and opens on the page for
    // tabbedContent()->currentIndex(). Throws std::invalid_argument for a panel that is not a
    // SidePanel, std::logic_error for a second panel.
    void setSidePanel(QWidget* panel, QWidget* edgeTab);
    QWidget* sidePanel() const;
    QWidget* sidePanelEdgeTab() const;
    // The View menu's panel item, Ctrl+H and the edge tab. Leaves Compact Mode first (and stops
    // there when the panel is already open); refused in Host Mode.
    void toggleSidePanel();
    // The window grows and shrinks in lockstep with the panel's animated width, keeping the
    // page's width, as far as the screen allows.
    void sidePanelWidthAnimated(int width);
    bool isSidePanelExpanded() const;

    // View → Host Mode shows a supplied dashboard without starting the host.
    void setHostDashboardFactory(std::function<QWidget*(QWidget*)>);
    void setHostMode(bool enabled, bool persist = true);
    bool isHostMode() const { return hostMode_; }
    // UIController.switch_to_tab: a tab the app opens (a meeting starting here) needs the tabs,
    // so Host Mode is left first.
    void switchToTab(int index);

    // --- Compact Mode (A01-52) ---
    // The compact recording controller, shown in place of the tabs in Compact Mode. It gets
    // setRecordingState(bool) and setStatus(QString) when it declares them.
    void setCompactController(QWidget* controller);
    QWidget* compactController() const;
    // Refused while Host Mode is on; persist saves compact_mode.
    void setCompactMode(bool compact, bool persist = true);
    bool isCompactMode() const override;
    // The View menu's Compact Mode (Ctrl+Shift+C).
    void toggleCompactMode();
    // Applies a saved compact_mode (without saving it again).
    void restoreCompactMode();

    // --- Recording state relay (A01-57) ---
    // Forwards the recording state to Quick Record (setRecordingActive(bool)), the compact
    // controller, and the Upload File, Walkthrough and Meeting Mode screens
    // (set_recording_active / set_recording_state through ScreenRegistry::update, calling
    // setRecordingActive(bool) and setRecordingState(bool, bool) where declared). A recording
    // that starts brings the window to Quick Record, unless a walkthrough runs in the full
    // window.
    void setRecordingActive(bool active);
    bool isRecording() const { return recording_; }
    // Whether the recording in progress is a walkthrough, which its own tab then finishes; it
    // brings the window to that tab outside Compact Mode.
    void setWalkthroughActive(bool active);
    bool isWalkthroughActive() const { return walkthroughActive_; }

    // --- Page fit (A01-48, A01-49) ---
    // A transcript card was collapsed or expanded by delta pixels: the other transcription tabs
    // follow (setTranscriptionCollapsed(bool)), and the window refits, or on the Upload tab
    // moves by delta with the card's own animation. Connect the tab's signal straight to this
    // method: the sender is the tab that is not synced. An open transcript makes every
    // transcription tab elastic (fills_height), without a setPageElastic call.
    void onTranscriptionCollapsed(bool collapsed, int delta);
    // A result's stats card was shown (visible) or hidden: once the window has finished
    // growing for it (kStatsRevealDelayMs), the card is scrolled into view below the
    // transcript, 12 px clear of the viewport's edge, where the window cannot grow enough
    // (a short screen, an open transcript's height). Skipped in Host Mode, whose dashboard is
    // not the result's page.
    void onStatsVisibilityChanged(bool visible, QWidget* stats);
    // SECTION_COLLAPSE_DURATION_MS + 50: the card's section has finished opening.
    static constexpr int kStatsRevealDelayMs = 350;
    static constexpr int kStatsRevealMargin = 12;
    // Pages call it after a content change; a burst coalesces into one fit.
    void schedulePageFit();
    // Fits now, even when the page has not changed.
    void refitPage(bool animate = true);
    // An elastic page (an open transcript, a storyboard) keeps a height the user drags.
    void setPageElastic(QWidget* page, bool elastic);
    bool isPageElastic(QWidget* page) const;
    // The tallest the window may fit to (_max_usable_height): the available height of the
    // window's screen. Replaceable, as tests pin the screen.
    std::function<int()> maxUsableHeight;
    PageFit& pageFit() const { return *pageFit_; }
    void setTrayAvailable(bool available);
    bool trayAvailable() const { return trayAvailable_; }
    void setUpdateNotice(const QString& version);
    // The hotkeys' display texts. Until the pages that show them exist (Quick Record, the
    // compact controller, Walkthrough) only the footer's tray button takes its hint.
    void updateHotkeys(const QString& record, const QString& cancel, const QString& enableDisable,
                       const QString& minimize, const QString& walkthrough,
                       const QString& screenshot);
    void minimizeToTray();
    void restoreFromTray();
    void toggleTrayVisibility();
    // Quits after confirmQuit agrees; MainWindow.request_quit.
    void requestQuit();
    // Saves the geometry and quits the application, past the close-to-tray rule.
    void quitApplication();
    void openSettings();
    void openDownloads();
    void openHotkeySettings();
    void checkForUpdates();
    void showAbout();

    // Asked before a quit the user requested; false keeps the app open. Set by M1.16.
    std::function<bool()> confirmQuit = [] {
        return true;
    };

    // Geometry (MainWindow._clamp_geometry, _available_screen_rect, _restore_window_geometry,
    // _save_geometry). availableScreenRect is replaceable, as tests pin the screen.
    QRect clampGeometry(int x, int y, int width, int height,
                        std::optional<QRect> available = std::nullopt) const;
    std::function<QRect()> availableScreenRect;
    // The available areas of the connected screens, the primary first (a saved geometry returns
    // to the first one it overlaps); replaceable, as tests give a second monitor.
    std::function<std::vector<QRect>()> screenAreas;
    void restoreWindowGeometry();
    void saveGeometryNow();
    void scheduleGeometrySave();
    bool geometrySaveScheduled() const;
    bool initialShowComplete() const { return initialShowComplete_; }
    // Lets a test stand for a window that has not been shown yet.
    void setInitialShowComplete(bool complete) { initialShowComplete_ = complete; }

    // FramelessWindowHost.
    ResizeEdge resizeEdge(QPoint local) const override;
    void beginResize(ResizeEdge edge, QPoint global) override;
    void applyResizeDelta(QPoint global) override;
    void finishResize() override;
    bool isResizing() const override { return resizing_; }
    void leaveMaximizedInPlace() override;

    // Whether a signal of this window has a receiver, for the "connects every signal" check
    // (architecture 8.5).
    bool hasReceivers(const QMetaMethod& signal) const { return isSignalConnected(signal); }

Q_SIGNALS:
    // Python's names in camelCase (main_window.py:378-421). Those without an emitter in M0 are
    // declared for the features that wire them.
    void recordToggled(bool recording);
    void recordCanceled();
    void walkthroughRequested();
    // A capture folder to open the walkthrough library at; empty for the newest.
    void walkthroughLibraryRequested(const QString& folder);
    void walkthroughsChanged();
    void modelChanged(const QString& model);
    void whisperEngineChanged();
    void remoteModelSelected(const QString& family, const QString& model);
    void remoteRuntimeSelected(const QString& family, const QString& model,
                               const QVariantMap& runtime);
    void remoteRetryRequested();
    void livePreviewChanged();
    void cleanupToggled();
    void settingsRequested();
    void guidedSetupRequested();
    // A Settings destination key (or legacy alias) to open.
    void settingsDestinationRequested(const QString& destination);
    void engineHelpRequested(const QString& topic);
    void hotkeysRequested();
    void aboutRequested();
    void checkForUpdatesRequested();
    // The title bar's update notice was clicked.
    void updateNoticeRequested();
    void retranscribeRequested(const QString& audioPath);
    void uploadFileRequested(const QString& path, double seconds);
    // A BatchUploadRequest (M2 gives the payload its type).
    void uploadFilesRequested(const QVariant& request);
    void uploadCancelRequested();
    void uploadCopyRequested(const QString& text);
    void quickRecordCopyRequested(const QString& text);
    void meetingDashboardRequested();
    void scratchpadRequested();
    void statsRequested();
    void hostModelSelected(const QString& family, const QString& model);
    void pastMeetingRequested(const QString& meetingId);
    void pastMeetingCopyRequested(const QString& meetingId);
    void pastMeetingDeleteRequested(const QString& meetingId, bool deleteAudio);
    void pastMeetingsClearRequested(bool deleteAudio);
    void screenCreated(const QString& key, QWidget* screen);

    // The window's own host signals, after Python's.
    // 75 ms after a tab switch, when the side panel is open: the panel refreshes its page.
    void sidePanelRefreshDue();
    // The tab switched to; the panel shows History, Past Meetings or Walkthroughs for it.
    void sidePanelPageRequested(int tabIndex);
    void compactModeChanged(bool compact);

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void setupTabs();
    void fillTabs();
    void setupMenus();
    void connectMenuAction(const char* id, std::function<void()> slot);
    void onTabChanged(int index);
    void ensureTab(int tab);
    void placeScreen(std::string_view key, QWidget* screen);
    void adoptPage(QWidget* page);
    // Applies to the tab's page now, or through the ScreenRegistry for a keyed tab.
    void updateTab(int tab, std::string method, std::function<void(QWidget*)> apply);
    void updateRecordingState();
    QWidget* currentPage() const;
    QWidget* quickRecordPage() const;
    // Quick Record or a screen of kTranscriptionGroup (existing_transcription_tabs).
    bool isTranscriptionTab(const QWidget* page) const;
    bool applySavedGeometry(const Json& geometry);
    void centerOnScreen();
    QRect currentScreenRect() const;
    void updateCursorForEdge(ResizeEdge edge);
    void onSidePanelAnimationFinished();
    void revealShownStats();

    AppContext& context_;
    TitleBar* titleBar_ = nullptr;
    UpdateNoticeButton* updateNotice_ = nullptr;
    QWidget* contentWrapper_ = nullptr;
    QHBoxLayout* contentLayout_ = nullptr;
    QWidget* mainArea_ = nullptr;
    QVBoxLayout* mainAreaLayout_ = nullptr;
    TabbedContentWidget* tabbedContent_ = nullptr;
    FooterBar* footer_ = nullptr;
    // The screen key each tab is built from, empty for a tab nothing registered.
    std::array<std::string, 4> tabKeys_;
    // Children of the window, made before any event can reach it.
    PageFit* pageFit_ = nullptr;
    SidePanelHost* sidePanelHost_ = nullptr;
    CompactMode* compactMode_ = nullptr;

    bool setupComplete_ = false;
    bool trayAvailable_ = true;
    bool forceQuit_ = false;
    bool initialShowComplete_ = false;
    bool hostMode_ = false;
    std::function<QWidget*(QWidget*)> hostDashboardFactory_;
    QPointer<QWidget> hostDashboard_;
    std::optional<QRect> recordingGeometry_;
    QString status_;
    bool recording_ = false;
    // What Quick Record was last told, to see a recording start (quick_record_tab.is_recording).
    bool pageRecording_ = false;
    bool walkthroughActive_ = false;

    bool resizing_ = false;
    ResizeEdge resizeEdge_;
    QPoint resizeStartPos_;
    std::optional<QRect> resizeStartGeometry_;
    QTimer* geometrySaveTimer_ = nullptr;
    // A result's stats card, scrolled into view once statsRevealTimer_ fires.
    QPointer<QWidget> statsToReveal_;
    QTimer* statsRevealTimer_ = nullptr;
};

} // namespace fv
