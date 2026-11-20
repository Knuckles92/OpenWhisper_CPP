#include "ui_settings/settings_dialog.h"

#include <algorithm>
#include <array>
#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

#include <QCloseEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLayout>
#include <QLayoutItem>
#include <QMenu>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "appkit/settings_page.h"
#include "base/desktop_session.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/json.h"
#include "settings/store.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/app_icon.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/wrapped_label.h"
#include "uimath/dialog_fit.h"
#include "uimath/overlay_position.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_dialog"};

// Home's cards, reached by the names of the Basic tabs they replaced.
constexpr std::array<std::string_view, 3> kHomeSections{
    destinations::basic_dictation, destinations::basic_meetings, destinations::basic_app};

// The pages whose shortcut fields share the window's one capture, which refresh() stops.
constexpr std::array<std::string_view, 2> kSharedCapturePages{destinations::hotkeys,
                                                              destinations::walkthroughs};

// The cards a reveal tints.
constexpr std::array<const char*, 3> kRevealCards{"settingsTile", "textModelFootnoteCard",
                                                  "homeCard"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

bool isWayland() {
    return QGuiApplication::platformName().toLower().startsWith(QStringLiteral("wayland"));
}

// desktop.compositor_managed: a Wayland or Omarchy session places windows itself.
bool compositorManaged() {
    return isWayland() || use_omarchy_ui();
}

// Runs one page hook; what it throws is logged under message (one "{}" for the page key).
template <class Fn> void guardedHook(std::string_view message, std::string_view key, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        kLog.error_with(error, std::vformat(message, std::make_format_args(key)));
    } catch (...) {
        kLog.error("{}", std::vformat(message, std::make_format_args(key)));
    }
}

// The body of a Qt slot or event handler: nothing may leave it into Qt's event loop
// (architecture 4 rule 10), so what fn throws (a page's create, the palette's opener) is logged
// under message.
template <class Fn> void guardedSlot(std::string_view message, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        kLog.error_with(error, message);
    } catch (...) {
        kLog.error("{}", message);
    }
}

// Whether widget is an item of layout or of a layout nested in it.
bool inLayout(QLayout* layout, const QWidget* widget) {
    if (layout == nullptr) {
        return false;
    }
    for (int index = 0; index < layout->count(); ++index) {
        QLayoutItem* item = layout->itemAt(index);
        if (item->widget() == widget || inLayout(item->layout(), widget)) {
            return true;
        }
    }
    return false;
}

} // namespace

struct SettingsDialog::Page {
    std::string key;
    const SettingsPageSpec* spec = nullptr;
    SettingsPage* page = nullptr;
    QScrollArea* scroll = nullptr;
    QPointer<QWidget> content;
    bool built = false;
};

SettingsDialog::SettingsDialog(AppContext& context, QWidget* parent)
    : QDialog(parent), context_(context),
      bindings_([this](std::string_view key, const Json& value) { return persist(key, value); }),
      meetingBindings_(
          [this](std::string_view key, const Json& value) { return persist(key, value); }) {
    setWindowTitle(QStringLiteral("Settings"));
    setWindowIcon(ui::appIcon());
    setObjectName(QStringLiteral("settingsDialog"));
    setModal(false);
    setWindowFlag(Qt::MSWindowsFixedSizeDialogHint, false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    setSizeGripEnabled(true);

    auto* root = new QVBoxLayout(this);
    // Only the shell constrains the window; page contents scroll independently.
    root->setSizeConstraint(QLayout::SetDefaultConstraint);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildHeader());
    splitter_ = new QSplitter(Qt::Horizontal);
    splitter_->setObjectName(QStringLiteral("settingsSplitter"));
    splitter_->setHandleWidth(6);
    splitter_->setChildrenCollapsible(false);
    root->addWidget(splitter_, 1);
    railPane_ = buildRailPane();
    splitter_->addWidget(railPane_);
    QWidget* body = buildBody();
    splitter_->addWidget(body);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({NavRail::kRailWidth, kDefaultSize.width() - NavRail::kRailWidth});
    QSplitterHandle* handle = splitter_->handle(1);
    handle->setCursor(Qt::SizeHorCursor);
    handle->setToolTip(QStringLiteral("Drag to resize the settings sidebar"));
    setMinimumSize(kMinimumSize);
    resize(kDefaultSize);

    for (const QKeySequence& sequence :
         {QKeySequence(QStringLiteral("Ctrl+K")), QKeySequence(QKeySequence::Find)}) {
        auto* shortcut = new QShortcut(sequence, this);
        connect(shortcut, &QShortcut::activated, this, [this] {
            guardedSlot("Couldn't open the settings search", [this] { openSearch(); });
        });
        searchShortcuts_.push_back(shortcut);
    }
    flashTimer_ = new QTimer(this);
    flashTimer_->setSingleShot(true);
    flashTimer_->setInterval(kSearchFlashMs);
    connect(flashTimer_, &QTimer::timeout, this, [this] { clearSearchFlash(); });
    // Owned by the window, so a reveal queued just before the window is destroyed dies with it
    // instead of touching deleted widgets.
    revealTimer_ = new QTimer(this);
    revealTimer_->setSingleShot(true);
    revealTimer_->setInterval(0);
    connect(revealTimer_, &QTimer::timeout, this, [this] { runReveal(); });

    pagesReady_ = true;
    const QStringList keys = rail_->keys();
    if (keys.contains(q(destinations::overview))) {
        rail_->select(q(destinations::overview));
    } else if (!keys.isEmpty()) {
        rail_->select(keys.first());
    }
    refresh();
}

SettingsDialog::~SettingsDialog() {
    // The pages go while the binders and the loading flag they save through still exist; a
    // control that saves on losing focus as it is destroyed must not write.
    loading_ = true;
    revealTimer_->stop();
    flashTimer_->stop();
    const QList<QWidget*> children = findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    qDeleteAll(children);
}

QWidget* SettingsDialog::buildHeader() {
    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("settingsViewHeader"));
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(20, 14, 20, 14);
    row->setSpacing(10);
    auto* icon = new QLabel;
    // The mark is already a rounded tile; framing it again reads as a ring. 32 px is a native
    // icon size, so it stays crisp instead of resampled.
    icon->setPixmap(ui::appIcon().pixmap(32, 32));
    row->addWidget(icon);
    brand_ = new QLabel(q(context_.identity().product));
    brand_->setObjectName(QStringLiteral("settingsViewBrand"));
    row->addWidget(brand_);
    settingsLabel_ = new QLabel(QStringLiteral("Settings"));
    settingsLabel_->setObjectName(QStringLiteral("settingsViewLabel"));
    row->addWidget(settingsLabel_);
    // resizeEvent hides both in a narrow window; until it does, they must not hold the window
    // wider than the narrow width it is going to. An explicit minimum replaces a label's
    // text-wide minimum size hint.
    brand_->setMinimumWidth(1);
    settingsLabel_->setMinimumWidth(1);
    // Stands in for the rail when the window is too narrow for it.
    pageMenuButton_ = new QPushButton;
    pageMenuButton_->setObjectName(QStringLiteral("settingsPageMenu"));
    pageMenuButton_->setCursor(Qt::PointingHandCursor);
    applyPageMenuIcon();
    pageMenuButton_->setLayoutDirection(Qt::RightToLeft);
    pageMenuButton_->setAccessibleName(QStringLiteral("Choose a settings page"));
    connect(pageMenuButton_, &QPushButton::clicked, this, [this] {
        guardedSlot("Couldn't show the settings page menu", [this] { showPageMenu(); });
    });
    pageMenuButton_->setVisible(false);
    row->addWidget(pageMenuButton_);
    row->addStretch(1);

    searchButton_ = new QPushButton;
    searchButton_->setObjectName(QStringLiteral("settingsRailSearch"));
    searchButton_->setCursor(Qt::PointingHandCursor);
    // Ctrl+K is the keyboard path; as the first control in the window it would otherwise take
    // focus on open and show a permanent focus ring.
    searchButton_->setFocusPolicy(Qt::NoFocus);
    searchButton_->setToolTip(QStringLiteral("Search every setting, model, and help note"));
    searchButton_->setAccessibleName(QStringLiteral("Search settings and models"));
    searchButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    searchButton_->setMaximumWidth(420);
    auto* searchRow = new QHBoxLayout(searchButton_);
    searchRow->setContentsMargins(12, 0, 8, 0);
    searchRow->setSpacing(8);
    auto* searchIcon = new QLabel;
    searchIcon->setObjectName(QStringLiteral("settingsRailSearchIcon"));
    searchIcon->setPixmap(ui::designIcon(QStringLiteral("search-slate.svg")).pixmap(14, 14));
    searchRow->addWidget(searchIcon);
    searchText_ = new QLabel(QStringLiteral("Search every setting, model and help note"));
    searchText_->setObjectName(QStringLiteral("settingsRailSearchText"));
    searchText_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    searchRow->addWidget(searchText_, 1);
    // settings_search.shortcut_text: the platform's spelling of the shortcut.
    searchHint_ =
        new QLabel(QKeySequence(QStringLiteral("Ctrl+K")).toString(QKeySequence::NativeText));
    searchHint_->setObjectName(QStringLiteral("settingsRailSearchHint"));
    searchRow->addWidget(searchHint_);
    for (QLabel* label : {searchIcon, searchText_, searchHint_}) {
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    searchButton_->setFixedHeight(36);
    connect(searchButton_, &QPushButton::clicked, this,
            [this] { guardedSlot("Couldn't open the settings search", [this] { openSearch(); }); });
    row->addWidget(searchButton_, 2);
    return header;
}

QWidget* SettingsDialog::buildRailPane() {
    auto* pane = new QWidget;
    pane->setObjectName(QStringLiteral("modelManagerRailPane"));
    pane->setMinimumWidth(NavRail::kRailMinWidth);
    pane->setMaximumWidth(560);
    auto* column = new QVBoxLayout(pane);
    column->setContentsMargins(0, 12, 0, 0);
    column->setSpacing(0);

    rail_ = new NavRail;
    const SettingsPageRegistry& registry = context_.settingsPages();
    if (registry.page(destinations::home.key) != nullptr) {
        rail_->addDestination(q(destinations::home.key), q(destinations::home.name),
                              ui::designIcon(q(destinations::home.icon)));
    }
    // A page the rail layout does not know sits beside Home, under its own title.
    const auto listed = [](std::string_view key) {
        if (key == destinations::home.key) {
            return true;
        }
        for (const auto& chapter : destinations::rail) {
            for (const auto& page : chapter.pages) {
                if (page.key == key) {
                    return true;
                }
            }
        }
        return false;
    };
    for (const SettingsPageSpec* spec : registry.pages()) {
        if (!listed(spec->destination)) {
            rail_->addDestination(q(spec->destination), spec->title);
        }
    }
    for (const auto& chapter : destinations::rail) {
        std::vector<const destinations::RailPage*> registered;
        for (const auto& page : chapter.pages) {
            if (registry.page(page.key) != nullptr) {
                registered.push_back(&page);
            }
        }
        if (registered.empty()) {
            continue;
        }
        rail_->addChapter(q(chapter.title), ui::designIcon(q(chapter.icon)));
        for (const destinations::RailPage* page : registered) {
            rail_->addDestination(q(page->key), q(page->name));
        }
    }
    connect(rail_, &NavRail::destinationChanged, this, [this](const QString& key) {
        guardedSlot("Couldn't show a settings page", [&] { onDestinationChanged(key); });
    });
    column->addWidget(rail_, 1);
    return pane;
}

QWidget* SettingsDialog::buildBody() {
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("modelManagerBody"));
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(28, 16, 28, 16);
    layout->setSpacing(14);

    auto* heading = new QVBoxLayout;
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(2);
    // The chapter a page belongs to, above its title.
    crumb_ = new QLabel(QString());
    crumb_->setObjectName(QStringLiteral("settingsPageCrumb"));
    title_ = new QLabel(QString());
    title_->setObjectName(QStringLiteral("modelManagerTitle"));
    subtitle_ = new WrappedLabel(QString());
    subtitle_->setObjectName(QStringLiteral("modelManagerSubtitle"));
    heading->addWidget(crumb_);
    heading->addWidget(title_);
    heading->addSpacing(2);
    heading->addWidget(subtitle_);
    // A hairline closes the heading, so a scrolled page slides under a clean edge rather than
    // being sliced off mid-card.
    heading->addSpacing(6);
    auto* divider = new QFrame;
    divider->setObjectName(QStringLiteral("settingsPageHeaderDivider"));
    divider->setFixedHeight(1);
    heading->addWidget(divider);
    layout->addLayout(heading);

    // Shared by every page, so their messages land in one place.
    message_ = new WrappedLabel(QString());
    message_->setObjectName(QStringLiteral("modelManagerMessage"));

    stack_ = new QStackedWidget;
    stack_->setObjectName(QStringLiteral("modelManagerStack"));
    for (const SettingsPageSpec* spec : context_.settingsPages().pages()) {
        addPage(*spec);
    }
    layout->addWidget(stack_, 1);

    auto* footer = new QHBoxLayout;
    footer->setSpacing(8);
    auto* saved = new QLabel(QStringLiteral("Changes save automatically"));
    saved->setObjectName(QStringLiteral("settingsAutoSaveLabel"));
    footer->addWidget(saved);
    footer->addWidget(message_, 1);
    closeButton_ = new Button(QStringLiteral("Done"));
    closeButton_->setObjectName(QStringLiteral("modelManagerCloseButton"));
    closeButton_->setToolTip(QStringLiteral("Close Settings (Esc)"));
    fitCompactButton(closeButton_, 110);
    connect(closeButton_, &QPushButton::clicked, this,
            [this] { guardedSlot("Couldn't close Settings", [this] { close(); }); });
    footer->addWidget(closeButton_, 0, Qt::AlignBottom);
    layout->addLayout(footer);
    return body;
}

void SettingsDialog::addPage(const SettingsPageSpec& spec) {
    auto entry = std::make_unique<Page>();
    entry->key = std::string(spec.destination);
    entry->spec = &spec;
    entry->page = new SettingsPage(this);
    entry->page->setObjectName(QStringLiteral("settingsPage_%1").arg(q(entry->key)));
    if (!spec.scroll) {
        // Downloads: its catalog list is the page's only scroller.
        stack_->addWidget(entry->page);
    } else {
        entry->page->pageLayout()->addStretch();
        auto* area = new QScrollArea;
        area->setObjectName(QStringLiteral("settingsPageScroll"));
        area->setFrameShape(QFrame::NoFrame);
        area->setWidgetResizable(true);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->setWidget(entry->page);
        entry->scroll = area;
        stack_->addWidget(area);
    }
    pages_.push_back(std::move(entry));
}

SettingsDialog::Page* SettingsDialog::findPage(std::string_view key) const {
    const std::string_view resolved = destinations::resolve_destination(key);
    for (const auto& page : pages_) {
        if (page->key == resolved) {
            return page.get();
        }
    }
    return nullptr;
}

SettingsPageModule* SettingsDialog::moduleOf(std::string_view key) const {
    const Page* entry = findPage(key);
    if (entry == nullptr || !entry->built || entry->content == nullptr) {
        return nullptr;
    }
    return dynamic_cast<SettingsPageModule*>(entry->content.data());
}

SettingsDeepLinks* SettingsDialog::linksOf(std::string_view key) const {
    const Page* entry = findPage(key);
    if (entry == nullptr || !entry->built || entry->content == nullptr) {
        return nullptr;
    }
    return dynamic_cast<SettingsDeepLinks*>(entry->content.data());
}

// --- Navigation ---

void SettingsDialog::selectDestination(const QString& name) {
    const std::string requested = qt::toUtf8(name);
    const std::string_view key = destinations::resolve_destination(requested);
    if (backupBusy_ && key != destinations::backup) {
        return;
    }
    if (std::ranges::find(kHomeSections, key) != kHomeSections.end()) {
        // The old Basic tabs live on as Home's cards.
        selectDestination(q(destinations::overview));
        if (SettingsPageModule* home = moduleOf(destinations::overview)) {
            QWidget* card = nullptr;
            guardedHook("Couldn't find the {} card on Home", key,
                        [&] { card = home->namedControl(key); });
            if (card != nullptr) {
                reveal(card);
            }
        }
        return;
    }
    if (findPage(key) != nullptr) {
        ensurePage(key);
        rail_->select(q(key));
    }
}

void SettingsDialog::showHome() {
    selectDestination(q(destinations::overview));
    QScrollArea* area = pageScroll(destinations::overview);
    if (area != nullptr && rail_->currentKey() == q(destinations::overview)) {
        area->verticalScrollBar()->setValue(0);
    }
}

void SettingsDialog::focusApiKeys(const QString& env) {
    selectDestination(q(destinations::api_keys));
    if (SettingsDeepLinks* links = linksOf(destinations::api_keys)) {
        guardedHook("Couldn't focus a credential on the {} page", destinations::api_keys,
                    [&] { links->focusCredential(env); });
    }
}

void SettingsDialog::focusHfPolicy() {
    selectDestination(q(destinations::downloads));
    if (QWidget* combo = control("hf_policy_combo")) {
        combo->setFocus();
    }
}

void SettingsDialog::focusCleanupModel() {
    selectDestination(q(destinations::cleanup));
    if (QWidget* tile = control("cleanup_model_tile")) {
        reveal(tile);
    }
}

void SettingsDialog::showDownloads(const QString& backend, const QString& componentId) {
    selectDestination(q(destinations::downloads));
    SettingsDeepLinks* links = linksOf(destinations::downloads);
    if (links == nullptr) {
        return;
    }
    if (!backend.isEmpty()) {
        guardedHook("Couldn't filter the {} page", destinations::downloads,
                    [&] { links->showBackend(backend); });
    }
    if (!componentId.isEmpty()) {
        guardedHook("Couldn't focus a component on the {} page", destinations::downloads,
                    [&] { links->focusComponent(componentId); });
    }
}

QString SettingsDialog::currentDestination() const {
    return rail_->currentKey();
}

void SettingsDialog::onDestinationChanged(const QString& name) {
    const std::string key = qt::toUtf8(name);
    if (backupBusy_ && key != destinations::backup) {
        rail_->select(q(destinations::backup));
        return;
    }
    // A capture belongs to the page it started on.
    cancelPageCaptures(key);
    savePageDrafts(key);
    Page* entry = findPage(key);
    if (entry == nullptr) {
        return;
    }
    try {
        ensurePage(key);
    } catch (const std::exception& error) {
        kLog.error_with(error, std::format("Couldn't build the {} settings page", key));
    } catch (...) {
        kLog.error("Couldn't build the {} settings page", key);
    }
    stack_->setCurrentWidget(entry->scroll != nullptr ? static_cast<QWidget*>(entry->scroll)
                                                      : entry->page);
    rail_->scrollToItem(rail_->currentItem());
    const QString chapter = rail_->chapter(name);
    crumb_->setText((chapter.isEmpty() ? QStringLiteral("Settings") : chapter).toUpper());
    title_->setText(entry->spec->title);
    subtitle_->setText(entry->spec->subtitle);
    pageMenuButton_->setText(rail_->name(name));
    if (pagesReady_) {
        if (SettingsPageModule* module = moduleOf(key)) {
            guardedHook("Couldn't show the {} settings page", key, [&] { module->onShown(); });
        }
    }
    Q_EMIT destinationChanged(name);
}

// --- Pages ---

SettingsPage* SettingsDialog::ensurePage(std::string_view requested) {
    const std::string_view key = destinations::resolve_destination(requested);
    Page* entry = findPage(key);
    if (entry == nullptr) {
        throw std::invalid_argument(std::format("No Settings page is registered for '{}'.", key));
    }
    if (entry->built || building_.contains(entry->key)) {
        return entry->page;
    }
    building_.insert(entry->key);
    const bool loading = loading_;
    const std::optional<std::string> initializing = initializing_;
    const int bindingStart = bindingStart_;
    const int meetingBindingStart = meetingBindingStart_;
    initializing_ = entry->key;
    bindingStart_ = bindings_.bindingCount();
    meetingBindingStart_ = meetingBindings_.bindingCount();
    loading_ = true;
    const auto restore = qScopeGuard([&] {
        loading_ = loading;
        initializing_ = initializing;
        bindingStart_ = bindingStart;
        meetingBindingStart_ = meetingBindingStart;
        building_.erase(entry->key);
    });
    QVBoxLayout* layout = entry->page->pageLayout();
    if (layout->count() > 0 && layout->itemAt(layout->count() - 1)->spacerItem() != nullptr) {
        delete layout->takeAt(layout->count() - 1);
    }
    QWidget* content = entry->spec->create ? entry->spec->create(entry->page) : nullptr;
    // A content widget the page already placed (in its column or a row inside it) stays put.
    if (content != nullptr && content != entry->page && !inLayout(layout, content)) {
        layout->addWidget(content);
    }
    entry->content = content;
    if (entry->scroll != nullptr) {
        layout->addStretch();
    }
    entry->built = true;
    loadSettings();
    entry->page->layout()->activate();
    return entry->page;
}

void SettingsDialog::ensureAllPages() {
    for (const auto& page : pages_) {
        ensurePage(page->key);
    }
}

QWidget* SettingsDialog::control(std::string_view name) {
    const std::string_view destination = context_.settingsPages().controlDestination(name);
    if (!destination.empty() && !building_.contains(destination)) {
        if (findPage(destination) != nullptr) {
            ensurePage(destination);
            if (SettingsPageModule* module = moduleOf(destination)) {
                QWidget* found = nullptr;
                guardedHook("Couldn't find a control of the {} settings page", destination,
                            [&] { found = module->namedControl(name); });
                if (found != nullptr) {
                    return found;
                }
            }
        }
    }
    for (const auto& page : pages_) {
        SettingsPageModule* module = moduleOf(page->key);
        if (module == nullptr) {
            continue;
        }
        QWidget* found = nullptr;
        guardedHook("Couldn't find a control of the {} settings page", page->key,
                    [&] { found = module->namedControl(name); });
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

std::vector<std::string> SettingsDialog::pageKeys() const {
    std::vector<std::string> keys;
    for (const QString& key : rail_->keys()) {
        keys.push_back(qt::toUtf8(key));
    }
    return keys;
}

bool SettingsDialog::isRegistered(std::string_view key) const {
    return findPage(key) != nullptr;
}

bool SettingsDialog::isBuilt(std::string_view key) const {
    const Page* entry = findPage(key);
    return entry != nullptr && entry->built;
}

SettingsPage* SettingsDialog::page(std::string_view key) const {
    const Page* entry = findPage(key);
    return entry != nullptr ? entry->page : nullptr;
}

QScrollArea* SettingsDialog::pageScroll(std::string_view key) const {
    const Page* entry = findPage(key);
    return entry != nullptr ? entry->scroll : nullptr;
}

QWidget* SettingsDialog::pageContent(std::string_view key) const {
    const Page* entry = findPage(key);
    return entry != nullptr && entry->built ? entry->content.data() : nullptr;
}

QString SettingsDialog::pageTitle(std::string_view key) const {
    const Page* entry = findPage(key);
    return entry != nullptr ? entry->spec->title : QString();
}

QString SettingsDialog::pageSubtitle(std::string_view key) const {
    const Page* entry = findPage(key);
    return entry != nullptr ? entry->spec->subtitle : QString();
}

bool SettingsDialog::isPageLoading(std::string_view key) const {
    const Page* entry = findPage(key);
    return entry != nullptr && entry->built &&
           (!initializing_.has_value() || *initializing_ == entry->key);
}

std::shared_ptr<const Json> SettingsDialog::settingsSnapshot() const {
    if (refreshSnapshot_ != nullptr) {
        return refreshSnapshot_;
    }
    try {
        return context_.settings().snapshot();
    } catch (const std::exception& error) {
        // The store never throws for a bad file, so this is a bug.
        kLog.error_with(error, "Couldn't read settings for the page; showing defaults");
        return std::make_shared<const Json>(Json::object());
    }
}

void SettingsDialog::loadSettings() {
    const std::shared_ptr<const Json> settings = settingsSnapshot();
    bindings_.load(*settings, bindingStart_);
    meetingBindings_.load(*settings, meetingBindingStart_);
    for (const auto& page : pages_) {
        if (!isPageLoading(page->key)) {
            continue;
        }
        if (SettingsPageModule* module = moduleOf(page->key)) {
            // One page's bad state must not stop Settings from opening.
            guardedHook("Couldn't load the {} settings page", page->key,
                        [&] { module->loadSettings(*settings); });
        }
    }
}

// --- Saving ---

bool SettingsDialog::persist(std::string_view key, const Json& value, const QString& message) {
    if (loading_) {
        return false;
    }
    try {
        // Deliberately the raw stored value: this asks whether the file already holds exactly
        // value, not what the setting means.
        const std::shared_ptr<const Json> stored = context_.settings().snapshot();
        if (stored->is_object()) {
            const auto found = stored->find(key);
            if (found != stored->end() && py::py_equal(*found, value)) {
                refreshRailValues();
                return true;
            }
        }
        context_.settings().save_setting(key, value);
    } catch (const std::exception& error) {
        kLog.error_with(error, std::format("Couldn't save setting {}", key));
        message_->setText(QStringLiteral("Couldn't save setting: %1").arg(q(error.what())));
        return false;
    }
    message_->setText(message);
    refreshRailValues();
    return true;
}

bool SettingsDialog::persistMany(const Json& updates, std::vector<std::string> drops) {
    if (loading_) {
        return false;
    }
    try {
        std::vector<std::string_view> remove(drops.begin(), drops.end());
        context_.settings().update(updates, remove);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't save settings");
        message_->setText(QStringLiteral("Couldn't save settings: %1").arg(q(error.what())));
        return false;
    }
    message_->setText(QString());
    refreshRailValues();
    return true;
}

// --- Messages and refresh ---

void SettingsDialog::showMessage(const QString& text) {
    message_->setText(text);
}

QString SettingsDialog::messageText() const {
    return message_->text();
}

void SettingsDialog::refresh() {
    beginRailBatch();
    const auto endBatch = qScopeGuard([this] { endRailBatch(); });
    for (std::string_view key : kSharedCapturePages) {
        if (SettingsPageModule* module = moduleOf(key)) {
            guardedHook("Couldn't stop the {} page's shortcut capture", key,
                        [&] { module->cancelCapture(); });
        }
    }
    loading_ = true;
    const auto reset = qScopeGuard([this] {
        loading_ = false;
        refreshSnapshot_.reset();
    });
    refreshSnapshot_ = settingsSnapshot();
    {
        const auto loaded = qScopeGuard([this] { loading_ = false; });
        loadSettings();
    }
    for (const auto& page : pages_) {
        // Home is rebuilt by the rail redraw below, once.
        if (page->key == destinations::overview) {
            continue;
        }
        if (SettingsPageModule* module = moduleOf(page->key)) {
            guardedHook("Couldn't refresh the {} settings page", page->key,
                        [&] { module->refreshPage(); });
        }
    }
    refreshRailValues();
}

void SettingsDialog::refreshPage(std::string_view key) {
    if (SettingsPageModule* module = moduleOf(key)) {
        guardedHook("Couldn't refresh the {} settings page", destinations::resolve_destination(key),
                    [&] { module->refreshPage(); });
    }
    refreshRailValues();
}

void SettingsDialog::refreshRailValues() {
    if (railBatchDepth_ > 0) {
        railRefreshPending_ = true;
        return;
    }
    if (refreshSnapshot_ != nullptr) {
        renderRailValues(*refreshSnapshot_);
        return;
    }
    // Every save lands here, and the values read the settings many times over; one copy serves
    // them all.
    refreshSnapshot_ = settingsSnapshot();
    const auto reset = qScopeGuard([this] { refreshSnapshot_.reset(); });
    renderRailValues(*refreshSnapshot_);
}

void SettingsDialog::beginRailBatch() {
    ++railBatchDepth_;
}

void SettingsDialog::endRailBatch() {
    railBatchDepth_ = std::max(0, railBatchDepth_ - 1);
    if (railBatchDepth_ == 0 && railRefreshPending_) {
        railRefreshPending_ = false;
        refreshRailValues();
    }
}

void SettingsDialog::renderRailValues(const Json& settings) {
    for (const auto& page : pages_) {
        if (!page->spec->railValue) {
            continue;
        }
        // Every save redraws the rail, so one page's bad value must not turn into a failed save
        // on another page.
        QString value;
        guardedHook("Couldn't read the {} rail value", page->key,
                    [&] { value = page->spec->railValue(settings); });
        rail_->setValue(q(page->key), value);
    }
    if (rail_->currentKey() == q(destinations::overview)) {
        if (SettingsPageModule* home = moduleOf(destinations::overview)) {
            guardedHook("Couldn't refresh the {} settings page", destinations::overview,
                        [&] { home->refreshPage(); });
        }
    }
}

void SettingsDialog::notifyChanged(const QString& kind) {
    refreshRailValues();
    Q_EMIT settingsChanged(kind);
}

void SettingsDialog::cancelPageCaptures(std::string_view keep) {
    for (const auto& page : pages_) {
        if (page->key == keep) {
            continue;
        }
        if (SettingsPageModule* module = moduleOf(page->key)) {
            // Runs while closing and switching pages, which must still finish.
            guardedHook("Couldn't stop the {} page's shortcut capture", page->key,
                        [&] { module->cancelCapture(); });
        }
    }
}

bool SettingsDialog::savePageDrafts(std::string_view keep, bool ask) {
    // False, with the page shown, when its draft is kept for more editing.
    const auto asks = [this, keep, ask](const std::string& key) {
        if (key == keep) {
            return true;
        }
        SettingsPageModule* module = moduleOf(key);
        if (module == nullptr) {
            return true;
        }
        bool saved = true;
        try {
            saved = module->saveDrafts(ask);
        } catch (const std::exception& error) {
            kLog.error_with(error, std::format("Couldn't save the {} page's draft", key));
            return true;
        } catch (...) {
            kLog.error("Couldn't save the {} page's draft", key);
            return true;
        }
        if (!saved) {
            selectDestination(q(key));
        }
        return saved;
    };
    // OpenWhisper asks its page modules first and the Profiles panel, which the dialog builds
    // itself, last.
    for (const auto& page : pages_) {
        if (page->key != destinations::cleanup_profiles && !asks(page->key)) {
            return false;
        }
    }
    return asks(std::string(destinations::cleanup_profiles));
}

// --- Locks, reveal, search and hotkeys ---

void SettingsDialog::setBackupBusy(bool busy) {
    if (busy && rail_->currentKey() != q(destinations::backup)) {
        selectDestination(q(destinations::backup));
    }
    backupBusy_ = busy;
    rail_->setEnabled(!busy);
    searchButton_->setEnabled(!busy);
    pageMenuButton_->setEnabled(!busy);
    for (QShortcut* shortcut : searchShortcuts_) {
        shortcut->setEnabled(!busy);
    }
    if (busy && searchCloser_) {
        guardedSlot("Couldn't close the settings search", searchCloser_);
    }
}

void SettingsDialog::reveal(QWidget* target) {
    revealTarget_ = target;
    revealTimer_->start();
}

void SettingsDialog::runReveal() {
    QWidget* target = revealTarget_;
    revealTarget_ = nullptr;
    if (target == nullptr) {
        // Never set, or its page was rebuilt before the reveal ran.
        return;
    }
    if (const Page* entry = findPage(qt::toUtf8(rail_->currentKey()));
        entry != nullptr && entry->scroll != nullptr) {
        entry->scroll->ensureWidgetVisible(target, 0, 40);
    }
    QWidget* card = target;
    while (card != nullptr && std::ranges::none_of(kRevealCards, [card](const char* name) {
               return card->objectName() == QLatin1String(name);
           })) {
        card = card->parentWidget();
    }
    clearSearchFlash();
    if (card != nullptr) {
        ui::setStyleProperty(card, ui::prop::searchHit, true);
        flash_ = card;
        flashTimer_->start();
    }
    QList<QWidget*> candidates{target};
    candidates.append(target->findChildren<QWidget*>());
    for (QWidget* widget : candidates) {
        if ((widget->focusPolicy() & Qt::TabFocus) != 0 && widget->isEnabled() &&
            widget->isVisibleTo(this)) {
            widget->setFocus(Qt::ShortcutFocusReason);
            break;
        }
    }
}

void SettingsDialog::clearSearchFlash() {
    QWidget* card = flash_;
    flash_ = nullptr;
    if (card != nullptr) {
        ui::setStyleProperty(card, ui::prop::searchHit, false);
    }
}

void SettingsDialog::setSearchOpener(std::function<void(QString)> opener) {
    searchOpener_ = std::move(opener);
}

void SettingsDialog::setSearchCloser(std::function<void()> closer) {
    searchCloser_ = std::move(closer);
}

void SettingsDialog::openSearch(const QString& text) {
    if (backupBusy_) {
        return;
    }
    if (searchOpener_) {
        searchOpener_(text);
    }
}

void SettingsDialog::setStandardHotkeyHandler(std::function<QString(QString, QString)> handler) {
    standardHotkeyHandler_ = std::move(handler);
}

QString SettingsDialog::setStandardHotkey(const QString& action, const QString& hotkey) {
    if (!standardHotkeyHandler_ && isRegistered(destinations::hotkeys)) {
        try {
            ensurePage(destinations::hotkeys);
        } catch (const std::exception& error) {
            kLog.error_with(error, "Couldn't build the hotkeys settings page");
        }
    }
    if (!standardHotkeyHandler_) {
        const QString error = QStringLiteral("Couldn't save hotkeys: no Hotkeys page is open.");
        message_->setText(error);
        return error;
    }
    return standardHotkeyHandler_(action, hotkey);
}

void SettingsDialog::setHotkeyCaptureSuspended(bool suspended) {
    Q_EMIT hotkeyCaptureSuspended(suspended);
}

void SettingsDialog::setEscapeOverride(std::function<void()> cancel) {
    escapeOverride_ = std::move(cancel);
}

// --- Window ---

QMenu* SettingsDialog::showPageMenu() {
    if (pageMenu_ != nullptr) {
        pageMenu_->deleteLater();
    }
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("settingsPageMenuList"));
    const QString current = rail_->currentKey();
    const auto add = [&](const QString& key) {
        QAction* action = menu->addAction(rail_->name(key));
        action->setCheckable(true);
        action->setChecked(key == current);
        connect(action, &QAction::triggered, this, [this, key] {
            guardedSlot("Couldn't show a settings page", [&] { selectDestination(key); });
        });
    };
    for (const QString& key : rail_->keys()) {
        if (rail_->chapter(key).isEmpty()) {
            add(key);
        }
    }
    for (const QString& chapter : rail_->chapters()) {
        menu->addSeparator();
        QAction* heading = menu->addAction(chapter);
        heading->setEnabled(false);
        for (const QString& key : rail_->chapterKeys(chapter)) {
            add(key);
        }
    }
    pageMenu_ = menu;
    menu->popup(pageMenuButton_->mapToGlobal(pageMenuButton_->rect().bottomLeft()));
    return menu;
}

QMenu* SettingsDialog::pageMenu() const {
    return pageMenu_;
}

void SettingsDialog::applyPageMenuIcon() {
    // The same per-theme chevron the stylesheet and the rail draw.
    const QString suffix = ui::currentPalette().qCss(u"icon-suffix");
    pageMenuButton_->setIcon(
        ui::designIcon(QStringLiteral("chevron-down-slate%1.svg").arg(suffix)));
}

std::optional<QRect> SettingsDialog::availableGeometry() const {
    if (availableScreenRect) {
        return availableScreenRect();
    }
    QWidget* parent = parentWidget();
    QScreen* screen = parent != nullptr ? parent->screen() : nullptr;
    if (screen == nullptr) {
        screen = this->screen();
    }
    if (screen == nullptr) {
        return std::nullopt;
    }
    return screen->availableGeometry();
}

void SettingsDialog::fitToScreen() {
    const std::optional<QRect> screen = availableGeometry();
    if (!screen) {
        return;
    }
    const QRect available = *screen;
    const Size minimum = use_omarchy_ui() ? kOmarchyDialogMinimum
                                          : Size{kMinimumSize.width(), kMinimumSize.height()};
    const FitResult fit =
        fit_to_screen(Size{width(), height()}, minimum,
                      Rect{available.x(), available.y(), available.width(), available.height()});
    setMinimumSize(fit.minimum.width, fit.minimum.height);
    if (fit.resized) {
        resize(fit.size.width, fit.size.height);
    }
}

void SettingsDialog::placeOnScreen(std::optional<QRect> available) {
    if (compositorManaged()) {
        return;
    }
    if (!available) {
        available = availableGeometry();
        if (!available) {
            return;
        }
    }
    QRect frame = frameGeometry();
    if (frame == geometry()) {
        frame.adjust(0, -kTitleBarAllowance, 0, 0);
    }
    const Point position = on_screen_position(
        Rect{frame.x(), frame.y(), frame.width(), frame.height()},
        Rect{available->x(), available->y(), available->width(), available->height()});
    if (QPoint(position.x, position.y) != frame.topLeft()) {
        move(position.x, position.y);
    }
}

void SettingsDialog::showEvent(QShowEvent* event) {
    if (!event->spontaneous()) {
        // Before QDialog's own showEvent, which centres by the size.
        fitToScreen();
    }
    QDialog::showEvent(event);
    if (!event->spontaneous()) {
        placeOnScreen();
    }
    if (pagesReady_ && !event->spontaneous()) {
        beginRailBatch();
        const auto endBatch = qScopeGuard([this] { endRailBatch(); });
        Q_EMIT windowShown();
    }
}

void SettingsDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    brand_->setVisible(width() >= ui::scaled(560));
    settingsLabel_->setVisible(width() >= ui::scaled(700));
    searchText_->setVisible(width() >= ui::scaled(720));
    const bool narrow = width() < ui::scaled(kNarrowWidth);
    if (narrow_ != narrow) {
        narrow_ = narrow;
        railPane_->setVisible(!narrow);
        pageMenuButton_->setVisible(narrow);
    }
}

void SettingsDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    // A theme switch restyles the window. QDialog's constructor already delivers events, so the
    // button may not exist yet.
    if (event->type() == QEvent::StyleChange && pageMenuButton_ != nullptr) {
        applyPageMenuIcon();
    }
}

bool SettingsDialog::event(QEvent* event) {
    // While a page holds a capture, Esc must not reject Settings: the field accepts the
    // override itself, and this covers focus elsewhere on the page.
    if (escapeOverride_ && event->type() == QEvent::ShortcutOverride &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        event->accept();
        return true;
    }
    return QDialog::event(event);
}

void SettingsDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && escapeOverride_) {
        // A copy: the override may clear itself.
        const std::function<void()> cancel = escapeOverride_;
        guardedSlot("Couldn't cancel what Esc stops", cancel);
        event->accept();
        return;
    }
    // Settings saves as you go and has no default action, but QDialog makes the first
    // auto-default button in the focus chain its default and clicks it for every Enter a field
    // ignores. A focused button still takes Enter itself before it gets here.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void SettingsDialog::reject() {
    // Esc closes through here without a close event.
    if (savePageDrafts({}, isVisible())) {
        QDialog::reject();
        if (!closing_) {
            Q_EMIT closed();
        }
    }
}

void SettingsDialog::closeEvent(QCloseEvent* event) {
    closing_ = true;
    const auto done = qScopeGuard([this] { closing_ = false; });
    // Quitting the app closes a visible Settings window this way too. A hidden one has nobody to
    // ask, so only its valid drafts are saved.
    if (!savePageDrafts({}, isVisible())) {
        event->ignore();
        return;
    }
    if (searchCloser_) {
        // Closing must still finish.
        guardedSlot("Couldn't close the settings search", searchCloser_);
    }
    cancelPageCaptures();
    QDialog::closeEvent(event);
    if (event->isAccepted()) {
        Q_EMIT closed();
    }
}

} // namespace fv
