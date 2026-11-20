#include "ui_settings/backup_calendar.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string>

#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStringList>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "ui_theme/palette.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

using history::Day;
using history::MonthKey;

QString q(std::string_view utf8) {
    return qt::toQString(utf8);
}

int monthOf(Day day) {
    return static_cast<int>(static_cast<unsigned>(day.month()));
}

int dayOf(Day day) {
    return static_cast<int>(static_cast<unsigned>(day.day()));
}

std::chrono::sys_days sysDays(Day day) {
    return std::chrono::sys_days{day};
}

// int(255 * fraction): the alpha of a palette colour.
int alpha(double fraction) {
    return static_cast<int>(255 * fraction);
}

// "Oct 4": strftime("%b") and the day.
std::string shortDate(Day day) {
    return std::format("{} {}", calendar_view::month_abbr(monthOf(day)), dayOf(day));
}

} // namespace

namespace backup_calendar {

bool is_planned(Day day, std::optional<Day> next_due, int interval_days) {
    if (!next_due || interval_days <= 0) {
        return false;
    }
    const std::int64_t elapsed = (sysDays(day) - sysDays(*next_due)).count();
    return elapsed >= 0 && elapsed % interval_days == 0;
}

std::map<MonthKey, std::int64_t>
month_counts(std::optional<Day> latest, std::optional<Day> next_due, int interval_days, Day today) {
    std::map<MonthKey, std::int64_t> counts;
    if (latest) {
        counts[history::month_key(*latest)] = 1;
    }
    if (next_due && interval_days > 0) {
        const auto end = sysDays(today) + std::chrono::days{kPickerHorizonDays};
        for (auto day = sysDays(*next_due); day <= end; day += std::chrono::days{interval_days}) {
            ++counts[history::month_key(Day{day})];
        }
    }
    return counts;
}

} // namespace backup_calendar

BackupMonthGrid::BackupMonthGrid(const Clock& clock, QWidget* parent) : MonthGrid(clock, parent) {
    setObjectName(QStringLiteral("backupCalendarGrid"));
    setAccessibleName(QStringLiteral("Backup calendar"));
    // The History calendar fills a window; this one shares a settings card.
    setMinimumSize(280, 250);
}

void BackupMonthGrid::setDates(std::optional<Day> latest, std::optional<Day> nextDue,
                               int intervalDays) {
    latest_ = latest;
    nextDue_ = nextDue;
    intervalDays_ = intervalDays;
    update();
}

bool BackupMonthGrid::isPlanned(Day day) const {
    return backup_calendar::is_planned(day, nextDue_, intervalDays_);
}

std::vector<std::pair<std::string_view, std::string_view>> BackupMonthGrid::events(Day day) const {
    std::vector<std::pair<std::string_view, std::string_view>> found;
    if (latest_ && day == *latest_) {
        found.emplace_back("Saved", backup_calendar::kLatestText);
    }
    if (isPlanned(day)) {
        found.emplace_back("Planned", backup_calendar::kPlannedText);
    }
    return found;
}

void BackupMonthGrid::paintCell(QPainter& painter, const QRectF& rect, const QDate& date) {
    MonthGrid::paintCell(painter, rect, date);
    const Day day = toDay(date);
    if (events(day).empty()) {
        return;
    }
    painter.save();
    if (monthOf(day) != month().month) {
        painter.setOpacity(0.5);
    }
    const bool latest = latest_ && day == *latest_;
    const bool planned = isPlanned(day);
    QPainterPath outline;
    outline.addRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    if (latest) {
        painter.fillPath(outline, ui::tokenColor("success-rgb", alpha(0.16)));
        painter.setPen(QPen(ui::tokenColor("success-rgb", alpha(0.55)), 1));
    } else {
        QPen pen(ui::tokenColor("accent-rgb", alpha(0.7)), 1.2);
        pen.setDashPattern({3.0, 3.0});
        painter.setPen(pen);
    }
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(outline);
    paintLabels(painter, rect, latest, planned);
    painter.restore();
}

void BackupMonthGrid::paintLabels(QPainter& painter, const QRectF& rect, bool latest,
                                  bool planned) {
    struct Entry {
        QString text;
        const char* fill;
        const char* ink;
    };
    std::vector<Entry> entries;
    if (latest) {
        entries.push_back({QStringLiteral("Saved"), "success", "success-text"});
    }
    if (planned) {
        entries.push_back({QStringLiteral("Planned"), "accent", "accent-soft"});
    }
    const double top = numberRect(rect).bottom() + 3;
    const double line = calendarPx(16);
    const double left = rect.left() + 9;
    const double radius = std::max(2.5, static_cast<double>(calendarPx(3)));
    const double textLeft = left + calendarPx(10);
    const double width = rect.right() - 7 - textLeft;
    const int room = static_cast<int>(std::floor((rect.bottom() - 7 - top) / line));
    painter.setPen(Qt::NoPen);
    if (width < calendarPx(40) || room < static_cast<int>(entries.size())) {
        // Too small for words: a dot per event along the bottom.
        for (std::size_t index = 0; index < entries.size(); ++index) {
            painter.setBrush(ui::tokenColor(entries[index].fill));
            painter.drawEllipse(
                QPointF(left + radius + static_cast<double>(index) * (radius * 2 + 3),
                        rect.bottom() - 9 - radius),
                radius, radius);
        }
        return;
    }
    const QFont font = calendarFont(this->font(), 11, QFont::DemiBold);
    const QFontMetrics metrics(font);
    painter.setFont(font);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const double y = top + static_cast<double>(index) * line;
        painter.setPen(Qt::NoPen);
        painter.setBrush(ui::tokenColor(entries[index].fill));
        painter.drawEllipse(QPointF(left + radius, y + line / 2), radius, radius);
        painter.setPen(ui::tokenColor(entries[index].ink));
        painter.drawText(
            QRectF(textLeft, y, width, line), static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter),
            metrics.elidedText(entries[index].text, Qt::ElideRight, static_cast<int>(width)));
    }
}

bool BackupMonthGrid::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const std::optional<Day> day = dayAt(QPointF(help->pos()));
        if (!day) {
            QToolTip::hideText();
        } else {
            QStringList lines;
            lines << q(std::format("{}, {} {}",
                                   calendar_view::day_name(calendar_view::weekday(*day)),
                                   calendar_view::month_name(monthOf(*day)), dayOf(*day)));
            const auto found = events(*day);
            if (found.empty()) {
                lines << QStringLiteral("No backup");
            }
            for (const auto& entry : found) {
                lines << q(entry.second);
            }
            QToolTip::showText(help->globalPos(), lines.join(QLatin1Char('\n')), this);
        }
        return true;
    }
    return MonthGrid::event(event);
}

BackupCalendar::BackupCalendar(const Clock& clock, QWidget* parent)
    : QWidget(parent), clock_(clock), view_(history::month_key(localToday(clock))),
      selected_(localToday(clock)) {
    setObjectName(QStringLiteral("backupCalendar"));

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(8);
    auto* navigation = new QHBoxLayout();
    navigation->setSpacing(6);
    title_ = new MonthTitleButton(QString(), this);
    title_->setFixedHeight(34);
    title_->setToolTip(QStringLiteral("Choose a month or year (M)"));
    connect(title_, &QPushButton::clicked, this, &BackupCalendar::openMonthPicker);
    navigation->addWidget(title_);
    navigation->addStretch(1);
    today_ = navButton(QStringLiteral("Today"), QStringLiteral("Go to today (T)"),
                       "backupCalendarToday");
    previous_ =
        navButton(QStringLiteral(u"‹"), QStringLiteral("Previous month"), "backupCalendarNav");
    next_ = navButton(QStringLiteral(u"›"), QStringLiteral("Next month"), "backupCalendarNav");
    for (QToolButton* button : {today_, previous_, next_}) {
        navigation->addWidget(button);
    }
    column->addLayout(navigation);

    grid_ = new BackupMonthGrid(clock, this);
    column->addWidget(grid_, 1);
    picker_ = new MonthPicker(clock, this);

    auto* legend = new QHBoxLayout();
    for (const auto& [text, name] :
         {std::pair{QStringLiteral(u"● Latest backup"), "backupCalendarLatest"},
          std::pair{QStringLiteral(u"┄ Planned"), "backupCalendarPlanned"}}) {
        auto* label = new QLabel(text);
        label->setObjectName(QString::fromLatin1(name));
        legend->addWidget(label);
    }
    legend->addStretch();
    column->addLayout(legend);
    detail_ = new WrappedLabel(QString());
    detail_->setObjectName(QStringLiteral("backupCalendarDetail"));
    detail_->setAccessibleName(QStringLiteral("Selected backup date"));
    column->addWidget(detail_);

    connect(previous_, &QToolButton::clicked, this, [this] { stepMonth(-1); });
    connect(next_, &QToolButton::clicked, this, [this] { stepMonth(1); });
    connect(today_, &QToolButton::clicked, this, &BackupCalendar::goToday);
    connect(picker_, &MonthPicker::monthChosen, this, [this](int year, int month) {
        showMonth(MonthKey{year, month});
        grid_->setFocus();
    });
    connect(picker_, &MonthPicker::closed, this, [this] { title_->setOpen(false); });
    connect(grid_, &MonthGrid::daySelected, this,
            [this](const QDate& day) { selectDay(toDay(day)); });
    connect(grid_, &MonthGrid::monthStep, this, &BackupCalendar::stepMonth);
    connect(grid_, &MonthGrid::todayRequested, this, &BackupCalendar::goToday);
    connect(grid_, &MonthGrid::pickerRequested, this, &BackupCalendar::openMonthPicker);
    render(0);
}

QToolButton* BackupCalendar::navButton(const QString& text, const QString& tip, const char* name) {
    auto* button = new QToolButton(this);
    button->setObjectName(QString::fromLatin1(name));
    button->setText(text);
    button->setAccessibleName(tip.left(tip.indexOf(QStringLiteral(" ("))));
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

void BackupCalendar::setDates(std::optional<Day> latest, std::optional<Day> nextDue,
                              int intervalDays) {
    latest_ = latest;
    nextDue_ = nextDue;
    intervalDays_ = intervalDays;
    grid_->setDates(latest, nextDue, intervalDays);
    refreshPicker();
    updateDetail();
}

void BackupCalendar::showMonth(MonthKey key, std::optional<Day> selected) {
    const std::int64_t direction = history::month_distance(view_, key);
    view_ = key;
    if (selected && history::month_key(*selected) == key) {
        selected_ = *selected;
    } else if (history::month_key(selected_) != key) {
        const Day today = localToday(clock_);
        selected_ = history::month_key(today) == key
                        ? today
                        : Day{std::chrono::year{key.year} /
                              std::chrono::month{static_cast<unsigned>(key.month)} / 1};
    }
    render((direction > 0) - (direction < 0));
}

void BackupCalendar::stepMonth(int step) {
    showMonth(history::shift_month(view_, step));
}

void BackupCalendar::selectDay(Day day) {
    if (history::month_key(day) != view_) {
        showMonth(history::month_key(day), day);
        return;
    }
    selected_ = day;
    grid_->setSelected(day, true);
    updateDetail();
}

void BackupCalendar::goToday() {
    const Day today = localToday(clock_);
    if (history::month_key(today) == view_) {
        selectDay(today);
    } else {
        showMonth(history::month_key(today), today);
    }
    grid_->pulseToday();
}

void BackupCalendar::openMonthPicker() {
    refreshPicker();
    title_->setOpen(true);
    picker_->popup(title_);
}

void BackupCalendar::render(int direction) {
    grid_->setMonth(view_.year, view_.month, MonthDetails{}, localToday(clock_), history::MONDAY,
                    direction);
    grid_->setSelected(selected_, false);
    title_->setTitle(q(std::format("{} {}", calendar_view::month_name(view_.month), view_.year)),
                     q(std::format("{} {}", calendar_view::month_abbr(view_.month), view_.year)));
    refreshPicker();
    updateDetail();
}

void BackupCalendar::refreshPicker() {
    const Day today = localToday(clock_);
    picker_->setData(backup_calendar::month_counts(latest_, nextDue_, intervalDays_, today), view_,
                     history::month_key(today));
}

void BackupCalendar::updateDetail() {
    std::string text;
    for (const auto& entry : grid_->events(selected_)) {
        text += text.empty() ? "" : " \xC2\xB7 ";
        text += entry.second;
    }
    if (text.empty()) {
        text = "No backup shown for this date";
    }
    detail_->setText(q(std::format("{} \xE2\x80\x94 {}", shortDate(selected_), text)));
}

} // namespace fv
