#include "ui_history/history_sidebar.h"

#include <algorithm>
#include <exception>
#include <format>
#include <set>
#include <utility>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFont>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "base/clock.h"
#include "base/executor.h"
#include "base/log.h"
#include "base/utf8.h"
#include "history/calendar_source.h"
#include "pyport/text.h"
#include "remote/record_sync_control.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_history/calendar_glyph_button.h"
#include "ui_history/detail/background.h"
#include "ui_history/history_item.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_store.h"
#include "ui_history/history_text.h"
#include "ui_theme/local_style.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/context_menu.h"
#include "ui_widgets/file_reveal.h"
#include "ui_widgets/tabbed_content.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.history_sidebar"};
constexpr log::Logger kWindowLog{"ui_qt.main_window"};

// HistorySidebar._apply_style.
const QString kSidebarStyle = QStringLiteral(R"qss(
            QWidget#historySidebar {
                background-color: @bg;
            }
            QWidget#sidebarContent {
                background-color: @bg;
                border-left: 1px solid rgba(@overlay-rgb, 0.08);
            }
            QLabel#sidebarHeader {
                color: @text-heading;
                font-weight: 700;
            }
            QLabel#sectionHeader {
                color: @text-secondary-strong;
                padding-top: 4px;
                font-size: 11px;
                font-weight: 600;
            }
            QPushButton#sidebarMenuBtn {
                background-color: transparent;
                color: @text-secondary;
                border: none;
                border-radius: 14px;
                padding: 0px;
                font-size: 15px;
            }
            QPushButton#sidebarMenuBtn:hover {
                background-color: rgba(@overlay-rgb, 0.1);
                color: @text-heading;
            }
            QLineEdit#historySearchInput {
                background-color: rgba(@surface-rgb, 0.8);
                color: @text;
                border: 1px solid rgba(@overlay-rgb, 0.08);
                border-radius: 8px;
                padding: 4px 10px;
                font-size: 12px;
            }
            QLineEdit#historySearchInput:focus {
                border: 1px solid @accent;
                background-color: rgba(@surface-rgb, 1.0);
            }
            QLineEdit#historySearchInput::placeholder {
                color: @text-muted;
            }
            QScrollArea#historyScrollArea {
                background-color: transparent;
                border: none;
            }
            QScrollArea#historyScrollArea > QWidget > QWidget {
                background-color: transparent;
            }
            QScrollArea#historyScrollArea QScrollBar:vertical {
                background: transparent;
                width: 8px;
                margin: 0px;
            }
            QScrollArea#historyScrollArea QScrollBar::handle:vertical {
                background: rgba(@overlay-rgb, 0.15);
                border-radius: 4px;
                min-height: 30px;
            }
            QScrollArea#historyScrollArea QScrollBar::handle:vertical:hover {
                background: rgba(@overlay-rgb, 0.3);
            }
            QScrollArea#historyScrollArea QScrollBar::add-line:vertical,
            QScrollArea#historyScrollArea QScrollBar::sub-line:vertical {
                height: 0px;
            }
            QScrollArea#historyScrollArea QScrollBar::add-page:vertical,
            QScrollArea#historyScrollArea QScrollBar::sub-page:vertical {
                background: transparent;
            }
        )qss");

// _make_empty_label's sheet.
const QString kEmptyLabelStyle =
    QStringLiteral("color: @text-muted; font-size: 12px; padding: 8px 0px;");

QString text(const std::string& utf8) {
    return qt::toQString(utf8);
}

// Python's entry_id[:8], for the log.
std::string shortId(const QString& entryId) {
    return qt::toUtf8(entryId.left(8));
}

// _unique_entries: one card per entry. A repeated id would get two cards but one slot in the
// card table, and the extra card would outlive the next refresh.
std::vector<HistoryEntry> uniqueEntries(std::vector<HistoryEntry> entries) {
    std::vector<HistoryEntry> shown;
    std::set<std::string> seen;
    for (HistoryEntry& entry : entries) {
        if (seen.insert(entry.id).second) {
            shown.push_back(std::move(entry));
        }
    }
    return shown;
}

// _clear_layout: every item goes now, not at the next event-loop pass: a card left showing
// until then reads as one more entry than the header says.
void clearLayout(QVBoxLayout* layout) {
    while (layout->count() > 0) {
        QLayoutItem* item = layout->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->setParent(nullptr);
            widget->deleteLater();
        }
        delete item;
    }
}

bool copyToClipboard(const QString& value) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }
    clipboard->setText(value);
    return true;
}

} // namespace

template <class Work, class Done> void HistorySidebar::inBackground(Work work, Done done) {
    history_detail::runInBackground(io_, ui_, std::move(work), std::move(done));
}

HistorySidebar::HistorySidebar(std::shared_ptr<HistoryStore> store, Executor& io,
                               SettingsStore* settings, QWidget* parent)
    : HistorySidebar(std::move(store), io, settings, system_clock(), parent) {}

HistorySidebar::HistorySidebar(std::shared_ptr<HistoryStore> store, Executor& io,
                               SettingsStore* settings, const Clock& clock, QWidget* parent)
    : QWidget(parent), runMessageBox([](QMessageBox& box) { return box.exec(); }),
      copyText(&copyToClipboard), openFolder(&openFolderInFileManager), store_(std::move(store)),
      io_(io), settings_(settings), ui_(std::make_shared<qt::QtUiExecutor>(this)),
      loadCanceled_(std::make_shared<std::atomic<bool>>(false)) {
    setupUi(clock);
    applyStyle();

    setMinimumWidth(kCollapsedWidth);
    setMaximumWidth(kCollapsedWidth);
}

HistorySidebar::~HistorySidebar() {
    // A load still running stops before it hands anything back.
    loadCanceled_->store(true);
}

void HistorySidebar::setupUi(const Clock& clock) {
    setObjectName(QStringLiteral("historySidebar"));
    setAttribute(Qt::WA_StyledBackground, true);

    // Fixed-width content pinned in resizeEvent (no layout on the sidebar itself): children
    // are clipped to the parent, so animating the width reveals the content without laying it
    // out again.
    content_ = new QWidget(this);
    content_->setObjectName(QStringLiteral("sidebarContent"));
    content_->setAttribute(Qt::WA_StyledBackground, true);
    content_->setFixedWidth(kExpandedWidth);

    auto* contentLayout = new QVBoxLayout(content_);
    contentLayout->setContentsMargins(16, 16, 16, 16);
    contentLayout->setSpacing(12);

    auto* headerLayout = new QHBoxLayout;
    headerLayout->setSpacing(8);

    menuButton_ = new QPushButton(QStringLiteral("☰"));
    menuButton_->setObjectName(QStringLiteral("sidebarMenuBtn"));
    menuButton_->setFixedSize(28, 28);
    menuButton_->setCursor(Qt::PointingHandCursor);
    connect(menuButton_, &QPushButton::clicked, this, [this] {
        QMenu* menu = buildHeaderMenu();
        menu->exec(menuButton_->mapToGlobal(menuButton_->rect().bottomLeft()));
        menu->deleteLater();
    });
    headerLayout->addWidget(menuButton_);

    headerLabel_ = new QLabel(QStringLiteral("History"));
    headerLabel_->setObjectName(QStringLiteral("sidebarHeader"));
    headerLabel_->setFont(QFont(QStringLiteral("Segoe UI"), 16, QFont::Bold));
    headerLayout->addWidget(headerLabel_);

    headerLayout->addStretch();

    calendarButton_ = new CalendarGlyphButton(clock);
    calendarButton_->setToolTip(QStringLiteral("See history on a calendar (Ctrl+Shift+D)"));
    connect(calendarButton_, &CalendarGlyphButton::calendarRequested, this,
            &HistorySidebar::calendarRequested);
    headerLayout->addWidget(calendarButton_);

    contentLayout->addLayout(headerLayout);

    searchInput_ = new QLineEdit;
    searchInput_->setObjectName(QStringLiteral("historySearchInput"));
    searchInput_->setPlaceholderText(QStringLiteral("Search history..."));
    searchInput_->setClearButtonEnabled(true);
    searchInput_->setFixedHeight(32);
    connect(searchInput_, &QLineEdit::textChanged, this, &HistorySidebar::onSearchTextChanged);
    contentLayout->addWidget(searchInput_);

    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(kSearchDelayMs);
    connect(searchTimer_, &QTimer::timeout, this, &HistorySidebar::loadHistory);

    scrollArea_ = new QScrollArea;
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setObjectName(QStringLiteral("historyScrollArea"));

    auto* scrollContent = new QWidget;
    // The scroll area sizes the content to the viewport even when a child's minimum is wider
    // (an unbreakable URL in a preview): overflow clips inside its own card instead of pushing
    // the panel past the sidebar's edge.
    scrollContent->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* scrollLayout = new QVBoxLayout(scrollContent);
    scrollLayout->setContentsMargins(0, 0, 6, 0);
    scrollLayout->setSpacing(12);
    scrollLayout->setAlignment(Qt::AlignTop);

    // The panel title already says History; this line only counts the cards, and hides while
    // there are none.
    historyHeader_ = new QLabel(QString());
    historyHeader_->setObjectName(QStringLiteral("sectionHeader"));
    historyHeader_->hide();
    historyHeader_->setFont(QFont(QStringLiteral("Segoe UI"), 11, QFont::DemiBold));
    historyHeader_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    scrollLayout->addWidget(historyHeader_);

    historyList_ = new QVBoxLayout;
    historyList_->setSpacing(12);
    scrollLayout->addLayout(historyList_);

    scrollArea_->setWidget(scrollContent);
    contentLayout->addWidget(scrollArea_, 1);

    // One animation drives the sidebar's width and, through widthAnimated, the window's, with
    // the timing of the app's other collapsible sections.
    animation_ = new QPropertyAnimation(this, "sidebarWidth", this);
    animation_->setDuration(kCollapseDurationMs);
    animation_->setEasingCurve(kCollapseEasing);
    connect(animation_, &QPropertyAnimation::finished, this, &HistorySidebar::onAnimationFinished);
}

void HistorySidebar::applyStyle() {
    ui::setLocalStyleSheet(this, kSidebarStyle);
}

void HistorySidebar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const QRect geometry(0, 0, kExpandedWidth, height());
    content_->setGeometry(geometry);
    for (const auto& [page, widget] : pageWidgets_) {
        if (widget != nullptr) {
            widget->setGeometry(geometry);
        }
    }
}

void HistorySidebar::setSidebarWidth(int width) {
    currentWidth_ = width;
    setMinimumWidth(currentWidth_);
    setMaximumWidth(currentWidth_);
    Q_EMIT widthAnimated(currentWidth_);
}

void HistorySidebar::onAnimationFinished() {
    const int target = expanded_ ? kExpandedWidth : kCollapsedWidth;
    setMinimumWidth(target);
    setMaximumWidth(target);
}

QAbstractAnimation* HistorySidebar::widthAnimation() const {
    return animation_;
}

void HistorySidebar::expand() {
    if (expanded_) {
        return;
    }
    expanded_ = true;

    // Filled before the animation, so the first open reveals a laid-out list instead of popping
    // it in afterwards.
    if (refreshPending_) {
        refresh();
        content_->ensurePolished();
        if (QLayout* layout = content_->layout()) {
            layout->activate();
        }
    }

    animation_->stop();
    animation_->setStartValue(currentWidth_);
    animation_->setEndValue(kExpandedWidth);
    animation_->start();
    kLog.debug("Sidebar expanding");
}

void HistorySidebar::stopVisiblePlayback() {
    for (const auto& [id, entry] : cards_) {
        if (auto* card = qobject_cast<HistoryItemWidget*>(entry.card);
            card != nullptr && card->playback() != nullptr) {
            card->playback()->stop();
        }
    }
    playback::stopAll();
}

void HistorySidebar::collapse() {
    if (!expanded_) {
        return;
    }
    expanded_ = false;
    stopVisiblePlayback();

    animation_->stop();
    animation_->setStartValue(currentWidth_);
    animation_->setEndValue(kCollapsedWidth);
    animation_->start();
    kLog.debug("Sidebar collapsing");
}

void HistorySidebar::toggle() {
    if (expanded_) {
        collapse();
    } else {
        expand();
    }
}

HistorySidebar::Page HistorySidebar::pageForTab(int tab) {
    switch (tab) {
    case TabbedContentWidget::TabMeetingMode:
        return Page::Meetings;
    case TabbedContentWidget::TabWalkthrough:
        return Page::Walkthroughs;
    default:
        return Page::History;
    }
}

void HistorySidebar::setPageWidget(Page page, QWidget* widget) {
    if (page == Page::History || widget == nullptr) {
        return;
    }
    widget->setParent(this);
    widget->setFixedWidth(kExpandedWidth);
    widget->setGeometry(0, 0, kExpandedWidth, height());
    widget->setVisible(page_ == page);
    pageWidgets_[page] = widget;
}

void HistorySidebar::setPage(Page page) {
    if (page == page_) {
        return;
    }
    page_ = page;
    if (page != Page::History) {
        stopVisiblePlayback();
    }
    content_->setVisible(page == Page::History);
    for (const auto& [shown, widget] : pageWidgets_) {
        if (widget != nullptr) {
            widget->setVisible(shown == page);
        }
    }
    refreshPending_ = true;
    if (expanded_) {
        refresh();
    }
}

void HistorySidebar::refresh() {
    if (!expanded_) {
        refreshPending_ = true;
        return;
    }
    refreshPending_ = false;
    if (page_ == Page::History) {
        loadHistory();
        return;
    }
    const auto found = pageWidgets_.find(page_);
    if (found != pageWidgets_.end() && found->second != nullptr) {
        QMetaObject::invokeMethod(found->second, "refresh", Qt::DirectConnection);
    }
}

QLabel* HistorySidebar::makeEmptyLabel(const QString& message) const {
    auto* label = new QLabel(message);
    ui::setLocalStyleSheet(label, kEmptyLabelStyle);
    label->setAlignment(Qt::AlignCenter);
    return label;
}

void HistorySidebar::onSearchTextChanged() {
    // An in-flight result is out of date at once; the replacement query starts after the user
    // pauses typing.
    gate_.next();
    searchTimer_->start();
}

void HistorySidebar::loadHistory() {
    const std::string query = py::lower(py::strip(qt::toUtf8(searchInput_->text())));
    const std::uint64_t generation = gate_.next();
    loadCanceled_->store(true);
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    loadCanceled_ = canceled;
    if (historyList_->count() == 0) {
        historyList_->addWidget(makeEmptyLabel(QStringLiteral("Loading history…")));
    }

    const std::int64_t limit = kMaxHistoryItems + 1;
    std::shared_ptr<HistoryStore> store = store_;
    std::shared_ptr<qt::QtUiExecutor> ui = ui_;
    // `this` is used only on the GUI thread, by a task the sidebar's executor drops once the
    // sidebar is gone.
    const auto deliver = [this, ui, generation, query](std::vector<HistoryEntry> entries,
                                                       std::string error, std::string notice) {
        ui->post([this, generation, query, entries = std::move(entries), error = std::move(error),
                  notice = std::move(notice)]() mutable {
            applyHistoryResults(generation, text(query), std::move(entries), text(error),
                                text(notice));
        });
    };
    io_.post([store, canceled, query, limit, deliver, records = remoteRecords_, &io = io_,
              wait = remoteMergeWait_] {
        std::vector<HistoryEntry> entries;
        std::string error;
        try {
            entries = query.empty() ? store->get_history(limit) : store->search(query, limit);
        } catch (const std::exception& failure) {
            kLog.error_with(
                failure, std::format("Failed to load transcription history: {}", failure.what()));
            error = failure.what();
            if (error.empty()) {
                error = "error";
            }
        }
        if (canceled->load()) {
            return;
        }
        if (!error.empty()) {
            deliver(std::move(entries), std::move(error), {});
            return;
        }
        const auto listing = startRemoteListing(records, io, query, limit, entries);
        if (!listing) {
            deliver(std::move(entries), {}, {});
            return;
        }
        std::unique_lock lock(listing->mutex);
        if (!listing->arrived.wait_for(lock, wait, [&] { return listing->done; })) {
            // Show this computer's entries now rather than wait on the host; the host's are
            // merged in when they come.
            listing->late = [deliver, canceled, listing] {
                if (!canceled->load()) {
                    auto [merged, notice] = listing->merged();
                    deliver(std::move(merged), {}, std::move(notice));
                }
            };
            lock.unlock();
            deliver(std::move(entries), {}, {});
            return;
        }
        lock.unlock();
        if (!canceled->load()) {
            auto [merged, notice] = listing->merged();
            deliver(std::move(merged), {}, std::move(notice));
        }
    });
}

std::shared_ptr<HistorySidebar::RemoteListing>
HistorySidebar::startRemoteListing(history::RemoteRecordSource* records, Executor& io,
                                   const std::string& query, std::int64_t limit,
                                   const std::vector<HistoryEntry>& entries) {
    if (records == nullptr) {
        return nullptr;
    }
    try {
        if (!records->listing_wanted("dictation")) {
            return nullptr;
        }
    } catch (const std::exception& error) {
        kLog.debug("Record sync unavailable for History: {}", error.what());
        return nullptr;
    }
    auto listing = std::make_shared<RemoteListing>();
    listing->records = records;
    listing->entries = entries;
    listing->limit = limit;
    io.post([listing, query] {
        std::vector<std::string> ids;
        for (const auto& entry : listing->entries) {
            ids.push_back(entry.id);
        }
        std::vector<std::string> copies;
        std::vector<Json> items;
        std::string notice;
        try {
            copies = listing->records->copies_on_host("dictation", ids);
            items =
                listing->records->list_remote("dictation", query, static_cast<int>(listing->limit));
        } catch (const std::exception& problem) {
            kLog.warning("Could not list host-kept History entries: {}", problem.what());
            notice = problem.what();
        }
        std::function<void()> late;
        {
            std::lock_guard lock(listing->mutex);
            listing->copies = std::move(copies);
            listing->items = std::move(items);
            listing->notice = std::move(notice);
            listing->done = true;
            late = std::move(listing->late);
        }
        listing->arrived.notify_all();
        if (late) {
            late();
        }
    });
    return listing;
}

std::pair<std::vector<HistoryEntry>, std::string> HistorySidebar::RemoteListing::merged() {
    // _start_remote_listing's merged(): this computer's entries with the host's merged in by
    // time, copies kept on both marked, and the host's note when it couldn't be asked.
    std::lock_guard lock(mutex);
    std::set<std::string> local;
    for (const auto& entry : entries) {
        local.insert(entry.id);
    }
    std::string host;
    for (const auto& item : items) {
        const auto found = item.find("stored_on");
        if (found != item.end() && found->is_string() && !found->get<std::string>().empty()) {
            host = found->get<std::string>();
        }
    }
    if (host.empty()) {
        try {
            host = records->host_name();
        } catch (const std::exception& error) {
            kLog.debug("Could not read the host's name for History: {}", error.what());
        }
    }
    std::vector<HistoryEntry> combined = entries;
    for (auto& entry : combined) {
        if (std::ranges::find(copies, entry.id) != copies.end()) {
            entry.also_on = host.empty() ? "the host" : host;
        }
    }
    for (const auto& item : items) {
        if (const auto id = item.find("id");
            id != item.end() && id->is_string() && local.contains(id->get<std::string>())) {
            continue;
        }
        try {
            combined.push_back(records->remote_history_entry(item));
        } catch (const std::exception& error) {
            kLog.warning("Skipped a host-kept History entry: {}", error.what());
        }
    }
    std::ranges::stable_sort(combined, std::greater{}, &HistoryEntry::timestamp);
    if (combined.size() > static_cast<std::size_t>(limit)) {
        combined.resize(static_cast<std::size_t>(limit));
    }
    return {std::move(combined), notice};
}

void HistorySidebar::applyHistoryResults(std::uint64_t generation, const QString& query,
                                         std::vector<HistoryEntry> entries, const QString& error,
                                         const QString& hostNotice) {
    if (generation != gate_.current()) {
        return;
    }
    if (!error.isEmpty()) {
        showHistoryMessage(QStringLiteral("History could not be loaded"));
        return;
    }

    remoteEntries_.clear();
    alsoOn_.clear();
    for (const HistoryEntry& entry : entries) {
        if (entry.stored_on && !entry.stored_on->empty()) {
            remoteEntries_[entry.id] = entry;
        }
        if (entry.also_on && !entry.also_on->empty()) {
            alsoOn_[entry.id] = *entry.also_on;
        }
    }
    const bool hasMore = entries.size() > static_cast<std::size_t>(kMaxHistoryItems);
    if (hasMore) {
        entries.resize(kMaxHistoryItems);
    }
    const std::vector<HistoryEntry> shown = uniqueEntries(std::move(entries));

    if (shown.empty()) {
        showHistoryMessage(query.isEmpty() ? QStringLiteral("No history yet")
                                           : QStringLiteral("No matching entries"));
        return;
    }

    std::vector<ReconcileItem> items;
    items.reserve(shown.size());
    for (const HistoryEntry& entry : shown) {
        std::optional<bool> audioAvailable;
        if (entry.audio_file && !entry.audio_file->empty() &&
            !(entry.stored_on && !entry.stored_on->empty())) {
            audioAvailable = store_->get_recording_path(*entry.audio_file).has_value();
        }
        items.push_back({entry.id, card_fingerprint(entry, audioAvailable)});
    }
    std::vector<QWidget*> made;
    try {
        cards_ = reconcileCards(historyList_, items, cards_, [&](std::size_t index) -> QWidget* {
            QWidget* card = createCard(shown[index]);
            made.push_back(card);
            return card;
        });
    } catch (...) {
        // Nothing reached the layout: the list keeps its old cards and the new ones go.
        for (QWidget* card : made) {
            delete card;
        }
        throw;
    }

    // Counted from the cards on screen, after they are built: set first, a card that failed to
    // build left the new count over the old list.
    const int rendered = renderedCardCount();
    historyHeader_->setText(
        QStringLiteral("%1%2 %3")
            .arg(rendered)
            .arg(hasMore ? QStringLiteral("+") : QString())
            .arg(rendered == 1 && !hasMore ? QStringLiteral("entry") : QStringLiteral("entries")));
    historyHeader_->setVisible(rendered > 0);
    if (!hostNotice.isEmpty()) {
        addRemoteNotice(hostNotice);
    }
    if (hasMore) {
        historyList_->addWidget(makeEmptyLabel(
            QStringLiteral("Showing the first %1 matches — refine your search to find older "
                           "entries")
                .arg(shown.size())));
    }
}

void HistorySidebar::addRemoteNotice(const QString& notice) {
    // Above the list: why the host's entries are missing.
    QLabel* note =
        makeEmptyLabel(notice.contains(QStringLiteral("reachable"))
                           ? notice + QStringLiteral(" Entries kept there show when it is.")
                           : notice);
    note->setObjectName(QStringLiteral("historyRemoteNotice"));
    note->setWordWrap(true);
    historyList_->insertWidget(0, note);
}

void HistorySidebar::showHistoryMessage(const QString& message) {
    clearLayout(historyList_);
    cards_.clear();
    historyHeader_->setText(QString());
    historyHeader_->hide();
    historyList_->addWidget(makeEmptyLabel(message));
}

HistoryItemWidget* HistorySidebar::createCard(const HistoryEntry& entry) {
    HistoryItemWidget* item = makeCard ? makeCard(entry) : new HistoryItemWidget(entry, *store_);
    if (remoteActions_ && entry.stored_on && !entry.stored_on->empty()) {
        item->setRemoteAudio(
            io_, [records = remoteActions_, id = entry.id] { return records->audio_for(id); });
    }
    // A card rebuilt in place: a click still waiting on its download plays in the new one.
    if (const auto replaced = cards_.find(entry.id);
        replaced != cards_.end() && item->playback() != nullptr) {
        if (auto* old = qobject_cast<HistoryItemWidget*>(replaced->second.card);
            old != nullptr && old != item && old->playback() != nullptr) {
            item->playback()->takeOver(old->playback());
        }
    }
    connect(item, &HistoryItemWidget::clicked, this, &HistorySidebar::onEntryClicked);
    connect(item, &HistoryItemWidget::copyRequested, this, &HistorySidebar::onCopyRequested);
    connect(item, &HistoryItemWidget::copyRawRequested, this, &HistorySidebar::onCopyRawRequested);
    connect(item, &HistoryItemWidget::deleteRequested, this, &HistorySidebar::onDeleteRequested);
    connect(item, &HistoryItemWidget::retranscribeRequested, this,
            &HistorySidebar::retranscribeRequested);
    connect(item, &HistoryItemWidget::versionRequested, this, &HistorySidebar::onVersionRequested);
    return item;
}

int HistorySidebar::renderedCardCount() const {
    std::set<const QWidget*> cards;
    for (int index = 0; index < historyList_->count(); ++index) {
        const QWidget* widget = historyList_->itemAt(index)->widget();
        if (qobject_cast<const HistoryItemWidget*>(widget) != nullptr) {
            cards.insert(widget);
        }
    }
    return static_cast<int>(cards.size());
}

void HistorySidebar::entryFor(const QString& entryId,
                              std::function<void(std::optional<HistoryEntry>)> done) {
    const std::string id = qt::toUtf8(entryId);
    inBackground(
        [store = store_, id]() -> std::optional<HistoryEntry> {
            try {
                return store->get_entry(id);
            } catch (const std::exception& error) {
                kLog.error_with(error,
                                std::format("Could not read a History entry: {}", error.what()));
                return std::nullopt;
            }
        },
        [this, id, done = std::move(done)](std::optional<HistoryEntry> entry) {
            if (!entry) {
                const auto remote = remoteEntries_.find(id);
                if (remote != remoteEntries_.end()) {
                    entry = remote->second;
                }
            }
            done(std::move(entry));
        });
}

void HistorySidebar::onEntryClicked(const QString& entryId) {
    entryFor(entryId, [this, entryId](std::optional<HistoryEntry> entry) {
        if (entry) {
            Q_EMIT entrySelected(entryId);
            kLog.debug("Entry selected: {}...", shortId(entryId));
        }
    });
}

void HistorySidebar::onCopyRequested(const QString& entryId) {
    entryFor(entryId, [this, entryId](std::optional<HistoryEntry> entry) {
        if (!entry) {
            return;
        }
        if (copyText(text(entry->text))) {
            Q_EMIT entryCopied(entryId);
            kLog.info("Copied entry to clipboard: {}...", shortId(entryId));
        } else {
            kLog.error("Failed to copy to clipboard: the clipboard is not available");
        }
    });
}

void HistorySidebar::onCopyRawRequested(const QString& entryId) {
    entryFor(entryId, [this, entryId](std::optional<HistoryEntry> entry) {
        if (!entry || !entry->raw_text || entry->raw_text->empty()) {
            return;
        }
        if (copyText(text(*entry->raw_text))) {
            Q_EMIT entryCopied(entryId);
            kLog.info("Copied raw entry to clipboard: {}...", shortId(entryId));
        } else {
            kLog.error("Failed to copy raw text to clipboard: the clipboard is not available");
        }
    });
}

void HistorySidebar::onVersionRequested(const QString& entryId, const QString& version) {
    inBackground(
        [store = store_, id = qt::toUtf8(entryId), wanted = qt::toUtf8(version)] {
            try {
                return store->use_version(id, wanted).has_value();
            } catch (const std::exception& error) {
                kLog.error_with(error, std::format("Could not change a History entry's version: {}",
                                                   error.what()));
                return false;
            }
        },
        [this](bool changed) {
            if (changed) {
                refresh();
            }
        });
}

bool HistorySidebar::deleteConfirmationWanted() const {
    if (settings_ == nullptr) {
        return true;
    }
    try {
        return settings_->get(keys::confirm_history_entry_delete);
    } catch (const std::exception& error) {
        kLog.warning("Failed to load history deletion preference: {}", error.what());
        return true;
    }
}

void HistorySidebar::setRemoteRecords(history::RemoteRecordSource* records,
                                      remote::RecordSyncControl* actions) {
    remoteRecords_ = records;
    remoteActions_ = actions;
}

void HistorySidebar::onDeleteRequested(const QString& entryId) {
    const std::string id = qt::toUtf8(entryId);
    const auto remote = remoteEntries_.find(id);
    const auto also = alsoOn_.find(id);
    const QString alsoOn = also != alsoOn_.end() ? text(also->second) : QString();

    if (remote != remoteEntries_.end()) {
        if (remoteActions_ &&
            (!deleteConfirmationWanted() || confirmDelete(&remote->second, {}, std::nullopt))) {
            deleteRemoteEntry(entryId);
        }
        return;
    }
    if (!deleteConfirmationWanted()) {
        deleteEntry(entryId, false);
        return;
    }
    // The audio choice is offered only for a recording still on disk.
    inBackground(
        [store = store_, id]() -> std::optional<std::filesystem::path> {
            try {
                const std::optional<HistoryEntry> entry = store->get_entry(id);
                if (entry && entry->audio_file && !entry->audio_file->empty()) {
                    return store->get_recording_path(*entry->audio_file);
                }
            } catch (const std::exception& error) {
                kLog.error_with(error,
                                std::format("Could not read a History entry: {}", error.what()));
            }
            return std::nullopt;
        },
        [this, entryId, alsoOn](std::optional<std::filesystem::path> audioPath) {
            const std::optional<bool> deleteAudio = confirmDelete(nullptr, alsoOn, audioPath);
            if (deleteAudio) {
                deleteEntry(entryId, *deleteAudio);
            }
        });
}

HistorySidebar::DeleteConfirmation
HistorySidebar::makeDeleteConfirmation(const HistoryEntry* remote, const QString& alsoOn,
                                       bool recordingSaved) {
    DeleteConfirmation made;
    made.box = std::make_unique<QMessageBox>(this);
    QMessageBox& confirmation = *made.box;
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.setWindowTitle(QStringLiteral("Delete History Entry"));
    if (remote != nullptr) {
        confirmation.setText(QStringLiteral("Delete this transcription from %1?")
                                 .arg(text(remote->stored_on.value_or(""))));
        confirmation.setInformativeText(
            QStringLiteral("Its recording there is deleted too. This cannot be undone."));
    } else {
        confirmation.setText(QStringLiteral("Delete this transcription from history?"));
        // OpenWhisper: "...is deleted too." Deleting here never deletes what the user shared
        // with the host (a17-retention), so the copy there is kept.
        confirmation.setInformativeText(
            alsoOn.isEmpty()
                ? QStringLiteral("This cannot be undone.")
                : QStringLiteral("This cannot be undone. Its copy on %1 is kept.").arg(alsoOn));
    }
    confirmation.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    confirmation.setDefaultButton(QMessageBox::No);

    // _add_audio_choice: offered for a local entry whose recording is still saved. As in
    // OpenWhisper, the rows go into the box's grid before setCheckBox, which lays the box out
    // again without them.
    if (remote == nullptr && recordingSaved) {
        auto* audioLabel =
            new QLabel(QStringLiteral("Delete saved audio file too?"), &confirmation);
        made.audioChoice = new QComboBox(&confirmation);
        made.audioChoice->addItem(QStringLiteral("No — keep the audio file"), false);
        made.audioChoice->addItem(QStringLiteral("Yes — permanently delete it"), true);
        made.audioChoice->setToolTip(QStringLiteral(
            "Choose whether the recording attached to this transcript should also be deleted"));
        if (auto* grid = qobject_cast<QGridLayout*>(confirmation.layout())) {
            const int row = grid->rowCount();
            const int columns = std::max(1, grid->columnCount());
            grid->addWidget(audioLabel, row, 0, 1, columns);
            grid->addWidget(made.audioChoice, row + 1, 0, 1, columns);
        }
    }

    made.dontAskAgain = new QCheckBox(QStringLiteral("Don't ask me again"), &confirmation);
    confirmation.setCheckBox(made.dontAskAgain);
    return made;
}

std::optional<bool>
HistorySidebar::confirmDelete(const HistoryEntry* remote, const QString& alsoOn,
                              const std::optional<std::filesystem::path>& audioPath) {
    const DeleteConfirmation confirmation =
        makeDeleteConfirmation(remote, alsoOn, audioPath.has_value());
    if (runMessageBox(*confirmation.box) != QMessageBox::Yes) {
        return std::nullopt;
    }

    const bool deleteAudioFile =
        confirmation.audioChoice != nullptr && confirmation.audioChoice->currentData().toBool();
    if (confirmation.dontAskAgain && confirmation.dontAskAgain->isChecked() &&
        settings_ != nullptr) {
        try {
            settings_->save_setting(keys::confirm_history_entry_delete.key(), false);
        } catch (const std::exception& error) {
            kLog.warning("Failed to save history deletion preference: {}", error.what());
        }
    }
    return deleteAudioFile;
}

void HistorySidebar::deleteEntry(const QString& entryId, bool deleteAudioFile) {
    inBackground(
        [store = store_, id = qt::toUtf8(entryId), deleteAudioFile] {
            try {
                return store->delete_entry(id, deleteAudioFile);
            } catch (const std::exception& error) {
                kLog.error_with(error,
                                std::format("Could not delete a History entry: {}", error.what()));
                return false;
            }
        },
        [this, entryId](bool deleted) {
            if (deleted) {
                Q_EMIT entryDeleted(entryId);
                refresh();
                kLog.info("Deleted entry: {}...", shortId(entryId));
            }
        });
}

void HistorySidebar::deleteRemoteEntry(const QString& entryId) {
    if (!remoteActions_) {
        return;
    }
    inBackground(
        [records = remoteActions_, store = store_, id = qt::toUtf8(entryId)]() -> QString {
            try {
                (void)records->delete_remote("dictation", id);
            } catch (const std::exception& problem) {
                kLog.warning("Could not delete a host-kept entry: {}", problem.what());
                const std::string reason = problem.what();
                return text(reason.empty() ? "error" : reason);
            }
            // Not the dictation "Paste original" offers any more.
            store->forget_dictation(id);
            return {};
        },
        [this, entryId](const QString& error) {
            if (!error.isEmpty()) {
                auto* notice =
                    new QMessageBox(QMessageBox::Warning, QStringLiteral("Delete History Entry"),
                                    QStringLiteral("The entry couldn't be deleted: %1").arg(error),
                                    QMessageBox::Ok, this);
                notice->setTextFormat(Qt::PlainText);
                notice->setAttribute(Qt::WA_DeleteOnClose);
                notice->open();
                return;
            }
            Q_EMIT entryDeleted(entryId);
            refresh();
        });
}

void HistorySidebar::deleteEntryFromDialog(const HistoryEntry& entry) {
    if (entry.stored_on && !entry.stored_on->empty()) {
        deleteRemoteEntry(text(entry.id));
        return;
    }
    const QString entryId = text(entry.id);
    inBackground(
        [store = store_, id = entry.id] {
            try {
                return store->delete_entry(id, false);
            } catch (const std::exception& error) {
                kLog.error_with(error,
                                std::format("Could not delete a History entry: {}", error.what()));
                return false;
            }
        },
        [this, entryId](bool deleted) {
            if (deleted) {
                refresh();
                Q_EMIT entryDeleted(entryId);
                kWindowLog.info("Deleted history entry from dialog: {}...", shortId(entryId));
            }
        });
}

QMenu* HistorySidebar::buildHeaderMenu() {
    QMenu* menu = contextMenu(this);
    connect(menu->addAction(QStringLiteral("Export history…")), &QAction::triggered, this,
            &HistorySidebar::onExportHistory);
    connect(menu->addAction(QStringLiteral("Open recordings folder")), &QAction::triggered, this,
            &HistorySidebar::onOpenRecordingsFolder);
    menu->addSeparator();
    connect(menu->addAction(QStringLiteral("Clear history")), &QAction::triggered, this,
            &HistorySidebar::onClearHistory);
    connect(menu->addAction(QStringLiteral("Clear history + recordings")), &QAction::triggered,
            this, &HistorySidebar::onClearHistoryAndRecordings);
    return menu;
}

bool HistorySidebar::askToClear(const QString& title, const QString& question) {
    // QMessageBox.question: the question icon, Yes and No, No the default. OpenWhisper adds
    // "This computer's entries on <host> are deleted too." while records sync; here a clear
    // leaves what the user shared with the host where it is (a17-retention), so there is no
    // such note.
    QMessageBox box(QMessageBox::Question, title, question, QMessageBox::Yes | QMessageBox::No,
                    this);
    box.setDefaultButton(QMessageBox::No);
    return runMessageBox(box) == QMessageBox::Yes;
}

void HistorySidebar::onClearHistory() {
    if (!askToClear(QStringLiteral("Clear History"),
                    QStringLiteral("Delete all history entries?\n\nSaved recordings will be "
                                   "kept."))) {
        return;
    }
    inBackground(
        [store = store_] {
            try {
                store->clear_history();
            } catch (const std::exception& error) {
                kLog.error_with(error, std::format("Could not clear History: {}", error.what()));
            }
            return true;
        },
        [this](bool) { refresh(); });
}

void HistorySidebar::onClearHistoryAndRecordings() {
    if (!askToClear(QStringLiteral("Clear History and Recordings"),
                    QStringLiteral("Delete all history entries AND permanently delete all saved "
                                   "recordings from disk?\n\nThis cannot be undone."))) {
        return;
    }
    inBackground(
        [store = store_] {
            try {
                store->clear_history_and_recordings();
            } catch (const std::exception& error) {
                kLog.error_with(error, std::format("Could not clear History and its recordings: {}",
                                                   error.what()));
            }
            return true;
        },
        [this](bool) { refresh(); });
}

void HistorySidebar::onExportHistory() {
    Q_EMIT exportRequested();
}

void HistorySidebar::onOpenRecordingsFolder() {
    std::error_code error;
    const std::filesystem::path folder =
        std::filesystem::absolute(store_->recordings_folder(), error);
    std::filesystem::create_directories(folder, error);
    if (error) {
        kLog.warning("Could not create the recordings folder: {}", error.message());
    }
    openFolder(qt::toQString(path_to_utf8(folder)));
}

} // namespace fv
