#include "ui_shell/main_window.h"

#include <algorithm>
#include <exception>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractAnimation>
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMetaType>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/order.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/identity.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_shell/compact_mode.h"
#include "ui_shell/footer_bar.h"
#include "ui_shell/page_fit.h"
#include "ui_shell/side_panel_host.h"
#include "ui_theme/local_style.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/update_notice.h"
#include "uimath/window_fit.h"
#include "uimath/window_geometry.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.main_window"};

// config.MAIN_WINDOW_HOST_DEFAULT_WIDTH and _HEIGHT: the least a first Host Mode window grows to.
constexpr int kHostDefaultSize = 780;

// When no screen is known (main_window.py:1863).
const QRect kFallbackScreen(0, 0, 1280, 800);

const QString kCentralWidgetStyle = QStringLiteral(R"(
            QWidget#centralWidget {
                border: 1px solid @border;
            }
        )");

// The tab an appkit/order.h tab order fills, or -1.
int tabForOrder(int order) {
    switch (order) {
    case order::tab::quick_record:
        return TabbedContentWidget::TabQuickRecord;
    case order::tab::upload_file:
        return TabbedContentWidget::TabUploadFile;
    case order::tab::walkthrough:
        return TabbedContentWidget::TabWalkthrough;
    case order::tab::meeting_mode:
        return TabbedContentWidget::TabMeetingMode;
    default:
        return -1;
    }
}

// Python's n // 2, which floors where C++ division truncates (a window wider than the screen).
int floorHalf(int n) {
    return n >= 0 ? n / 2 : -((-n + 1) / 2);
}

Rect toRect(const QRect& rect) {
    return Rect{rect.x(), rect.y(), rect.width(), rect.height()};
}

// Python's repr() of the saved geometry, for its log line: a dict of ints, bools and strings.
std::string pythonRepr(const Json& value) {
    if (value.is_object()) {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, item] : value.items()) {
            out += (first ? "" : ", ") + pythonRepr(Json(key)) + ": " + pythonRepr(item);
            first = false;
        }
        return out + "}";
    }
    if (value.is_array()) {
        std::string out = "[";
        bool first = true;
        for (const auto& item : value) {
            out += (first ? "" : ", ") + pythonRepr(item);
            first = false;
        }
        return out + "]";
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        const char quote =
            text.find('\'') != std::string::npos && text.find('"') == std::string::npos ? '"'
                                                                                        : '\'';
        std::string out(1, quote);
        for (const char c : text) {
            if (c == '\\' || c == quote) {
                out += '\\';
            }
            out += c;
        }
        return out + quote;
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_null()) {
        return "None";
    }
    return value.dump();
}

// The pages and the controller the window relays to are built by later features, so it knows
// them only by Python's method names in Qt's spelling. A target that does not declare the
// method (as a slot or Q_INVOKABLE) is skipped.
bool declares(const QObject* target, const QByteArray& signature) {
    return target != nullptr && target->metaObject()->indexOfMethod(
                                    QMetaObject::normalizedSignature(signature.constData())) >= 0;
}

template <class... Args> void callDeclared(QObject* target, const char* name, Args... args) {
    QByteArray signature(name);
    signature += '(';
    bool first = true;
    for (const char* type : {QMetaType::fromType<Args>().name()...}) {
        signature += first ? "" : ",";
        signature += type;
        first = false;
    }
    signature += ')';
    if (!declares(target, signature)) {
        if (target != nullptr) {
            kLog.debug("{} has no {}", target->metaObject()->className(), signature.toStdString());
        }
        return;
    }
    QMetaObject::invokeMethod(target, name, Qt::DirectConnection, args...);
}

} // namespace

MainWindow::MainWindow(AppContext& context, QWidget* parent)
    : MainWindow(context, DeferSetup{}, parent) {
    completeSetup();
}

MainWindow::MainWindow(AppContext& context, DeferSetup, QWidget* parent)
    : QMainWindow(parent), context_(context) {
    // Before the first resize: resizeEvent consults them.
    pageFit_ = new PageFit(*this);
    sidePanelHost_ = new SidePanelHost(*this);
    compactMode_ = new CompactMode(*this);

    setWindowTitle(qt::toQString(identity().product));
    // The explicit Window type flag stays: setWindowFlags() replaces every flag, and a bare
    // FramelessWindowHint drops the top-level Window type (on macOS such a window cannot come
    // back from hide()).
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setMinimumSize(window_size::kMinWidth, window_size::kMinHeight);
    setMaximumWidth(window_size::kMaxWidth);
    resize(window_size::kDefaultWidth, window_size::kDefaultHeight);

    availableScreenRect = [this] {
        return currentScreenRect();
    };
    screenAreas = [] {
        std::vector<QRect> areas;
        QScreen* primary = QGuiApplication::primaryScreen();
        if (primary != nullptr) {
            areas.push_back(primary->availableGeometry());
        }
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen != primary) {
                areas.push_back(screen->availableGeometry());
            }
        }
        return areas;
    };
    maxUsableHeight = [this] {
        QScreen* screen = QGuiApplication::screenAt(geometry().center());
        if (screen == nullptr) {
            screen = QGuiApplication::primaryScreen();
        }
        return screen != nullptr ? screen->availableGeometry().height() : kUnlimitedHeight;
    };

    setupUi();

    setMouseTracking(true);
    // Edge cursors follow the pointer over any child, which the window's own mouse events miss.
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow() {
    context_.screens().setPlacer({});
}

void MainWindow::completeSetup() {
    if (setupComplete_) {
        return;
    }
    setupComplete_ = true;
    fillTabs();
    setupMenus();
    restoreWindowGeometry();
    restoreCompactMode();
    if (hostDashboardFactory_ && context_.settings().get(keys::host_mode)) {
        setHostMode(true, false);
    }
}

void MainWindow::setupUi() {
    auto* central = new QWidget;
    setCentralWidget(central);
    ui::setLocalStyleSheet(central, kCentralWidgetStyle);
    central->setObjectName(QStringLiteral("centralWidget"));
    central->setMouseTracking(true);

    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(1, 1, 1, 1); // room for the 1 px border
    outer->setSpacing(0);

    titleBar_ = new TitleBar(this, this);
    outer->addWidget(titleBar_);
    updateNotice_ = new UpdateNoticeButton;
    connect(updateNotice_, &QPushButton::clicked, this, &MainWindow::updateNoticeRequested);
    titleBar_->addNotice(updateNotice_);

    contentWrapper_ = new QWidget;
    contentLayout_ = new QHBoxLayout(contentWrapper_);
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(0);
    outer->addWidget(contentWrapper_, 1);

    mainArea_ = new QWidget;
    mainAreaLayout_ = new QVBoxLayout(mainArea_);
    mainAreaLayout_->setContentsMargins(0, 0, 0, 0);
    mainAreaLayout_->setSpacing(0);

    setupTabs();
    mainAreaLayout_->addWidget(tabbedContent_);
    contentLayout_->addWidget(mainArea_, 1);

    footer_ = new FooterBar;
    connect(footer_->trayButton(), &QPushButton::clicked, this, &MainWindow::minimizeToTray);
    connect(footer_->statsButton(), &QPushButton::clicked, this, &MainWindow::statsRequested);
    connect(footer_->settingsButton(), &QPushButton::clicked, this, &MainWindow::openSettings);
    connect(footer_->quitButton(), &QPushButton::clicked, this, &MainWindow::requestQuit);
    outer->addWidget(footer_);
}

void MainWindow::setupTabs() {
    tabbedContent_ = new TabbedContentWidget(context_.settings());
    for (int tab = 0; tab < static_cast<int>(tabKeys_.size()); ++tab) {
        tabbedContent_->setPage(tab, new QWidget);
    }
}

void MainWindow::fillTabs() {
    ScreenRegistry& screens = context_.screens();
    std::array<bool, 4> filled{};
    for (const ScreenSpec& spec : screens.screens()) {
        const int tab = tabForOrder(spec.order);
        if (tab < 0) {
            kLog.warning("The screen {} has the order {}, which is no tab of the main window",
                         spec.key, spec.order);
            continue;
        }
        if (filled[tab]) {
            kLog.warning("The screen {} is the second for tab {}; the first one stays", spec.key,
                         tab);
            continue;
        }
        filled[tab] = true;
        if (spec.key.empty()) {
            if (QWidget* screen = spec.create ? spec.create(tabbedContent_->stack()) : nullptr) {
                tabbedContent_->setPage(tab, screen);
                adoptPage(screen);
            }
        } else {
            tabKeys_[tab] = spec.key;
        }
    }
    screens.setPlacer([this](std::string_view key, QWidget* screen) { placeScreen(key, screen); });
    connect(&screens, &ScreenRegistry::screenCreated, this, &MainWindow::screenCreated);

    // After every page exists: the tab bar restored its saved tab before the stack had them.
    tabbedContent_->syncStackWithTabBar();
    connect(tabbedContent_, &TabbedContentWidget::currentChanged, this, &MainWindow::onTabChanged);
    onTabChanged(tabbedContent_->currentIndex());
}

void MainWindow::setupMenus() {
    menuBar()->hide();
    QMenuBar* bar = titleBar_->menuBar();
    MenuRegistry& menus = context_.menus();
    menus.populate(Menu::File, bar->addMenu(QStringLiteral("File")));
    menus.populate(Menu::View, bar->addMenu(QStringLiteral("View")));
    menus.populate(Menu::Help, bar->addMenu(QStringLiteral("Help")));

    connectMenuAction(menu_id::settings, [this] { openSettings(); });
    connectMenuAction(menu_id::guidedSetup, [this] { Q_EMIT guidedSetupRequested(); });
    connectMenuAction(menu_id::downloads, [this] { openDownloads(); });
    connectMenuAction(menu_id::hotkeys, [this] { openHotkeySettings(); });
    connectMenuAction(menu_id::minimizeToTray, [this] { minimizeToTray(); });
    connectMenuAction(menu_id::exit, [this] { requestQuit(); });
    connectMenuAction(menu_id::sidePanel, [this] { toggleSidePanel(); });
    connectMenuAction(menu_id::compactMode, [this] { toggleCompactMode(); });
    connectMenuAction(menu_id::hostMode, [this] { setHostMode(!hostMode_); });
    connectMenuAction(menu_id::scratchpad, [this] { Q_EMIT scratchpadRequested(); });
    connectMenuAction(menu_id::stats, [this] { Q_EMIT statsRequested(); });
    connectMenuAction(menu_id::meetingDashboard, [this] { Q_EMIT meetingDashboardRequested(); });
    connectMenuAction(menu_id::checkForUpdates, [this] { checkForUpdates(); });
    connectMenuAction(menu_id::about, [this] { showAbout(); });

    // The side panel's name for the tab the window opens on, as _setup_menu names it after
    // _on_tab_changed; later tab switches rename it with the panel.
    if (QAction* sidePanel = menus.action(menu_id::sidePanel)) {
        sidePanel->setText(tabbedContent_->currentIndex() == TabbedContentWidget::TabMeetingMode
                               ? QStringLiteral("Past Meetings")
                               : QStringLiteral("History"));
    }
}

void MainWindow::connectMenuAction(const char* id, std::function<void()> slot) {
    if (QAction* action = context_.menus().action(id)) {
        connect(action, &QAction::triggered, this, [slot = std::move(slot)] { slot(); });
    }
}

QWidget* MainWindow::page(int tab) const {
    return tabbedContent_->stack()->widget(tab);
}

// main_window.py:1037-1064; the Meeting Mode intro is the meeting feature's.
void MainWindow::onTabChanged(int index) {
    kLog.debug("Tab changed to index {}", index);
    // A factory that throws must not unwind through Qt's signal.
    try {
        ensureTab(index);
    } catch (const std::exception& error) {
        kLog.error_with(error, std::format("Could not open tab {}", index));
    }

    if (compactMode_->isOn() && index != TabbedContentWidget::TabQuickRecord) {
        setCompactMode(false);
    }

    Q_EMIT sidePanelPageRequested(index);
    sidePanelHost_->tabChanged(index);
    pageFit_->refit();
}

void MainWindow::ensureTab(int tab) {
    if (tab < 0 || tab >= static_cast<int>(tabKeys_.size()) || tabKeys_[tab].empty()) {
        return;
    }
    context_.screens().ensure(tabKeys_[tab], tabbedContent_->stack());
}

void MainWindow::placeScreen(std::string_view key, QWidget* screen) {
    const auto it = std::ranges::find(tabKeys_, key);
    if (it == tabKeys_.end()) {
        return;
    }
    tabbedContent_->setPage(static_cast<int>(it - tabKeys_.begin()), screen);
    adoptPage(screen);
}

void MainWindow::adoptPage(QWidget* page) {
    pageFit_->watch(page);
}

QWidget* MainWindow::currentPage() const {
    return tabbedContent_ != nullptr ? tabbedContent_->stack()->currentWidget() : nullptr;
}

QWidget* MainWindow::quickRecordPage() const {
    return tabbedContent_ != nullptr ? page(TabbedContentWidget::TabQuickRecord) : nullptr;
}

bool MainWindow::isTranscriptionTab(const QWidget* page) const {
    if (page == nullptr) {
        return false;
    }
    if (page == quickRecordPage()) {
        return true;
    }
    const std::vector<QWidget*> tabs = context_.screens().existingInGroup(kTranscriptionGroup);
    return std::ranges::find(tabs, page) != tabs.end();
}

void MainWindow::updateTab(int tab, std::string method, std::function<void(QWidget*)> apply) {
    if (!tabKeys_[tab].empty()) {
        context_.screens().update(tabKeys_[tab], std::move(method), std::move(apply));
        return;
    }
    if (QWidget* target = page(tab)) {
        apply(target);
    }
}

void MainWindow::updateScreen(std::string_view key, std::string method,
                              std::function<void(QWidget*)> apply) {
    context_.screens().update(key, std::move(method), std::move(apply));
}

// main_window.py:1181-1183.
void MainWindow::setStatus(const QString& status) {
    status_ = status;
    updateTab(TabbedContentWidget::TabQuickRecord, "set_status",
              [status](QWidget* page) { callDeclared(page, "setStatus", status); });
    callDeclared(compactController(), "setStatus", status);
}

// main_window.py:2064-2071.
void MainWindow::showTransientStatus(const QString& status, std::chrono::milliseconds duration) {
    setStatus(status);
    QTimer::singleShot(duration, this, [this] { setStatus(QString::fromUtf8(kReadyStatus)); });
}

// main_window.py:1154-1169.
void MainWindow::setRecordingActive(bool active) {
    recording_ = active;
    updateRecordingState();
}

void MainWindow::updateRecordingState() {
    const bool started = recording_ && !pageRecording_;
    pageRecording_ = recording_;
    const bool recording = recording_;
    updateTab(TabbedContentWidget::TabQuickRecord, "set_recording_active",
              [recording](QWidget* page) { callDeclared(page, "setRecordingActive", recording); });
    if (QWidget* controller = compactController()) {
        callDeclared(controller, "setRecordingState", recording);
        callDeclared(controller, "setStatus",
                     recording ? QStringLiteral("Recording in progress...")
                               : QString::fromUtf8(kReadyStatus));
    }

    const bool walkthrough = walkthroughActive_;
    updateScreen(screen_key::walkthrough, "set_recording_state",
                 [recording, walkthrough](QWidget* screen) {
                     callDeclared(screen, "setRecordingState", recording, walkthrough);
                 });
    updateScreen(screen_key::upload, "set_recording_active", [recording](QWidget* screen) {
        callDeclared(screen, "setRecordingActive", recording);
    });
    updateScreen(screen_key::meeting, "set_recording_active", [recording](QWidget* screen) {
        callDeclared(screen, "setRecordingActive", recording);
    });
    if (started && (!walkthroughActive_ || isCompactMode())) {
        tabbedContent_->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    }
}

// main_window.py:1171-1179.
void MainWindow::setWalkthroughActive(bool active) {
    walkthroughActive_ = active;
    const bool recording = recording_;
    updateScreen(screen_key::walkthrough, "set_recording_state",
                 [recording, active](QWidget* screen) {
                     callDeclared(screen, "setRecordingState", recording, active);
                 });
    // Bring the walkthrough's controls forward when it starts in full mode.
    if (walkthroughActive_ && !isCompactMode()) {
        tabbedContent_->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    }
}

// main_window.py:1421-1428. The card comes as an argument: the Upload tab relays its card's
// signal, so the sender is not always the card.
void MainWindow::onStatsVisibilityChanged(bool visible, QWidget* stats) {
    if (hostMode_) {
        // A dictation finished behind the dashboard, which is not its page.
        return;
    }
    if (visible && stats != nullptr) {
        statsToReveal_ = stats;
        if (statsRevealTimer_ == nullptr) {
            statsRevealTimer_ = new QTimer(this);
            statsRevealTimer_->setSingleShot(true);
            statsRevealTimer_->setInterval(kStatsRevealDelayMs);
            connect(statsRevealTimer_, &QTimer::timeout, this, &MainWindow::revealShownStats);
        }
        statsRevealTimer_->start();
    }
}

// main_window.py:1430-1451: the window grows to fit the card like any other row, but not past
// the screen, past an open transcript's height, or where the compositor sizes it, which left the
// card under the tab's fold, behind the footer.
void MainWindow::revealShownStats() {
    QPointer<QWidget> stats = statsToReveal_;
    statsToReveal_ = nullptr;
    if (stats == nullptr || !stats->isVisible()) {
        return;
    }
    QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (stats == nullptr) {
        return; // The tab went while its layout settled.
    }
    for (QWidget* area = stats->parentWidget(); area != nullptr; area = area->parentWidget()) {
        if (auto* scroll = qobject_cast<QScrollArea*>(area)) {
            scroll->ensureWidgetVisible(stats, 0, kStatsRevealMargin);
        }
    }
}

// main_window.py:1225-1260.
void MainWindow::onTranscriptionCollapsed(bool collapsed, int delta) {
    const auto* source = qobject_cast<const QWidget*>(sender());
    // Every transcription tab now shows the transcript open or collapsed alike.
    pageFit_->setTranscriptOpen(!collapsed);
    context_.screens().syncGroup(
        kTranscriptionGroup, "set_transcription_collapsed",
        [collapsed](QWidget* tab) { callDeclared(tab, "setTranscriptionCollapsed", collapsed); },
        source);
    // Quick Record is always one of them (existing_transcription_tabs), even as a screen built
    // with the window outside the group.
    if (QWidget* quick = quickRecordPage(); quick != nullptr && quick != source) {
        const std::vector<QWidget*> members =
            context_.screens().existingInGroup(kTranscriptionGroup);
        if (std::ranges::find(members, quick) == members.end()) {
            callDeclared(quick, "setTranscriptionCollapsed", collapsed);
        }
    }

    if (!isVisible() || isCompactMode() || hostMode_) {
        return;
    }
    QWidget* current = currentPage();
    if (current == quickRecordPage()) {
        schedulePageFit();
        return;
    }
    const std::vector<QWidget*> tabs = context_.screens().existingInGroup(kTranscriptionGroup);
    if (std::ranges::find(tabs, current) == tabs.end()) {
        return;
    }
    // The window moves with the card's own animation, then refits to the page once both settle.
    const CollapseResize resize = transcript_collapse(
        collapsed, delta, height(), pageFit_->transcriptHeight(), minimumHeight());
    pageFit_->setTranscriptHeight(resize.transcript_height);
    if (resize.min_height) {
        setMinimumHeight(*resize.min_height);
    }
    pageFit_->animateResize(width(), resize.target_height);
}

void MainWindow::schedulePageFit() {
    pageFit_->schedule();
}

void MainWindow::refitPage(bool animate) {
    pageFit_->refit(animate);
}

void MainWindow::setPageElastic(QWidget* page, bool elastic) {
    pageFit_->setElastic(page, elastic);
}

bool MainWindow::isPageElastic(QWidget* page) const {
    return pageFit_->isElastic(page);
}

void MainWindow::setSidePanel(QWidget* panel, QWidget* edgeTab) {
    sidePanelHost_->install(panel, edgeTab, contentLayout_);
    if (QAbstractAnimation* animation = dynamic_cast<SidePanel*>(panel)->widthAnimation()) {
        connect(animation, &QAbstractAnimation::finished, this,
                &MainWindow::onSidePanelAnimationFinished);
    }
    if (auto* button = qobject_cast<QAbstractButton*>(edgeTab)) {
        connect(button, &QAbstractButton::clicked, this, &MainWindow::toggleSidePanel);
    }
    if (isCompactMode()) {
        panel->hide();
        if (edgeTab != nullptr) {
            edgeTab->hide();
        }
    }
}

QWidget* MainWindow::sidePanel() const {
    return sidePanelHost_->panel();
}

QWidget* MainWindow::sidePanelEdgeTab() const {
    return sidePanelHost_->edgeTab();
}

bool MainWindow::isSidePanelExpanded() const {
    return sidePanelHost_->isExpanded();
}

// main_window.py:1799-1828.
void MainWindow::toggleSidePanel() {
    kLog.info("Toggling contextual sidebar");
    if (hostMode_) {
        return;
    }
    if (isCompactMode()) {
        setCompactMode(false);
        if (isSidePanelExpanded()) {
            return;
        }
    }
    // A running height animation would fight the per-frame lockstep resize.
    pageFit_->stopAnimation();
    sidePanelHost_->toggle();
}

void MainWindow::sidePanelWidthAnimated(int width) {
    if (!hostMode_) {
        sidePanelHost_->widthAnimated(width);
    }
}

// main_window.py:1907-1913.
void MainWindow::onSidePanelAnimationFinished() {
    QWidget* current = currentPage();
    if (QScrollArea* scroll = pageScrollArea(current)) {
        current->updateGeometry();
        if (QWidget* content = scroll->widget()) {
            content->updateGeometry();
        }
        scroll->viewport()->update();
    }
    schedulePageFit();
}

void MainWindow::setHostDashboardFactory(std::function<QWidget*(QWidget*)> factory) {
    hostDashboardFactory_ = std::move(factory);
    if (setupComplete_ && hostDashboardFactory_ && context_.settings().get(keys::host_mode)) {
        setHostMode(true, false);
    }
}

void MainWindow::setHostMode(bool enabled, bool persist) {
    auto& menus = context_.menus();
    auto* command = menus.action(menu_id::hostMode);
    if (enabled && !hostDashboard_) {
        if (hostDashboardFactory_) {
            hostDashboard_ = hostDashboardFactory_(mainArea_);
        }
        if (!hostDashboard_) {
            if (command) {
                command->setChecked(false);
            }
            return;
        }
        hostDashboard_->hide();
        mainAreaLayout_->addWidget(hostDashboard_);
    }
    if (enabled == hostMode_) {
        if (command) {
            command->setChecked(enabled);
        }
        return;
    }

    pageFit_->stopAnimation();
    leaveMaximizedInPlace();
    if (enabled && isCompactMode()) {
        setCompactMode(false, persist);
    }
    if (persist) {
        saveGeometryNow();
    }
    if (enabled) {
        recordingGeometry_ = geometry();
    }
    hostMode_ = enabled;
    tabbedContent_->setVisible(!enabled);
    hostDashboard_->setVisible(enabled);
    for (auto* widget : {sidePanel(), sidePanelEdgeTab()}) {
        if (widget) {
            widget->setVisible(!enabled);
        }
    }

    const auto product = qt::toQString(identity().product);
    titleBar_->titleLabel()->setText(enabled ? tr("%1 Host").arg(product) : product);
    for (const auto* id : {menu_id::sidePanel, menu_id::compactMode}) {
        if (auto* action = menus.action(id)) {
            action->setEnabled(!enabled);
        }
    }
    if (command) {
        command->setChecked(enabled);
    }
    if (enabled) {
        bool restored = false;
        try {
            restored = applySavedGeometry(context_.settings().get(keys::host_window_geometry));
        } catch (const std::exception& error) {
            kLog.warning("Failed to restore Host Mode geometry: {}", error.what());
        }
        if (!restored) {
            // The first time: grow from where the window is to at least
            // config.MAIN_WINDOW_HOST_DEFAULT_WIDTH x _HEIGHT.
            setGeometry(clampGeometry(x(), y(), std::max(width(), kHostDefaultSize),
                                      std::max(height(), kHostDefaultSize)));
        }
    } else {
        if (recordingGeometry_) {
            const auto rect = *recordingGeometry_;
            setGeometry(clampGeometry(rect.x(), rect.y(), rect.width(), rect.height()));
            recordingGeometry_.reset();
        }
        schedulePageFit();
    }
    if (persist) {
        try {
            context_.settings().save_setting(keys::host_mode.key(), enabled);
        } catch (const std::exception& error) {
            kLog.warning("Failed to save Host Mode: {}", error.what());
        }
    }
}

void MainWindow::switchToTab(int index) {
    if (hostMode_) {
        setHostMode(false);
    }
    tabbedContent_->setCurrentIndex(index);
}

void MainWindow::setCompactController(QWidget* controller) {
    compactMode_->setController(controller, mainAreaLayout_);
}

QWidget* MainWindow::compactController() const {
    return compactMode_->controller();
}

// main_window.py:1501-1528.
void MainWindow::setCompactMode(bool compact, bool persist) {
    const bool before = compactMode_->isOn();
    compactMode_->set(compact, persist);
    if (compactMode_->isOn() != before) {
        Q_EMIT compactModeChanged(compactMode_->isOn());
    }
}

bool MainWindow::isCompactMode() const {
    return compactMode_->isOn();
}

// main_window.py:1496-1499.
void MainWindow::toggleCompactMode() {
    if (hostMode_) {
        return;
    }
    setCompactMode(!isCompactMode());
}

void MainWindow::restoreCompactMode() {
    const bool before = compactMode_->isOn();
    compactMode_->restore();
    if (compactMode_->isOn() != before) {
        Q_EMIT compactModeChanged(compactMode_->isOn());
    }
}

void MainWindow::openSettings() {
    kLog.info("Opening settings dialog");
    Q_EMIT settingsRequested();
}

void MainWindow::openDownloads() {
    kLog.info("Opening Settings on Downloads");
    Q_EMIT settingsDestinationRequested(QStringLiteral("downloads"));
}

void MainWindow::openHotkeySettings() {
    kLog.info("Opening hotkey settings");
    Q_EMIT hotkeysRequested();
}

void MainWindow::checkForUpdates() {
    kLog.info("Check for updates requested");
    Q_EMIT checkForUpdatesRequested();
}

void MainWindow::showAbout() {
    kLog.info("Showing about dialog");
    Q_EMIT aboutRequested();
}

void MainWindow::setUpdateNotice(const QString& version) {
    updateNotice_->showVersion(version);
}

void MainWindow::setTrayAvailable(bool available) {
    trayAvailable_ = available;
    footer_->setTrayAvailable(available);
    if (QAction* action = context_.menus().action(menu_id::minimizeToTray)) {
        action->setVisible(available);
    }
}

void MainWindow::updateHotkeys(const QString& /*record*/, const QString& /*cancel*/,
                               const QString& /*enableDisable*/, const QString& minimize,
                               const QString& /*walkthrough*/, const QString& /*screenshot*/) {
    footer_->setTrayHotkey(minimize);
}

void MainWindow::minimizeToTray() {
    if (!trayAvailable_) {
        kLog.info("No system tray is available; minimizing the window");
        showMinimized();
        return;
    }
    kLog.info("Minimizing to tray");
    hide();
}

void MainWindow::toggleTrayVisibility() {
    if (isVisible() && !isMinimized()) {
        minimizeToTray();
        return;
    }
    restoreFromTray();
}

void MainWindow::restoreFromTray() {
    kLog.info("Restoring window from tray");
    // Drop any minimized bit and mark the window active before showing: once an app has no
    // visible window, a bare showNormal() can leave it behind others (macOS).
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    showNormal();
    // showNormal() un-maximizes; the title bar's own flag must follow.
    titleBar_->syncMaximized(false);
    raise();
    activateWindow();
}

void MainWindow::requestQuit() {
    if (confirmQuit && !confirmQuit()) {
        kLog.info("Quit canceled: work in progress");
        return;
    }
    quitApplication();
}

void MainWindow::quitApplication() {
    kLog.info("Quitting application");
    saveGeometryNow();
    forceQuit_ = true;
    QApplication::quit();
}

QRect MainWindow::currentScreenRect() const {
    QScreen* screen = QGuiApplication::screenAt(frameGeometry().center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    return screen != nullptr ? screen->availableGeometry() : kFallbackScreen;
}

QRect MainWindow::clampGeometry(int x, int y, int width, int height,
                                std::optional<QRect> available) const {
    const QRect area = available ? *available : availableScreenRect();
    const Rect clamped = clamp_geometry(Rect{x, y, width, height}, toRect(area),
                                        Size{minimumWidth(), minimumHeight()},
                                        Size{maximumWidth(), maximumHeight()});
    return QRect(clamped.x, clamped.y, clamped.width, clamped.height);
}

void MainWindow::centerOnScreen() {
    const QRect area = availableScreenRect();
    setGeometry(clampGeometry(area.x() + floorHalf(area.width() - width()),
                              area.y() + floorHalf(area.height() - height()), width(), height()));
}

void MainWindow::restoreWindowGeometry() {
    try {
        const Json geometry = context_.settings().get(keys::window_geometry);
        if (applySavedGeometry(geometry)) {
            return;
        }
        kLog.debug("No valid saved geometry, using default");
        centerOnScreen();
    } catch (const std::exception& error) {
        kLog.warning("Failed to restore window geometry: {}", error.what());
        centerOnScreen();
    }
}

// main_window.py:2344-2384.
bool MainWindow::applySavedGeometry(const Json& geometry) {
    if (!geometry.is_object() || !geometry.contains("x") || !geometry.contains("y") ||
        !geometry.contains("width") || !geometry.contains("height")) {
        return false;
    }
    const QRect saved(geometry_int(geometry, "x"), geometry_int(geometry, "y"),
                      geometry_int(geometry, "width"), geometry_int(geometry, "height"));
    // Any connected screen, the primary first: a window kept on a second monitor returns there
    // rather than being re-centred on the first.
    std::optional<QRect> area;
    for (const QRect& screen : screenAreas()) {
        if (screen.intersects(saved)) {
            area = screen;
            break;
        }
    }
    if (!area) {
        return false;
    }
    int width = restored_width(geometry, minimumWidth(), maximumWidth(), kCollapsedContentFormat);
    // Only a start: showing the window fits its height to the tab.
    const int height = std::max(minimumHeight(), std::min(saved.height(), area->height()));
    if (!hostMode_ && isSidePanelExpanded()) {
        width = std::min(maximumWidth(), width + window_size::kHistorySidebarWidth);
    }
    setGeometry(clampGeometry(saved.x(), saved.y(), width, height, area));
    kLog.info("Restored window geometry: {}", pythonRepr(geometry));
    return true;
}

void MainWindow::scheduleGeometrySave() {
    if (!initialShowComplete_) {
        return;
    }
    if (geometrySaveTimer_ == nullptr) {
        geometrySaveTimer_ = new QTimer(this);
        geometrySaveTimer_->setSingleShot(true);
        connect(geometrySaveTimer_, &QTimer::timeout, this, &MainWindow::saveGeometryNow);
    }
    geometrySaveTimer_->stop();
    geometrySaveTimer_->start(kGeometrySaveDelayMs);
}

bool MainWindow::geometrySaveScheduled() const {
    return geometrySaveTimer_ != nullptr && geometrySaveTimer_->isActive();
}

// main_window.py:2288-2325; Host Mode's own geometry is M9's.
void MainWindow::saveGeometryNow() {
    if (isMaximized() || isMinimized()) {
        return;
    }
    if (isCompactMode()) {
        compactMode_->saveGeometry();
        return;
    }
    const QRect rect = geometry();
    if (hostMode_) {
        try {
            context_.settings().save_setting(keys::host_window_geometry.key(),
                                             Json{{"x", rect.x()},
                                                  {"y", rect.y()},
                                                  {"width", rect.width()},
                                                  {"height", rect.height()}});
        } catch (const std::exception& error) {
            kLog.warning("Failed to save Host Mode geometry: {}", error.what());
        }
        return;
    }
    const bool panelExpanded = isSidePanelExpanded();
    Json saved = Json::object();
    saved["x"] = rect.x();
    saved["y"] = rect.y();
    saved["width"] =
        saved_window_width(rect.width(), minimumWidth(), panelExpanded, pageFit_->ownWidth());
    saved["height"] = rect.height();
    saved["format"] = std::string(kCollapsedContentFormat);
    saved["history_expanded"] = panelExpanded;
    try {
        context_.settings().save_setting(keys::window_geometry.key(), saved);
    } catch (const std::exception& error) {
        kLog.warning("Failed to save window geometry: {}", error.what());
    }
}

ResizeEdge MainWindow::resizeEdge(QPoint local) const {
    ResizeEdge edge;
    if (isCompactMode()) {
        return edge;
    }
    const QRect area = rect();
    if (local.x() <= kResizeMargin) {
        edge.horizontal = -1;
    } else if (local.x() >= area.width() - kResizeMargin) {
        edge.horizontal = 1;
    }
    if (local.y() <= kResizeMargin) {
        edge.vertical = -1;
    } else if (local.y() >= area.height() - kResizeMargin) {
        edge.vertical = 1;
    }
    return edge;
}

void MainWindow::updateCursorForEdge(ResizeEdge edge) {
    const int h = edge.horizontal;
    const int v = edge.vertical;
    if (h == 0 && v == 0) {
        setCursor(Qt::ArrowCursor);
    } else if (h != 0 && v == 0) {
        setCursor(Qt::SizeHorCursor);
    } else if (h == 0) {
        setCursor(Qt::SizeVerCursor);
    } else if (h == v) {
        setCursor(Qt::SizeFDiagCursor);
    } else {
        setCursor(Qt::SizeBDiagCursor);
    }
}

// A maximized window is the platform's to place: dragging its edge fought that, and the window
// jumped about the screen.
void MainWindow::leaveMaximizedInPlace() {
    if (!(isMaximized() || titleBar_->isMaximized())) {
        return;
    }
    const QRect rect = geometry();
    showNormal();
    setGeometry(rect);
    titleBar_->syncMaximized(false);
}

void MainWindow::beginResize(ResizeEdge edge, QPoint global) {
    leaveMaximizedInPlace();
    resizing_ = true;
    resizeEdge_ = edge;
    resizeStartPos_ = global;
    resizeStartGeometry_ = geometry();
}

void MainWindow::applyResizeDelta(QPoint global) {
    if (!resizing_ || resizeEdge_.none() || !resizeStartGeometry_) {
        return;
    }
    const QPoint delta = global - resizeStartPos_;
    const QRect start = *resizeStartGeometry_;
    int x = start.x();
    int y = start.y();
    int width = start.width();
    int height = start.height();
    if (resizeEdge_.horizontal == -1) {
        // Capped like the right edge, or the window slides once it is.
        width = std::min(maximumWidth(), std::max(minimumWidth(), start.width() - delta.x()));
        x = start.x() + start.width() - width;
    } else if (resizeEdge_.horizontal == 1) {
        width = std::min(maximumWidth(), std::max(minimumWidth(), start.width() + delta.x()));
    }
    if (resizeEdge_.vertical == -1) {
        height = std::max(minimumHeight(), start.height() - delta.y());
        y = start.y() + start.height() - height;
    } else if (resizeEdge_.vertical == 1) {
        height = std::max(minimumHeight(), start.height() + delta.y());
    }
    setGeometry(x, y, width, height);
}

void MainWindow::finishResize() {
    if (!resizing_) {
        return;
    }
    resizing_ = false;
    // A width the user drags is their own: nothing is owed back to it.
    sidePanelHost_->resetSqueeze();
    QWidget* quickRecord = quickRecordPage();
    if (!isCompactMode() && !hostMode_ && currentPage() == quickRecord) {
        if (pageFit_->isElastic(quickRecord) && resizeStartGeometry_ &&
            height() != resizeStartGeometry_->height()) {
            pageFit_->setQuickRecordHeight(height());
        }
        schedulePageFit();
    }
    resizeEdge_ = {};
    resizeStartPos_ = {};
    resizeStartGeometry_.reset();
    scheduleGeometrySave();
}

void MainWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const ResizeEdge edge = resizeEdge(event->position().toPoint());
        if (!edge.none()) {
            beginResize(edge, event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent* event) {
    if (resizing_ && !resizeEdge_.none()) {
        applyResizeDelta(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    updateCursorForEdge(resizeEdge(event->position().toPoint()));
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && resizing_) {
        finishResize();
        event->accept();
        return;
    }
    QMainWindow::mouseReleaseEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseMove && !resizing_) {
        const QPoint local =
            mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
        if (rect().contains(local)) {
            updateCursorForEdge(resizeEdge(local));
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    // Quick Record remembers only an explicit drag (finishResize).
    if (!isCompactMode() && !hostMode_ && tabbedContent_ != nullptr &&
        !pageFit_->animationRunning()) {
        QWidget* current = currentPage();
        if (current != quickRecordPage() && pageFit_->isElastic(current)) {
            pageFit_->setTranscriptHeight(event->size().height());
        }
    }
    // A narrower window rewraps a page's text, changing how much height it needs; height-only
    // changes cannot alter the wrap.
    if (event->oldSize().width() != event->size().width()) {
        schedulePageFit();
    }
    if (!resizing_) {
        scheduleGeometrySave();
    }
}

void MainWindow::moveEvent(QMoveEvent* event) {
    QMainWindow::moveEvent(event);
    scheduleGeometrySave();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // The first show keeps the geometry the constructor restored; Qt's first layout pass must
    // not be disturbed.
    if (!initialShowComplete_) {
        initialShowComplete_ = true;
        pageFit_->refit(false);
        return;
    }
    if (!isMaximized()) {
        if (isCompactMode()) {
            compactMode_->restoreGeometry();
        } else {
            restoreWindowGeometry();
        }
    }
    // A tall saved height (an open transcript's) is not kept as blank space.
    pageFit_->refit(false);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    kLog.info("Main window closing");
    tabbedContent_->flushPendingTabSave();
    if (forceQuit_) {
        kLog.info("Force quit - closing application");
        event->accept();
        return;
    }
    bool minimizeTray = true;
    try {
        minimizeTray = context_.settings().get(keys::minimize_tray);
    } catch (const std::exception& error) {
        kLog.error("Failed to load settings: {}", error.what());
        minimizeTray = true; // a read error keeps the safer close-to-tray
    }
    if (minimizeTray && trayAvailable_) {
        event->ignore();
        hide();
        kLog.info("Window hidden to system tray");
        return;
    }
    event->accept();
}

} // namespace fv
