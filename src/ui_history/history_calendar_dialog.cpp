#include "ui_history/history_calendar_dialog.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <format>
#include <mutex>
#include <utility>
#include <variant>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/log.h"
#include "history/format_utils.h"
#include "ui_history/detail/calendar_relay.h"
#include "ui_history/history_calendar.h"
#include "ui_theme/local_style.h"

namespace fv {

namespace history_calendar_detail {

CalendarRelay& CalendarRelay::instance() {
    // Never deleted: a worker may emit on it after every dialog, and the application, is gone.
    static CalendarRelay* const relay = new CalendarRelay;
    return *relay;
}

std::uint64_t CalendarRelay::nextToken() {
    static std::atomic<std::uint64_t> tokens{0};
    return tokens.fetch_add(1) + 1;
}

} // namespace history_calendar_detail

namespace {

using history::Day;
using history::MonthKey;
using history_calendar_detail::CalendarRelay;
using history_calendar_detail::Items;
using history_calendar_detail::Marks;
namespace cv = calendar_view;

constexpr log::Logger kLog{"ui_qt.dialogs.history_calendar_dialog"};

QString q(std::string_view utf8) {
    return qt::toQString(utf8);
}

constexpr char kStyle[] = R"(
    QDialog#historyCalendarDialog {
        background-color: @bg;
    }
    QPushButton#calendarMonthTitle {
        color: @text-heading;
        background: transparent;
        border: none;
        padding: 0px 30px 0px 8px;
        text-align: left;
        font-size: 24px;
        font-weight: 700;
    }
    QPushButton#calendarNavBtn {
        background-color: @surface;
        color: @text;
        border: 1px solid rgba(@overlay-rgb, 0.06);
        border-radius: 16px;
        padding: 0px;
        font-size: 18px;
        font-weight: 600;
    }
    QPushButton#calendarNavBtn:hover {
        background-color: @surface-hover;
        color: @accent-soft;
    }
    QPushButton#calendarTodayBtn {
        background-color: rgba(@accent-rgb, 0.14);
        color: @accent-soft;
        border: 1px solid rgba(@accent-rgb, 0.3);
        border-radius: 14px;
        padding: 4px 14px;
        font-size: 12px;
        font-weight: 600;
    }
    QPushButton#calendarTodayBtn:hover {
        background-color: rgba(@accent-rgb, 0.24);
    }
    QPushButton#calendarKindChip {
        background-color: transparent;
        color: @text-secondary;
        border: 1px solid rgba(@overlay-rgb, 0.1);
        border-radius: 14px;
        padding: 4px 12px 4px 26px;
        font-size: 12px;
        font-weight: 600;
    }
    QPushButton#calendarKindChip:hover {
        background-color: rgba(@overlay-rgb, 0.05);
    }
    QPushButton#calendarKindChip:checked {
        background-color: @surface;
        color: @text;
        border: 1px solid rgba(@overlay-rgb, 0.14);
    }
    QComboBox#calendarPlaceCombo {
        padding: 4px 12px;
        font-size: 12px;
        border-radius: 14px;
    }
    QLabel#calendarStats {
        color: @text-secondary;
        background: transparent;
        font-size: 13px;
    }
    QPushButton#calendarBusiestBtn {
        background: transparent;
        color: @accent-soft;
        border: none;
        padding: 0px 2px;
        font-size: 13px;
        font-weight: 600;
    }
    QPushButton#calendarBusiestBtn:hover {
        color: @accent;
        text-decoration: underline;
    }
    QLabel#calendarStreak {
        background-color: rgba(@warning-rgb, 0.14);
        color: @warning-text;
        border: 1px solid rgba(@warning-rgb, 0.3);
        border-radius: 11px;
        padding: 2px 10px;
        font-size: 12px;
        font-weight: 600;
    }
    QLabel#calendarNotice, QLabel#calendarHint {
        color: @text-muted;
        background: transparent;
        font-size: 11px;
    }
    QFrame#calendarAgenda {
        background-color: rgba(@surface-rgb, 0.45);
        border: 1px solid rgba(@overlay-rgb, 0.06);
        border-radius: 14px;
    }
    QLabel#calendarAgendaWeekday {
        color: @accent-soft;
        background: transparent;
        font-size: 13px;
        font-weight: 600;
    }
    QLabel#calendarTodayBadge {
        color: @on-accent;
        background-color: @accent;
        border-radius: 8px;
        padding: 1px 8px;
        font-size: 10px;
        font-weight: 700;
    }
    QLabel#calendarAgendaDate {
        color: @text-heading;
        background: transparent;
        font-size: 21px;
        font-weight: 700;
    }
    QLabel#calendarAgendaSummary {
        color: @text-secondary;
        background: transparent;
        font-size: 12px;
    }
    QScrollArea#calendarAgendaScroll, QWidget#calendarAgendaList,
    QWidget#calendarEmptyState {
        background: transparent;
        border: none;
    }
    QScrollArea#calendarAgendaScroll QScrollBar:vertical {
        background: transparent;
        width: 8px;
    }
    QScrollArea#calendarAgendaScroll QScrollBar::handle:vertical {
        background: rgba(@overlay-rgb, 0.15);
        border-radius: 4px;
        min-height: 30px;
    }
    QScrollArea#calendarAgendaScroll QScrollBar::add-line:vertical,
    QScrollArea#calendarAgendaScroll QScrollBar::sub-line:vertical {
        height: 0px;
    }
    QFrame#calendarAgendaCard {
        background-color: rgba(@surface-rgb, 0.8);
        border: 1px solid rgba(@overlay-rgb, 0.05);
        border-left: 3px solid @accent;
        border-radius: 10px;
    }
    QFrame#calendarAgendaCard[kind="file"] {
        border-left: 3px solid @purple;
    }
    QFrame#calendarAgendaCard[kind="meeting"] {
        border-left: 3px solid @warning;
    }
    QFrame#calendarAgendaCard:hover {
        background-color: rgba(@surface-hover-rgb, 0.9);
        border-top: 1px solid rgba(@accent-rgb, 0.35);
        border-right: 1px solid rgba(@accent-rgb, 0.35);
        border-bottom: 1px solid rgba(@accent-rgb, 0.35);
    }
    QLabel#calendarCardTime {
        color: @text-secondary-strong;
        background: transparent;
        font-size: 11px;
        font-weight: 600;
    }
    QLabel#calendarKindPill {
        color: @accent-soft;
        background-color: rgba(@accent-rgb, 0.14);
        border-radius: 6px;
        padding: 1px 7px;
        font-size: 10px;
        font-weight: 700;
    }
    QLabel#calendarKindPill[kind="file"] {
        color: @purple-text;
        background-color: rgba(@purple-rgb, 0.14);
    }
    QLabel#calendarKindPill[kind="meeting"] {
        color: @warning-text;
        background-color: rgba(@warning-rgb, 0.14);
    }
    QLabel#calendarPlaceChip {
        color: @success-text;
        background-color: rgba(@success-rgb, 0.10);
        border: 1px solid rgba(@success-rgb, 0.24);
        border-radius: 6px;
        padding: 0px 7px;
        font-size: 10px;
        font-weight: 600;
    }
    QLabel#calendarLengthChip {
        color: @text-tertiary;
        background-color: rgba(@overlay-rgb, 0.06);
        border-radius: 6px;
        padding: 1px 7px;
        font-size: 10px;
        font-weight: 600;
    }
    QLabel#calendarCardTitle {
        color: @text;
        background: transparent;
        font-size: 13px;
        font-weight: 600;
    }
    QLabel#calendarCardPreview {
        color: @text-body;
        background: transparent;
        font-size: 12px;
    }
    QLabel#calendarMilestone {
        color: @warning-text;
        background: transparent;
        font-size: 11px;
        font-weight: 700;
    }
    QLabel#calendarCardProgress {
        color: @text-secondary;
        background: transparent;
        font-size: 11px;
    }
    QLabel#calendarEmptyTitle {
        color: @text-soft;
        background: transparent;
        font-size: 14px;
        font-weight: 600;
    }
    QLabel#calendarEmptyBody {
        color: @text-secondary;
        background: transparent;
        font-size: 12px;
    }
    QPushButton#calendarJumpBtn {
        background-color: @surface;
        color: @accent-soft;
        border: 1px solid rgba(@overlay-rgb, 0.08);
        border-radius: 14px;
        padding: 5px 14px;
        font-size: 12px;
        font-weight: 600;
    }
    QPushButton#calendarJumpBtn:hover {
        background-color: @surface-hover;
    }
)";

// _python_first_weekday: the locale's first day of the week in Python's numbering.
int pythonFirstWeekday() {
    return (static_cast<int>(QLocale().firstDayOfWeek()) - 1 + 7) % 7;
}

std::string titleOf(MonthKey key) {
    return std::format("{} {}", cv::month_name(key.month), key.year);
}

int monthOf(Day day) {
    return static_cast<int>(static_cast<unsigned>(day.month()));
}

int dayNumber(Day day) {
    return static_cast<int>(static_cast<unsigned>(day.day()));
}

int yearOf(Day day) {
    return static_cast<int>(day.year());
}

Day firstOf(MonthKey key) {
    return Day{std::chrono::year{key.year}, std::chrono::month{static_cast<unsigned>(key.month)},
               std::chrono::day{1}};
}

bool copyToClipboard(const QString& text) {
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return false;
    }
    clipboard->setText(text);
    return true;
}

// The source's reads, one call at a time (CalendarSource reuses what load_remote listed).
class SourceReads final : public CalendarReads {
public:
    explicit SourceReads(history::CalendarSource& source) : source_(source) {}
    explicit SourceReads(std::shared_ptr<history::CalendarSource> source)
        : owned_(std::move(source)), source_(*owned_) {}

    std::vector<history::Mark> load_index() override {
        std::lock_guard lock(mutex_);
        return source_.load_index();
    }
    bool remote_wanted() override {
        std::lock_guard lock(mutex_);
        return source_.remote_wanted();
    }
    std::vector<history::Mark> load_remote(const std::vector<std::string>& local_ids) override {
        std::lock_guard lock(mutex_);
        return source_.load_remote(local_ids);
    }
    std::vector<history::CalendarItem> load_month(int year, int month) override {
        std::lock_guard lock(mutex_);
        return source_.load_month(year, month);
    }
    std::string notice() override {
        std::lock_guard lock(mutex_);
        return source_.notice();
    }

private:
    std::shared_ptr<history::CalendarSource> owned_;
    history::CalendarSource& source_;
    std::mutex mutex_;
};

// _load_months. Runs on io; reaches the dialog only through the relay.
void loadMonths(CalendarReads& reads, CalendarRelay& relay, std::uint64_t generation,
                const std::vector<MonthKey>& keys) {
    for (const MonthKey& key : keys) {
        auto items = std::make_shared<std::vector<history::CalendarItem>>();
        try {
            *items = reads.load_month(key.year, key.month);
        } catch (const std::exception& error) {
            kLog.error("Couldn't read {} for the History calendar: {}", titleOf(key), error.what());
            items->clear();
        }
        Q_EMIT relay.monthLoaded(generation, key.year, key.month, Items{std::move(items)});
    }
}

// _load_all: the index, the shown month, the host's records, then the neighbours.
void loadAll(CalendarReads& reads, CalendarRelay& relay, std::uint64_t generation, MonthKey view,
             int prefetch) {
    std::vector<history::Mark> marks;
    try {
        marks = reads.load_index();
    } catch (const std::exception& error) {
        kLog.error("Couldn't read the History calendar: {}", error.what());
        Q_EMIT relay.indexLoaded(generation, Marks{},
                                 QStringLiteral("History could not be loaded."));
        return;
    }
    Q_EMIT relay.indexLoaded(
        generation, Marks{std::make_shared<const std::vector<history::Mark>>(marks)}, QString());
    loadMonths(reads, relay, generation, {view});
    bool remoteWanted = false;
    try {
        remoteWanted = reads.remote_wanted();
    } catch (const std::exception& error) {
        kLog.error("Couldn't read the History calendar: {}", error.what());
    }
    if (remoteWanted) {
        std::vector<std::string> localIds;
        localIds.reserve(marks.size());
        for (const history::Mark& mark : marks) {
            localIds.push_back(mark.record_id);
        }
        std::vector<history::Mark> remote;
        std::string notice;
        try {
            remote = reads.load_remote(localIds);
            notice = reads.notice();
        } catch (const std::exception& error) {
            kLog.error("Couldn't read the History calendar: {}", error.what());
        }
        auto all = std::make_shared<std::vector<history::Mark>>(marks);
        all->insert(all->end(), remote.begin(), remote.end());
        Q_EMIT relay.indexLoaded(generation, Marks{std::move(all)}, q(notice));
        if (!remote.empty()) {
            loadMonths(reads, relay, generation, {view});
        }
    }
    std::vector<MonthKey> neighbours;
    for (int step = -prefetch; step <= prefetch; ++step) {
        if (step != 0) {
            neighbours.push_back(history::shift_month(view, step));
        }
    }
    loadMonths(reads, relay, generation, neighbours);
}

QWidget* emptyState(const QString& title, const QString& body) {
    auto* widget = new QWidget();
    widget->setObjectName(QStringLiteral("calendarEmptyState"));
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(2, 18, 10, 0);
    layout->setSpacing(6);
    auto* heading = new QLabel(title);
    heading->setObjectName(QStringLiteral("calendarEmptyTitle"));
    heading->setWordWrap(true);
    layout->addWidget(heading);
    if (!body.isEmpty()) {
        auto* text = new QLabel(body);
        text->setObjectName(QStringLiteral("calendarEmptyBody"));
        text->setWordWrap(true);
        layout->addWidget(text);
    }
    layout->addSpacing(6);
    return widget;
}

} // namespace

std::shared_ptr<CalendarReads> calendarReadsOf(history::CalendarSource& source) {
    return std::make_shared<SourceReads>(source);
}

std::shared_ptr<CalendarReads> calendarReadsOf(std::shared_ptr<history::CalendarSource> source) {
    return std::make_shared<SourceReads>(std::move(source));
}

HistoryCalendarDialog::HistoryCalendarDialog(history::CalendarSource& source, Executor& io,
                                             QWidget* parent)
    : HistoryCalendarDialog(calendarReadsOf(source), io, system_clock(), std::nullopt, parent) {}

HistoryCalendarDialog::HistoryCalendarDialog(std::shared_ptr<CalendarReads> reads, Executor& io,
                                             const Clock& clock, std::optional<int> firstWeekday,
                                             QWidget* parent)
    : QDialog(parent), copyText(&copyToClipboard), reads_(std::move(reads)), io_(io), clock_(clock),
      firstWeekday_(firstWeekday.value_or(pythonFirstWeekday())),
      relay_(&CalendarRelay::instance()), generation_(CalendarRelay::nextToken()),
      view_(history::month_key(localToday(clock))), selected_(localToday(clock)),
      token_(CalendarRelay::nextToken()) {
    setObjectName(QStringLiteral("historyCalendarDialog"));
    setWindowTitle(QStringLiteral("History Calendar"));
    setModal(false);
    setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
    // No explicit minimum: the layout's own keeps the grid and the agenda from ever
    // overlapping, whatever the UI font scale.
    fitToScreen(1180, 800);
    ui::setLocalStyleSheet(this, QString::fromUtf8(kStyle));

    // Changes elsewhere tend to arrive in bursts (a transcription refreshes History and Past
    // Meetings together), so their refreshes are merged.
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(kRefreshDelayMs);
    connect(refreshTimer_, &QTimer::timeout, this, &HistoryCalendarDialog::refresh);
    // Deferred work runs on timers the dialog owns, so none of it can fire after it is gone.
    pulseTimer_ = new QTimer(this);
    pulseTimer_->setSingleShot(true);
    pulseTimer_->setInterval(kPulseDelayMs);
    scrollTimer_ = new QTimer(this);
    scrollTimer_->setSingleShot(true);
    scrollTimer_->setInterval(0);

    connect(relay_, &CalendarRelay::indexLoaded, this,
            [this](quint64 generation, const Marks& marks, const QString& notice) {
                onIndexLoaded(generation, marks.marks, notice);
            });
    connect(relay_, &CalendarRelay::monthLoaded, this,
            [this](quint64 generation, int year, int month, const Items& items) {
                onMonthLoaded(generation, year, month, items.items);
            });
    connect(relay_, &CalendarRelay::fetchProgress, this, &HistoryCalendarDialog::onFetchProgress);
    connect(relay_, &CalendarRelay::fetchDone, this, &HistoryCalendarDialog::onFetchDone);
    setupUi();
    connect(pulseTimer_, &QTimer::timeout, grid_, &MonthGrid::pulseToday);
    connect(scrollTimer_, &QTimer::timeout, this,
            [this] { agendaScroll_->verticalScrollBar()->setValue(scrollTo_); });
}

HistoryCalendarDialog::~HistoryCalendarDialog() {
    // A picker still open hides as the dialog's children go, and its closed() must not reach
    // a month title that may be gone by then.
    disconnect(monthPicker_, nullptr, this, nullptr);
}

Day HistoryCalendarDialog::today() const {
    return localToday(clock_);
}

void HistoryCalendarDialog::setMeetingCheckOut(MeetingCheckOut checkOut) {
    checkOut_ = std::move(checkOut);
}

void HistoryCalendarDialog::setPairedMeetingCheckOut(MeetingCheckOut checkOut) {
    pairedCheckOut_ = std::move(checkOut);
}

KindChip* HistoryCalendarDialog::kindChip(std::string_view kind) const {
    const auto found = kindChips_.find(kind);
    return found == kindChips_.end() ? nullptr : found->second;
}

AgendaCard* HistoryCalendarDialog::card(std::string_view recordId) const {
    for (AgendaCard* card : cards_) {
        if (card->item().record_id == recordId) {
            return card;
        }
    }
    return nullptr;
}

// ---- building ----

void HistoryCalendarDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 18, 22, 14);
    root->setSpacing(10);

    auto* header = new QHBoxLayout();
    header->setSpacing(8);
    // Today and the arrows come first so they stay put while the month's name changes length
    // beside them.
    todayButton_ = new QPushButton(QStringLiteral("Today"));
    todayButton_->setObjectName(QStringLiteral("calendarTodayBtn"));
    todayButton_->setCursor(Qt::PointingHandCursor);
    todayButton_->setFixedHeight(30);
    todayButton_->setToolTip(QStringLiteral("Go to today (T)"));
    connect(todayButton_, &QPushButton::clicked, this, &HistoryCalendarDialog::showToday);
    header->addWidget(todayButton_, 0, Qt::AlignVCenter);
    header->addSpacing(4);
    prevButton_ = new QPushButton(QStringLiteral(u"‹"));
    nextButton_ = new QPushButton(QStringLiteral(u"›"));
    for (const auto& [button, tip] :
         {std::pair{prevButton_, QStringLiteral("Previous month (Page Up)")},
          std::pair{nextButton_, QStringLiteral("Next month (Page Down)")}}) {
        button->setObjectName(QStringLiteral("calendarNavBtn"));
        button->setFixedSize(32, 32);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tip);
    }
    prevButton_->setAccessibleName(QStringLiteral("Previous month"));
    nextButton_->setAccessibleName(QStringLiteral("Next month"));
    connect(prevButton_, &QPushButton::clicked, this, [this] { stepMonth(-1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { stepMonth(1); });
    header->addWidget(prevButton_);
    header->addWidget(nextButton_);
    header->addSpacing(2);
    monthTitle_ = new MonthTitleButton(q(titleOf(view_)));
    monthTitle_->setFixedHeight(40);
    monthTitle_->setToolTip(QStringLiteral("Choose a month or year (M)"));
    monthTitle_->setAccessibleName(QStringLiteral("Choose a month or year"));
    connect(monthTitle_, &QPushButton::clicked, this, &HistoryCalendarDialog::openMonthPicker);
    header->addWidget(monthTitle_, 0, Qt::AlignVCenter);
    monthPicker_ = new MonthPicker(clock_, this);
    connect(monthPicker_, &MonthPicker::monthChosen, this, &HistoryCalendarDialog::onMonthPicked);
    connect(monthPicker_, &MonthPicker::closed, this, [this] { monthTitle_->setOpen(false); });
    header->addSpacing(4);
    streakLabel_ = new QLabel(QString());
    streakLabel_->setObjectName(QStringLiteral("calendarStreak"));
    streakLabel_->setToolTip(QStringLiteral("Days in a row with a recording, up to today"));
    streakLabel_->setFixedHeight(24);
    streakLabel_->hide();
    header->addWidget(streakLabel_, 0, Qt::AlignVCenter);
    header->addStretch();

    for (const std::string_view kind : history::KINDS) {
        auto* chip = new KindChip(std::string(kind));
        connect(chip, &QPushButton::toggled, this, [this, chip](bool) { onKindToggled(chip); });
        kindChips_.emplace(std::string(kind), chip);
        header->addWidget(chip, 0, Qt::AlignVCenter);
    }
    placeCombo_ = new QComboBox();
    placeCombo_->setObjectName(QStringLiteral("calendarPlaceCombo"));
    placeCombo_->setToolTip(QStringLiteral("Where the records are kept"));
    connect(placeCombo_, &QComboBox::currentIndexChanged, this,
            &HistoryCalendarDialog::onPlaceChanged);
    placeCombo_->hide();
    header->addWidget(placeCombo_, 0, Qt::AlignVCenter);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(16);

    auto* monthColumn = new QVBoxLayout();
    monthColumn->setSpacing(8);
    auto* summary = new QHBoxLayout();
    summary->setSpacing(10);
    statsLabel_ = new QLabel(QString());
    statsLabel_->setObjectName(QStringLiteral("calendarStats"));
    statsLabel_->setTextFormat(Qt::RichText);
    summary->addWidget(statsLabel_, 0, Qt::AlignVCenter);
    busiestButton_ = new QPushButton(QString());
    busiestButton_->setObjectName(QStringLiteral("calendarBusiestBtn"));
    busiestButton_->setCursor(Qt::PointingHandCursor);
    busiestButton_->setToolTip(QStringLiteral("Show the day with the most recordings"));
    connect(busiestButton_, &QPushButton::clicked, this, &HistoryCalendarDialog::showBusiest);
    busiestButton_->hide();
    summary->addWidget(busiestButton_, 0, Qt::AlignVCenter);
    summary->addStretch();
    monthColumn->addLayout(summary);
    noticeLabel_ = new QLabel(QString());
    noticeLabel_->setObjectName(QStringLiteral("calendarNotice"));
    noticeLabel_->setWordWrap(true);
    noticeLabel_->hide();
    monthColumn->addWidget(noticeLabel_);
    grid_ = new MonthGrid(clock_);
    connect(grid_, &MonthGrid::daySelected, this,
            [this](const QDate& day) { selectDay(toDay(day)); });
    connect(grid_, &MonthGrid::dayActivated, this,
            [this](const QDate& day) { activateDay(toDay(day)); });
    connect(grid_, &MonthGrid::monthStep, this, &HistoryCalendarDialog::stepMonth);
    connect(grid_, &MonthGrid::todayRequested, this, &HistoryCalendarDialog::showToday);
    connect(grid_, &MonthGrid::pickerRequested, this, &HistoryCalendarDialog::openMonthPicker);
    monthColumn->addWidget(grid_, 1);
    body->addLayout(monthColumn, 1);

    auto* dayColumn = new QVBoxLayout();
    dayColumn->setSpacing(8);
    yearStrip_ = new YearStrip();
    connect(yearStrip_, &YearStrip::monthClicked, this,
            [this](int year, int month) { showMonth(MonthKey{year, month}); });
    dayColumn->addWidget(yearStrip_);
    dayColumn->addWidget(buildAgenda(), 1);
    body->addLayout(dayColumn);
    root->addLayout(body, 1);

    hintLabel_ = new QLabel(
        QStringLiteral(u"Arrow keys move between days · Page Up and Page Down change the month · "
                       u"T goes to today · Enter opens the day's first recording"));
    hintLabel_->setObjectName(QStringLiteral("calendarHint"));
    root->addWidget(hintLabel_);
}

QFrame* HistoryCalendarDialog::buildAgenda() {
    agenda_ = new QFrame();
    agenda_->setObjectName(QStringLiteral("calendarAgenda"));
    agenda_->setFixedWidth(348);
    agenda_->setMinimumHeight(240);
    auto* layout = new QVBoxLayout(agenda_);
    layout->setContentsMargins(16, 14, 8, 12);
    layout->setSpacing(4);

    auto* weekdayRow = new QHBoxLayout();
    weekdayRow->setContentsMargins(0, 0, 8, 0);
    weekdayRow->setSpacing(8);
    agendaWeekday_ = new QLabel(QString());
    agendaWeekday_->setObjectName(QStringLiteral("calendarAgendaWeekday"));
    weekdayRow->addWidget(agendaWeekday_);
    todayBadge_ = new QLabel(QStringLiteral("Today"));
    todayBadge_->setObjectName(QStringLiteral("calendarTodayBadge"));
    todayBadge_->hide();
    weekdayRow->addWidget(todayBadge_);
    weekdayRow->addStretch();
    layout->addLayout(weekdayRow);
    agendaDate_ = new QLabel(QString());
    agendaDate_->setObjectName(QStringLiteral("calendarAgendaDate"));
    layout->addWidget(agendaDate_);
    agendaSummary_ = new QLabel(QString());
    agendaSummary_->setObjectName(QStringLiteral("calendarAgendaSummary"));
    agendaSummary_->setWordWrap(true);
    layout->addWidget(agendaSummary_);
    layout->addSpacing(8);

    agendaScroll_ = new QScrollArea();
    agendaScroll_->setObjectName(QStringLiteral("calendarAgendaScroll"));
    agendaScroll_->setWidgetResizable(true);
    agendaScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    agendaList_ = new QWidget();
    agendaList_->setObjectName(QStringLiteral("calendarAgendaList"));
    agendaList_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    agendaLayout_ = new QVBoxLayout(agendaList_);
    agendaLayout_->setContentsMargins(0, 0, 8, 0);
    agendaLayout_->setSpacing(10);
    agendaLayout_->setAlignment(Qt::AlignTop);
    agendaScroll_->setWidget(agendaList_);
    layout->addWidget(agendaScroll_, 1);
    return agenda_;
}

void HistoryCalendarDialog::fitToScreen(int width, int height) {
    const QWidget* parent = parentWidget();
    const QScreen* screen = parent != nullptr ? parent->screen() : QGuiApplication::primaryScreen();
    if (screen != nullptr) {
        const QRect room = screen->availableGeometry();
        width = std::min(width, room.width() - 40);
        height = std::min(height, room.height() - 60);
    }
    resize(width, height);
}

// ---- loading ----

void HistoryCalendarDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!event->spontaneous()) {
        // Opened again: back to today, with the pulse that shows where it is. Restoring a
        // minimized window keeps its place.
        const Day now = today();
        if (loaded_ && (view_ != history::month_key(now) || selected_ != now)) {
            showMonth(history::month_key(now), now);
        }
        pulsePending_ = true;
    }
    if (stale_) {
        refresh();
    } else if (pulsePending_) {
        pulsePending_ = false;
        pulseTimer_->start();
    }
    grid_->setFocus();
}

void HistoryCalendarDialog::refresh() {
    refreshTimer_->stop();
    stale_ = false;
    const std::uint64_t generation = CalendarRelay::nextToken();
    generation_ = generation;
    fresh_.clear();
    const MonthKey view = view_;
    // loadAll reads these; render must not ask for them again meanwhile.
    loading_.clear();
    for (int step = -kPrefetch; step <= kPrefetch; ++step) {
        loading_.insert(history::shift_month(view, step));
    }
    // Bound here: the worker must not reach back into the dialog.
    run([reads = reads_, relay = relay_, generation, view] {
        loadAll(*reads, *relay, generation, view, kPrefetch);
    });
}

void HistoryCalendarDialog::refreshIfVisible() {
    if (isVisible()) {
        refreshTimer_->start();
    } else {
        stale_ = true;
    }
}

void HistoryCalendarDialog::run(std::function<void()> work) {
    io_.post(std::move(work));
}

void HistoryCalendarDialog::requestMonths(MonthKey center) {
    // The shown month first, then after it, then before it.
    std::vector<MonthKey> missing;
    for (const int step : {0, 1, -1}) {
        if (std::abs(step) > kPrefetch) {
            continue;
        }
        const MonthKey key = history::shift_month(center, step);
        if (!fresh_.contains(key) && !loading_.contains(key)) {
            missing.push_back(key);
        }
    }
    if (missing.empty()) {
        return;
    }
    loading_.insert(missing.begin(), missing.end());
    run([reads = reads_, relay = relay_, generation = generation_, missing] {
        loadMonths(*reads, *relay, generation, missing);
    });
}

void HistoryCalendarDialog::onIndexLoaded(
    std::uint64_t generation, const std::shared_ptr<const std::vector<history::Mark>>& marks,
    const QString& notice) {
    if (generation != generation_) {
        return;
    }
    if (marks == nullptr) {
        // Nothing is on its way now; let navigation ask again.
        loading_.clear();
    }
    loaded_ = true;
    notice_ = qt::toUtf8(notice);
    index_ = history::CalendarIndex(marks == nullptr ? std::vector<history::Mark>() : *marks);
    syncPlaceFilter();
    render(0);
    if (pulsePending_ && isVisible()) {
        pulsePending_ = false;
        pulseTimer_->start();
    }
}

void HistoryCalendarDialog::onMonthLoaded(
    std::uint64_t generation, int year, int month,
    const std::shared_ptr<const std::vector<history::CalendarItem>>& items) {
    if (generation != generation_) {
        return;
    }
    const MonthKey key{year, month};
    loading_.erase(key);
    fresh_.insert(key);
    months_[key] = items == nullptr ? std::vector<history::CalendarItem>() : *items;
    if (key == view_) {
        renderDetails();
    }
}

// ---- filters ----

void HistoryCalendarDialog::onKindToggled(KindChip* chip) {
    history::KindSet checked;
    for (const auto& [kind, each] : kindChips_) {
        if (each->isChecked()) {
            checked.insert(kind);
        }
    }
    if (checked.empty()) {
        // One kind always stays shown; turning off the last shows it again.
        chip->blockSignals(true);
        chip->setChecked(true);
        chip->blockSignals(false);
        return;
    }
    if (checked.size() == history::KINDS.size()) {
        kinds_.reset();
    } else {
        kinds_ = std::move(checked);
    }
    render(0);
}

void HistoryCalendarDialog::syncPlaceFilter() {
    const std::vector<history::Place> places = index_.places();
    placeCombo_->blockSignals(true);
    placeCombo_->clear();
    placeCombo_->addItem(q(history::place_label(std::nullopt)));
    for (const history::Place& place : places) {
        placeCombo_->addItem(q(history::place_label(place)));
    }
    int row = 0;
    if (place_) {
        const auto found = std::find(places.begin(), places.end(), *place_);
        if (found == places.end()) {
            place_.reset();
        } else {
            row = static_cast<int>(found - places.begin()) + 1;
        }
    }
    placeCombo_->setCurrentIndex(row);
    placeCombo_->blockSignals(false);
    placeCombo_->setVisible(places.size() > 1);
}

void HistoryCalendarDialog::onPlaceChanged(int index) {
    const std::vector<history::Place> places = index_.places();
    if (index > 0 && index <= static_cast<int>(places.size())) {
        place_ = places[static_cast<std::size_t>(index - 1)];
    } else {
        place_.reset();
    }
    render(0);
}

bool HistoryCalendarDialog::visible(const history::CalendarItem& item) const {
    return (!kinds_ || kinds_->contains(item.kind)) && (!place_ || item.place == *place_);
}

// ---- navigation ----

void HistoryCalendarDialog::showMonth(MonthKey key, std::optional<Day> day) {
    const int direction = static_cast<int>(history::month_distance(view_, key));
    view_ = key;
    selected_ = day && history::month_key(*day) == key ? *day : defaultDay(key);
    render(direction);
}

void HistoryCalendarDialog::stepMonth(int step) {
    showMonth(history::shift_month(view_, step));
}

void HistoryCalendarDialog::openMonthPicker() {
    monthTitle_->setOpen(true);
    monthPicker_->popup(monthTitle_);
}

void HistoryCalendarDialog::onMonthPicked(int year, int month) {
    showMonth(MonthKey{year, month});
    grid_->setFocus();
}

void HistoryCalendarDialog::showToday() {
    const Day now = today();
    if (history::month_key(now) == view_) {
        selectDay(now);
    } else {
        showMonth(history::month_key(now), now);
    }
    grid_->pulseToday();
}

void HistoryCalendarDialog::selectDay(Day day) {
    if (history::month_key(day) != view_) {
        showMonth(history::month_key(day), day);
        return;
    }
    selected_ = day;
    grid_->setSelected(day, true);
    renderAgenda();
}

Day HistoryCalendarDialog::defaultDay(MonthKey key) const {
    const std::map<Day, history::DaySummary> days =
        index_.days(key.year, key.month, kinds_, place_);
    const Day now = today();
    if (history::month_key(now) == key) {
        if (days.contains(now) || days.empty()) {
            return now;
        }
        std::optional<Day> latest;
        for (const auto& [day, summary] : days) {
            if (day <= now) {
                latest = day;
            }
        }
        return latest ? *latest : days.begin()->first;
    }
    return days.empty() ? firstOf(key) : days.rbegin()->first;
}

void HistoryCalendarDialog::showBusiest() {
    const history::MonthStats stats = index_.month_stats(view_.year, view_.month, kinds_, place_);
    if (stats.busiest) {
        selectDay(stats.busiest->first);
    }
}

void HistoryCalendarDialog::activateDay(Day day) {
    if (history::month_key(day) != view_) {
        return;
    }
    const std::vector<history::CalendarItem> items = dayItems(day);
    if (!items.empty()) {
        openItem(items.front());
    }
}

// ---- rendering ----

std::map<Day, std::vector<history::CalendarItem>> HistoryCalendarDialog::itemsByDay() const {
    std::map<Day, std::vector<history::CalendarItem>> result;
    const auto found = months_.find(view_);
    if (found == months_.end()) {
        return result;
    }
    for (const history::CalendarItem& item : found->second) {
        if (visible(item)) {
            result[item.day()].push_back(item);
        }
    }
    return result;
}

std::vector<history::CalendarItem> HistoryCalendarDialog::dayItems(Day day) const {
    std::map<Day, std::vector<history::CalendarItem>> byDay = itemsByDay();
    const auto found = byDay.find(day);
    return found == byDay.end() ? std::vector<history::CalendarItem>() : std::move(found->second);
}

void HistoryCalendarDialog::render(int direction) {
    monthTitle_->setText(q(titleOf(view_)));
    if (history::month_key(selected_) != view_) {
        selected_ = defaultDay(view_);
    }
    MonthDetails details;
    details.days = index_.days(view_.year, view_.month, kinds_, place_);
    details.items = itemsByDay();
    details.milestones = index_.milestone_days(view_.year, view_.month);
    grid_->setMonth(view_.year, view_.month, std::move(details), today(), firstWeekday_, direction);
    grid_->setSelected(selected_, direction == 0);
    renderHeader();
    renderAgenda(true);
    if (loaded_) {
        requestMonths(view_);
    }
}

void HistoryCalendarDialog::renderDetails() {
    MonthDetails details;
    details.days = index_.days(view_.year, view_.month, kinds_, place_);
    details.items = itemsByDay();
    details.milestones = index_.milestone_days(view_.year, view_.month);
    grid_->setDetails(std::move(details));
    renderAgenda();
}

void HistoryCalendarDialog::renderHeader() {
    const history::MonthStats stats = index_.month_stats(view_.year, view_.month, kinds_, place_);
    const history::MonthStats all =
        index_.month_stats(view_.year, view_.month, std::nullopt, place_);
    for (const auto& [kind, chip] : kindChips_) {
        const auto found = all.by_kind.find(kind);
        chip->setCount(
            loaded_ ? std::optional<std::int64_t>(found == all.by_kind.end() ? 0 : found->second)
                    : std::nullopt);
    }

    if (!loaded_) {
        statsLabel_->setText(QStringLiteral(u"Loading…"));
    } else if (stats.count != 0) {
        std::string text = std::format("<b>{}</b> {}", cv::grouped(stats.count),
                                       stats.count == 1 ? "recording" : "recordings");
        if (stats.seconds >= 1) {
            text +=
                std::format(" \xC2\xB7 <b>{}</b> of audio", format_audio_duration(stats.seconds));
        }
        text += std::format(" \xC2\xB7 <b>{}</b> {}", cv::grouped(stats.active_days),
                            stats.active_days == 1 ? "day" : "days");
        statsLabel_->setText(q(text));
    } else {
        statsLabel_->setText(q(std::format("Nothing recorded in {}", cv::month_name(view_.month))));
    }

    const bool showBusiestButton = stats.busiest.has_value() && stats.active_days > 1;
    if (showBusiestButton) {
        const auto& [day, count] = *stats.busiest;
        busiestButton_->setText(
            q(std::format("Busiest: {}, {} {} \xC2\xB7 {}", cv::day_abbr(cv::weekday(day)),
                          cv::month_abbr(monthOf(day)), dayNumber(day), cv::grouped(count))));
    }
    busiestButton_->setVisible(showBusiestButton);

    const Day now = today();
    const std::int64_t streak = history::month_key(now) == view_ ? index_.streak(now) : 0;
    if (streak >= 2) {
        streakLabel_->setText(q(
            std::format("{}-day streak{}", streak, streak >= 7 ? " \xC2\xB7 keep it going" : "")));
    }
    streakLabel_->setVisible(streak >= 2);

    const std::map<MonthKey, std::int64_t> counts = index_.month_counts(kinds_, place_);
    yearStrip_->setData(stripMonths(), counts, view_, history::month_key(now));
    monthPicker_->setData(counts, view_, history::month_key(now));

    std::string notice = notice_;
    if (!notice.empty() && notice.find("reachable") != std::string::npos) {
        notice += " Records kept there show when it is.";
    }
    noticeLabel_->setText(q(notice));
    noticeLabel_->setVisible(!notice.empty());
}

std::vector<MonthKey> HistoryCalendarDialog::stripMonths() const {
    // Twelve months: the year up to now, or centred on the month shown when older.
    const MonthKey current = history::month_key(today());
    MonthKey end;
    if (history::month_distance(view_, current) <= 11 &&
        history::month_distance(current, view_) <= 0) {
        end = current;
    } else {
        end = history::shift_month(view_, 6);
    }
    std::vector<MonthKey> months;
    for (int step = -11; step <= 0; ++step) {
        months.push_back(history::shift_month(end, step));
    }
    return months;
}

void HistoryCalendarDialog::clearAgenda() {
    while (agendaLayout_->count() > 0) {
        QLayoutItem* item = agendaLayout_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            // Hidden now: deleteLater waits for the event loop, and until then the old cards
            // would still show under the new ones.
            widget->hide();
            if (acting_ > 0) {
                retired_.append(widget);
            } else {
                widget->deleteLater();
            }
        }
        delete item;
    }
    cards_.clear();
}

void HistoryCalendarDialog::actOnCard(const AgendaCard& card, bool copy) {
    const history::CalendarItem item = card.item();
    ++acting_;
    if (copy) {
        copyItem(item);
    } else {
        openItem(item);
    }
    if (--acting_ == 0) {
        // Posted from inside the card's own handler, these go once it has returned.
        for (const QPointer<QWidget>& widget : std::exchange(retired_, {})) {
            if (widget != nullptr) {
                widget->deleteLater();
            }
        }
    }
}

void HistoryCalendarDialog::renderAgenda(bool animate) {
    const Day day = selected_;
    const Day now = today();
    agendaWeekday_->setText(q(cv::day_name(cv::weekday(day))));
    todayBadge_->setVisible(day == now);
    agendaDate_->setText(q(std::format(
        "{} {}{}", cv::month_name(monthOf(day)), dayNumber(day),
        yearOf(day) == yearOf(now) ? std::string() : std::format(", {}", yearOf(day)))));
    const std::vector<history::CalendarItem> items = dayItems(day);
    const std::map<Day, history::DaySummary> days =
        index_.days(view_.year, view_.month, kinds_, place_);
    const auto found = days.find(day);
    const history::DaySummary* summary = found == days.end() ? nullptr : &found->second;
    if (summary != nullptr && summary->count != 0) {
        std::string text = cv::plural(summary->count, "recording");
        if (summary->seconds >= 1) {
            text += " \xC2\xB7 " + format_audio_duration(summary->seconds) + " of audio";
        }
        if (summary->by_kind.size() > 1) {
            text += "\n" + cv::kind_breakdown(summary->by_kind);
        }
        agendaSummary_->setText(q(text));
    } else {
        agendaSummary_->setText(loaded_ ? QStringLiteral("Nothing recorded") : QString());
    }

    const bool sameDay = agendaDay_ && *agendaDay_ == day;
    const bool hadCards = !cards_.isEmpty();
    agendaDay_ = day;
    const int scroll = sameDay ? agendaScroll_->verticalScrollBar()->value() : 0;
    clearAgenda();
    if (!items.empty()) {
        for (const history::CalendarItem& item : items) {
            auto* card = new AgendaCard(item, index_.milestone(item.record_id));
            connect(card, &AgendaCard::activated, this, [this, card] { actOnCard(*card, false); });
            connect(card, &AgendaCard::copyRequested, this,
                    [this, card] { actOnCard(*card, true); });
            if (fetching_.contains(item.record_id)) {
                card->setProgress(
                    q(std::format("Getting it from {}\xE2\x80\xA6", item.place.name)));
            }
            agendaLayout_->addWidget(card);
            cards_.append(card);
        }
        if (animate && !(sameDay && hadCards)) {
            QList<QWidget*> widgets;
            for (AgendaCard* card : cards_) {
                widgets.append(card);
            }
            fadeIn(widgets);
        }
        scrollTo_ = scroll;
        scrollTimer_->start();
        return;
    }
    if (summary != nullptr && summary->count != 0) {
        agendaLayout_->addWidget(emptyState(QStringLiteral(u"Loading this day…"), QString()));
        return;
    }
    agendaLayout_->addWidget(emptyStateFor(day));
}

QWidget* HistoryCalendarDialog::emptyStateFor(Day) {
    if (!loaded_) {
        return emptyState(QStringLiteral(u"Loading…"), QString());
    }
    if (index_.size() == 0) {
        return emptyState(QStringLiteral("Nothing recorded yet"),
                          QStringLiteral("Dictations, files you transcribe and meetings appear "
                                         "here on the day they happen, tinted brighter on busier "
                                         "days."));
    }
    const history::MonthStats stats = index_.month_stats(view_.year, view_.month, kinds_, place_);
    if (stats.count != 0) {
        return emptyState(QStringLiteral("A quiet day"),
                          QStringLiteral("Nothing was recorded on this day. Days with recordings "
                                         "are tinted; the brighter the tint, the more was "
                                         "recorded."));
    }
    const std::optional<MonthKey> nearest = index_.nearest_month(view_, kinds_, place_);
    const bool elsewhere = nearest && *nearest != view_;
    QString body;
    if (elsewhere) {
        body = q(std::format("The closest month with recordings is {}.", titleOf(*nearest)));
    } else if (filtered()) {
        body = QStringLiteral("Nothing recorded yet matches these filters.");
    }
    QWidget* widget =
        emptyState(q(std::format("Nothing recorded in {}", cv::month_name(view_.month))), body);
    if (elsewhere) {
        const std::int64_t distance = history::month_distance(view_, *nearest);
        auto* jump =
            new QPushButton(q(std::format("{}Go to {}{}", distance < 0 ? "\xE2\x86\x90 " : "",
                                          titleOf(*nearest), distance > 0 ? " \xE2\x86\x92" : "")));
        jump->setObjectName(QStringLiteral("calendarJumpBtn"));
        jump->setFixedHeight(30);
        jump->setCursor(Qt::PointingHandCursor);
        const MonthKey target = *nearest;
        connect(jump, &QPushButton::clicked, this, [this, target] { showMonth(target); });
        static_cast<QVBoxLayout*>(widget->layout())->addWidget(jump, 0, Qt::AlignLeft);
    }
    return widget;
}

bool HistoryCalendarDialog::filtered() const {
    return kinds_.has_value() || place_.has_value();
}

void HistoryCalendarDialog::finishAnimations() {
    grid_->finishAnimations();
    yearStrip_->finishAnimations();
    monthPicker_->finishAnimations();
    for (AgendaCard* card : cards_) {
        finishFadeIn(card);
    }
}

// ---- opening and copying ----

void HistoryCalendarDialog::openItem(const history::CalendarItem& item) {
    if (item.kind != history::MEETING_KIND) {
        if (const auto* entry = std::get_if<HistoryEntry>(&item.source)) {
            Q_EMIT entryRequested(*entry);
        }
        return;
    }
    if (item.place.scope == history::ON) {
        fetchMeeting(item);
        return;
    }
    Q_EMIT meetingRequested(q(item.record_id));
}

void HistoryCalendarDialog::copyItem(const history::CalendarItem& item) {
    if (item.kind == history::MEETING_KIND) {
        Q_EMIT meetingCopyRequested(q(item.record_id));
        return;
    }
    const auto* entry = std::get_if<HistoryEntry>(&item.source);
    if (!copyText(entry != nullptr ? q(entry->text) : QString())) {
        kLog.error("Couldn't copy a calendar entry: the clipboard is not available");
        return;
    }
    Q_EMIT entryCopied(q(item.record_id));
}

void HistoryCalendarDialog::fetchMeeting(const history::CalendarItem& item) {
    // Downloads a host-kept meeting here, then opens it, as Past Meetings does.
    const std::string meetingId = item.record_id;
    const std::string host = item.place.name;
    if (fetching_.contains(meetingId)) {
        return;
    }
    const auto* row = std::get_if<Json>(&item.source);
    const auto fingerprint = row ? row->find("_stored_on_fingerprint") : Json::const_iterator{};
    const auto pin = row && fingerprint != row->end() && fingerprint->is_string()
                         ? fingerprint->get<std::string>()
                         : std::string();
    const auto checkOut = pairedCheckOut_ ? pairedCheckOut_ : checkOut_;
    if (!checkOut || (pairedCheckOut_ && pin.empty())) {
        kLog.warning("Couldn't get a meeting from the host: no record sync is set up");
        return;
    }
    fetching_.insert(meetingId);
    const std::uint64_t token = token_;
    onFetchProgress(token, q(meetingId), q(std::format("Getting it from {}\xE2\x80\xA6", host)));
    // Bound here: the worker must not reach back into the dialog.
    run([checkOut, relay = relay_, token, meetingId, host,
         target = pairedCheckOut_ ? pin : std::string("meeting")] {
        const QString id = qt::toQString(meetingId);
        try {
            checkOut(target, meetingId,
                     [relay, token, id, host](std::int64_t got, std::int64_t total) {
                         if (total != 0) {
                             Q_EMIT relay->fetchProgress(
                                 token, id,
                                 qt::toQString(std::format("Getting it from {}\xE2\x80\xA6 {}%",
                                                           host, got * 100 / total)));
                         }
                     });
        } catch (const std::exception& error) {
            kLog.warning("Couldn't get a meeting from the host: {}", error.what());
            const std::string text = error.what();
            Q_EMIT relay->fetchDone(token, id, qt::toQString(text.empty() ? "exception" : text));
            return;
        }
        Q_EMIT relay->fetchDone(token, id, QString());
    });
}

void HistoryCalendarDialog::onFetchProgress(std::uint64_t token, const QString& meetingId,
                                            const QString& text) {
    if (token != token_) {
        return;
    }
    if (AgendaCard* found = card(qt::toUtf8(meetingId))) {
        found->setProgress(text);
    }
}

void HistoryCalendarDialog::onFetchDone(std::uint64_t token, const QString& meetingId,
                                        const QString& error) {
    if (token != token_) {
        return;
    }
    fetching_.erase(qt::toUtf8(meetingId));
    if (!error.isEmpty()) {
        if (AgendaCard* found = card(qt::toUtf8(meetingId))) {
            found->setProgress(QStringLiteral("Couldn't open it: %1").arg(error));
        }
        return;
    }
    refresh();
    Q_EMIT meetingRequested(meetingId);
}

} // namespace fv
