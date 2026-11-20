#include "ui_history/history_calendar.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <set>
#include <utility>

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSequentialAnimationGroup>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include "appkit/qt_bridge.h"
#include "base/clock.h"
#include "history/format_utils.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

using history::Day;
using history::MonthKey;

QString q(std::string_view utf8) {
    return qt::toQString(utf8);
}

// The menus of the agenda cards.
constexpr char kMenuStyle[] = R"(
    QMenu {
        background-color: rgba(@surface-rgb, 0.95);
        color: @text;
        border: 1px solid rgba(@overlay-rgb, 0.1);
        border-radius: 10px;
        padding: 6px;
    }
    QMenu::item {
        padding: 8px 28px 8px 14px;
        border-radius: 6px;
        font-size: 13px;
    }
    QMenu::item:selected {
        background-color: @accent;
        color: @on-accent;
    }
)";

int monthOf(Day day) {
    return static_cast<int>(static_cast<unsigned>(day.month()));
}

int yearOf(Day day) {
    return static_cast<int>(day.year());
}

int dayNumber(Day day) {
    return static_cast<int>(static_cast<unsigned>(day.day()));
}

Day addDays(Day day, int count) {
    return Day{std::chrono::sys_days(day) + std::chrono::days{count}};
}

// Python's int() of a float: toward zero.
int truncate(double value) {
    return static_cast<int>(value);
}

void setOpacity(QColor& color, double alpha) {
    color.setAlphaF(static_cast<float>(alpha));
}

} // namespace

int calendarPx(double size) {
    return std::max(1, static_cast<int>(py::round_half_even(size * ui::currentUiFontScale())));
}

QFont calendarFont(const QFont& base, double size, QFont::Weight weight) {
    QFont font(base);
    font.setPixelSize(calendarPx(size));
    font.setWeight(weight);
    return font;
}

QDate toQDate(Day day) {
    return QDate(yearOf(day), monthOf(day), dayNumber(day));
}

Day toDay(const QDate& date) {
    return Day{std::chrono::year{date.year()},
               std::chrono::month{static_cast<unsigned>(date.month())},
               std::chrono::day{static_cast<unsigned>(date.day())}};
}

Day localToday(const Clock& clock) {
    const std::chrono::zoned_time local(clock.zone(), clock.utc());
    return Day{std::chrono::floor<std::chrono::days>(local.get_local_time())};
}

namespace calendar_view {

namespace {

constexpr std::array<std::string_view, 12> kMonthNames = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
constexpr std::array<std::string_view, 12> kMonthAbbr = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
constexpr std::array<std::string_view, 7> kDayNames = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
constexpr std::array<std::string_view, 7> kDayAbbr = {"Mon", "Tue", "Wed", "Thu",
                                                      "Fri", "Sat", "Sun"};

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return result;
}

} // namespace

std::string_view kind_color(std::string_view kind) {
    if (kind == history::DICTATION_KIND) {
        return "accent";
    }
    if (kind == history::FILE_KIND) {
        return "purple";
    }
    if (kind == history::MEETING_KIND) {
        return "warning";
    }
    return "";
}

std::string_view kind_text(std::string_view kind) {
    if (kind == history::DICTATION_KIND) {
        return "accent-soft";
    }
    if (kind == history::FILE_KIND) {
        return "purple-text";
    }
    if (kind == history::MEETING_KIND) {
        return "warning-text";
    }
    return "";
}

std::string_view month_name(int month) {
    return kMonthNames.at(static_cast<std::size_t>(month - 1));
}

std::string_view month_abbr(int month) {
    return kMonthAbbr.at(static_cast<std::size_t>(month - 1));
}

std::string_view day_name(int weekday) {
    return kDayNames.at(static_cast<std::size_t>(weekday));
}

std::string_view day_abbr(int weekday) {
    return kDayAbbr.at(static_cast<std::size_t>(weekday));
}

int weekday(Day day) {
    // iso_encoding: Monday 1 .. Sunday 7.
    return static_cast<int>(std::chrono::weekday(std::chrono::sys_days(day)).iso_encoding()) - 1;
}

std::string grouped(std::int64_t number) {
    const bool negative = number < 0;
    std::string digits = std::to_string(negative ? -number : number);
    std::string result;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        // A comma before each group of three counted from the right.
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            result.push_back(',');
        }
        result.push_back(digits[i]);
    }
    return negative ? "-" + result : result;
}

std::string clock(const py::DateTime& when) {
    const int hour12 = when.hour % 12 == 0 ? 12 : when.hour % 12;
    return std::format("{}:{:02} {}", hour12, when.minute, when.hour < 12 ? "AM" : "PM");
}

std::string plural(std::int64_t count, std::string_view one, std::optional<std::string_view> many) {
    std::string word;
    if (count == 1) {
        word = one;
    } else if (many && !many->empty()) {
        word = *many;
    } else {
        word = std::string(one) + "s";
    }
    return grouped(count) + " " + word;
}

std::string kind_breakdown(const std::map<std::string, std::int64_t, std::less<>>& by_kind) {
    std::string result;
    for (const std::string_view kind : history::KINDS) {
        const auto found = by_kind.find(kind);
        const std::int64_t count = found == by_kind.end() ? 0 : found->second;
        if (count == 0) {
            continue;
        }
        if (!result.empty()) {
            result += " \xC2\xB7 ";
        }
        result += plural(count, lower(history::kind_label(kind)), history::kind_plural(kind));
    }
    return result;
}

std::vector<std::size_t> pick_line_indexes(const std::vector<std::string_view>& kinds,
                                           std::int64_t capacity) {
    std::vector<std::size_t> all;
    if (static_cast<std::int64_t>(kinds.size()) <= capacity) {
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            all.push_back(index);
        }
        return all;
    }
    std::set<std::size_t> picked;
    std::set<std::string_view> seen;
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        if (!seen.contains(kinds[index]) && static_cast<std::int64_t>(picked.size()) < capacity) {
            picked.insert(index);
            seen.insert(kinds[index]);
        }
    }
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        if (static_cast<std::int64_t>(picked.size()) >= capacity) {
            break;
        }
        picked.insert(index);
    }
    return {picked.begin(), picked.end()};
}

std::vector<const history::CalendarItem*>
pick_lines(const std::vector<history::CalendarItem>& items, std::int64_t capacity) {
    std::vector<std::string_view> kinds;
    kinds.reserve(items.size());
    for (const history::CalendarItem& item : items) {
        kinds.push_back(item.kind);
    }
    std::vector<const history::CalendarItem*> shown;
    for (const std::size_t index : pick_line_indexes(kinds, capacity)) {
        shown.push_back(&items[index]);
    }
    return shown;
}

std::string day_tooltip(Day day, const history::DaySummary* summary,
                        std::optional<std::int64_t> milestone) {
    const std::string heading =
        std::format("{}, {} {}", day_name(weekday(day)), month_name(monthOf(day)), dayNumber(day));
    if (summary == nullptr || summary->count == 0) {
        return heading + "\nNothing recorded";
    }
    std::string text = heading + "\n" + plural(summary->count, "recording");
    if (summary->seconds >= 1) {
        text += " \xC2\xB7 " + format_audio_duration(summary->seconds) + " of audio";
    }
    const std::string breakdown = kind_breakdown(summary->by_kind);
    if (!breakdown.empty() && summary->by_kind.size() > 1) {
        text += "\n" + breakdown;
    }
    if (milestone && *milestone != 0) {
        text += "\n\xE2\x9C\xA6 " + history::milestone_text(*milestone);
    }
    return text;
}

QColor heat(std::int64_t count, std::int64_t peak) {
    const double level = std::pow(
        static_cast<double>(count) / static_cast<double>(std::max<std::int64_t>(1, peak)), 1.4);
    const bool dark = ui::currentPalette().isDark();
    const double base = dark ? 0.07 : 0.05;
    const double span = dark ? 0.24 : 0.16;
    return ui::tokenColor("accent-rgb", truncate(255 * (base + span * level)));
}

QPainterPath rounded(const QRectF& rect, double radius) {
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

QColor mix(const QColor& a, const QColor& b, double amount) {
    amount = std::clamp(amount, 0.0, 1.0);
    float ar = 0, ag = 0, ab = 0, aa = 0, br = 0, bg = 0, bb = 0, ba = 0;
    a.getRgbF(&ar, &ag, &ab, &aa);
    b.getRgbF(&br, &bg, &bb, &ba);
    const auto lerp = [amount](float x, float y) {
        return static_cast<float>(x + (y - x) * amount);
    };
    return QColor::fromRgbF(lerp(ar, br), lerp(ag, bg), lerp(ab, bb), lerp(aa, ba));
}

void paint_chevron(QPainter& painter, const QPointF& center, double size, Pointing pointing,
                   const QColor& color, double width) {
    const double x = center.x();
    const double y = center.y();
    const double s = size;
    const double h = size / 2;
    std::array<QPointF, 3> points;
    switch (pointing) {
    case Pointing::Left:
        points = {QPointF(x + h, y - s), QPointF(x - h, y), QPointF(x + h, y + s)};
        break;
    case Pointing::Right:
        points = {QPointF(x - h, y - s), QPointF(x + h, y), QPointF(x - h, y + s)};
        break;
    case Pointing::Up:
        points = {QPointF(x - s, y + h), QPointF(x, y - h), QPointF(x + s, y + h)};
        break;
    case Pointing::Down:
        points = {QPointF(x - s, y - h), QPointF(x, y + h), QPointF(x + s, y - h)};
        break;
    }
    QPainterPath path(points[0]);
    path.lineTo(points[1]);
    path.lineTo(points[2]);
    QPen pen(color, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.restore();
}

} // namespace calendar_view

namespace cv = calendar_view;

// ---- MonthGrid ----

MonthGrid::MonthGrid(QWidget* parent) : MonthGrid(system_clock(), parent) {}

MonthGrid::MonthGrid(const Clock& clock, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("calendarMonthGrid"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(460, 360);
    setAccessibleName(QStringLiteral("Month calendar"));
    today_ = localToday(clock);
    year_ = yearOf(today_);
    month_ = monthOf(today_);
    weeks_ = history::month_weeks(year_, month_, firstWeekday_);

    selectionAnim_ = new QVariantAnimation(this);
    selectionAnim_->setDuration(190);
    selectionAnim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(selectionAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        selectionRect_ = value.toRectF();
        update();
    });

    hoverAnim_ = new QVariantAnimation(this);
    hoverAnim_->setDuration(130);
    hoverAnim_->setStartValue(0.0);
    hoverAnim_->setEndValue(1.0);
    connect(hoverAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        hoverLevel_ = value.toDouble();
        update();
    });

    slideAnim_ = new QVariantAnimation(this);
    slideAnim_->setDuration(280);
    slideAnim_->setEasingCurve(QEasingCurve::OutCubic);
    slideAnim_->setStartValue(0.0);
    slideAnim_->setEndValue(1.0);
    connect(slideAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        slideProgress_ = value.toDouble();
        update();
    });
    connect(slideAnim_, &QVariantAnimation::finished, this, [this] {
        slideFrom_.reset();
        slideProgress_ = 1.0;
        update();
    });

    pulseAnim_ = new QVariantAnimation(this);
    pulseAnim_->setDuration(1100);
    pulseAnim_->setEasingCurve(QEasingCurve::OutCubic);
    pulseAnim_->setStartValue(0.0);
    pulseAnim_->setEndValue(1.0);
    connect(pulseAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        pulse_ = value.toDouble();
        update();
    });
    connect(pulseAnim_, &QVariantAnimation::finished, this, [this] {
        pulse_.reset();
        update();
    });
}

void MonthGrid::setMonth(int year, int month, MonthDetails details, Day today, int firstWeekday,
                         int direction) {
    const bool changed = MonthKey{year, month} != MonthKey{year_, month_};
    const bool slide = direction != 0 && changed && isVisible() && width() > 0;
    if (slide) {
        slideFrom_ = grab(gridRect().toAlignedRect());
        slideDirection_ = direction > 0 ? 1 : -1;
    }
    year_ = year;
    month_ = month;
    firstWeekday_ = firstWeekday;
    weeks_ = history::month_weeks(year, month, firstWeekday);
    today_ = today;
    hover_.reset();
    setDetails(std::move(details));
    syncSelection(false);
    if (slide) {
        slideAnim_->stop();
        slideProgress_ = 0.0;
        slideAnim_->start();
    }
    update();
}

void MonthGrid::setDetails(MonthDetails details) {
    details_ = std::move(details);
    std::int64_t peak = 0;
    for (const auto& [day, summary] : details_.days) {
        peak = std::max(peak, summary.count);
    }
    // max(..., default=1) or 1: an empty month and a month of zero counts both divide by 1.
    peak_ = details_.days.empty() ? 1 : (peak == 0 ? 1 : peak);
    update();
}

void MonthGrid::setSelected(std::optional<Day> day, bool animate) {
    selected_ = day;
    syncSelection(animate);
    update();
}

void MonthGrid::pulseToday() {
    if (!dayRect(today_)) {
        return;
    }
    pulseAnim_->stop();
    pulseAnim_->start();
}

void MonthGrid::finishAnimations() {
    for (QVariantAnimation* animation : {selectionAnim_, hoverAnim_, slideAnim_, pulseAnim_}) {
        if (animation->state() != QAbstractAnimation::Stopped) {
            animation->setCurrentTime(animation->duration());
        }
    }
}

int MonthGrid::headerHeight() const {
    return calendarPx(22);
}

QRectF MonthGrid::gridRect() const {
    const int top = headerHeight() + 6;
    return QRectF(0, top, width(), std::max(0, height() - top));
}

QRectF MonthGrid::cellRect(int row, int column) const {
    const QRectF grid = gridRect();
    const int rows = std::max<int>(1, static_cast<int>(weeks_.size()));
    const double cellWidth = (grid.width() - kGap * 6) / 7;
    const double cellHeight = (grid.height() - kGap * (rows - 1)) / rows;
    return QRectF(grid.left() + column * (cellWidth + kGap), grid.top() + row * (cellHeight + kGap),
                  cellWidth, cellHeight);
}

std::optional<QRectF> MonthGrid::dayRect(std::optional<Day> day) const {
    if (!day) {
        return std::nullopt;
    }
    for (std::size_t row = 0; row < weeks_.size(); ++row) {
        const auto& week = weeks_[row];
        const auto found = std::find(week.begin(), week.end(), *day);
        if (found != week.end()) {
            return cellRect(static_cast<int>(row), static_cast<int>(found - week.begin()));
        }
    }
    return std::nullopt;
}

std::optional<Day> MonthGrid::dayAt(const QPointF& pos) const {
    for (std::size_t row = 0; row < weeks_.size(); ++row) {
        for (std::size_t column = 0; column < weeks_[row].size(); ++column) {
            if (cellRect(static_cast<int>(row), static_cast<int>(column)).contains(pos)) {
                return weeks_[row][column];
            }
        }
    }
    return std::nullopt;
}

void MonthGrid::syncSelection(bool animate) {
    const std::optional<QRectF> target = dayRect(selected_);
    selectionAnim_->stop();
    if (!target) {
        selectionRect_.reset();
    } else if (animate && selectionRect_ && isVisible()) {
        selectionAnim_->setStartValue(*selectionRect_);
        selectionAnim_->setEndValue(*target);
        selectionAnim_->start();
    } else {
        selectionRect_ = target;
    }
}

void MonthGrid::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    paintWeekdays(painter);
    const QRectF grid = gridRect();
    painter.save();
    painter.setClipRect(grid.adjusted(-2, -2, 2, 2));
    if (slideFrom_ && slideProgress_ < 1.0) {
        const double progress = slideProgress_;
        const double shift = grid.width() * progress;
        painter.save();
        painter.setOpacity(1.0 - progress);
        painter.drawPixmap(QPointF(grid.left() - slideDirection_ * shift, grid.top()), *slideFrom_);
        painter.restore();
        painter.save();
        painter.translate(slideDirection_ * (grid.width() - shift), 0);
        painter.setOpacity(0.3 + 0.7 * progress);
        paintCells(painter);
        painter.restore();
    } else {
        paintCells(painter);
        paintSelection(painter);
        paintPulse(painter);
    }
    painter.restore();
}

void MonthGrid::paintWeekdays(QPainter& painter) {
    painter.setFont(calendarFont(font(), 11, QFont::DemiBold));
    painter.setPen(ui::tokenColor("text-secondary"));
    for (int column = 0; column < 7; ++column) {
        const int weekday = (firstWeekday_ + column) % 7;
        const QRectF cell = cellRect(0, column);
        const QRectF label(cell.left() + 8, 0, cell.width() - 8, headerHeight());
        painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, q(cv::day_abbr(weekday)));
    }
}

void MonthGrid::paintCells(QPainter& painter) {
    for (std::size_t row = 0; row < weeks_.size(); ++row) {
        for (std::size_t column = 0; column < weeks_[row].size(); ++column) {
            paintCell(painter, cellRect(static_cast<int>(row), static_cast<int>(column)),
                      toQDate(weeks_[row][column]));
        }
    }
}

QRectF MonthGrid::numberRect(const QRectF& rect) const {
    const int size = calendarPx(22);
    return QRectF(rect.left() + 5, rect.top() + 5, size, size);
}

void MonthGrid::paintCell(QPainter& painter, const QRectF& rect, const QDate& date) {
    const Day day = toDay(date);
    const bool inMonth = monthOf(day) == month_;
    const history::DaySummary* summary = nullptr;
    if (inMonth) {
        const auto found = details_.days.find(day);
        summary = found == details_.days.end() ? nullptr : &found->second;
    }
    QPainterPath path;
    path.addRoundedRect(rect, kRadius, kRadius);
    painter.fillPath(path, ui::tokenColor("surface-rgb", truncate(255 * (inMonth ? 0.55 : 0.14))));
    if (summary != nullptr && summary->count != 0) {
        // Busier days glow brighter; light surfaces need less to read as blue.
        const double level =
            std::pow(static_cast<double>(summary->count) / static_cast<double>(peak_), 0.8);
        const double span = ui::currentPalette().isDark() ? 0.44 : 0.30;
        painter.fillPath(path, ui::tokenColor("accent-rgb", truncate(255 * (0.06 + span * level))));
    }
    if (hover_ && day == *hover_) {
        painter.fillPath(path, ui::tokenColor("overlay-rgb", truncate(255 * 0.05 * hoverLevel_)));
        painter.setPen(QPen(ui::tokenColor("accent-rgb", truncate(255 * 0.5 * hoverLevel_)), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    }

    const QRectF number = numberRect(rect);
    painter.setFont(calendarFont(font(), 12.5, QFont::DemiBold));
    if (day == today_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ui::tokenColor("accent"));
        painter.drawEllipse(number);
        painter.setPen(ui::tokenColor("on-accent"));
    } else {
        painter.setPen(ui::tokenColor(inMonth ? "text" : "text-muted"));
    }
    painter.drawText(number, Qt::AlignCenter, QString::number(dayNumber(day)));

    if (inMonth) {
        const auto milestone = details_.milestones.find(day);
        if (milestone != details_.milestones.end() && milestone->second != 0) {
            painter.setFont(calendarFont(font(), 12, QFont::Bold));
            painter.setPen(ui::tokenColor("warning"));
            const QRectF star(number.right() + 2, number.top(), calendarPx(16), number.height());
            painter.drawText(star, Qt::AlignCenter, QStringLiteral(u"✦"));
        }
    }

    // Python tests the summary's truth: a DaySummary is always true, whatever its count.
    if (summary == nullptr) {
        return;
    }
    painter.setFont(calendarFont(font(), 10.5, QFont::DemiBold));
    painter.setPen(ui::tokenColor("text-secondary-strong"));
    const QRectF countRect(rect.right() - calendarPx(40) - 8, number.top(), calendarPx(40),
                           number.height());
    painter.drawText(countRect, Qt::AlignRight | Qt::AlignVCenter, q(cv::grouped(summary->count)));
    const auto items = details_.items.find(day);
    static const std::vector<history::CalendarItem> kNone;
    paintLines(painter, rect, number.bottom() + 3,
               items == details_.items.end() ? kNone : items->second);
    paintKindBar(painter, rect, *summary);
}

void MonthGrid::paintLines(QPainter& painter, const QRectF& rect, double top,
                           const std::vector<history::CalendarItem>& items) {
    if (items.empty()) {
        return;
    }
    const int lineHeight = calendarPx(16);
    const double bottom = rect.bottom() - 9;
    const int capacity = static_cast<int>(std::floor((bottom - top) / lineHeight));
    if (capacity <= 0) {
        return;
    }
    const double left = rect.left() + 9;
    const double textLeft = left + calendarPx(10);
    const double lineWidth = std::max(0.0, rect.right() - 8 - textLeft);
    if (lineWidth < calendarPx(46)) {
        paintDots(painter, QRectF(left, top + 2, rect.right() - 7 - left, bottom - top - 2), items);
        return;
    }
    const bool overflow = static_cast<int>(items.size()) > capacity;
    const std::vector<const history::CalendarItem*> shown =
        cv::pick_lines(items, overflow ? capacity - 1 : capacity);
    const QFont lineFont = calendarFont(font(), 11);
    const QFontMetrics metrics(lineFont);
    painter.setFont(lineFont);
    for (std::size_t index = 0; index < shown.size(); ++index) {
        const history::CalendarItem& item = *shown[index];
        const double y = top + static_cast<double>(index) * lineHeight;
        painter.setPen(Qt::NoPen);
        painter.setBrush(ui::tokenColor(cv::kind_color(item.kind)));
        const double radius = std::max(2.5, static_cast<double>(calendarPx(3)));
        painter.drawEllipse(QPointF(left + radius, y + lineHeight / 2.0), radius, radius);
        painter.setPen(ui::tokenColor("text-body"));
        std::string label;
        for (const std::string& word : py::split(item.label())) {
            if (!label.empty()) {
                label += ' ';
            }
            label += word;
        }
        const QString text = metrics.elidedText(q(label), Qt::ElideRight, truncate(lineWidth));
        painter.drawText(QRectF(textLeft, y, lineWidth, lineHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
    if (overflow) {
        const double y = top + static_cast<double>(shown.size()) * lineHeight;
        painter.setPen(ui::tokenColor("text-secondary"));
        painter.setFont(calendarFont(font(), 10.5, QFont::DemiBold));
        painter.drawText(QRectF(textLeft, y, lineWidth, lineHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("+%1 more").arg(items.size() - shown.size()));
    }
}

void MonthGrid::paintDots(QPainter& painter, const QRectF& area,
                          const std::vector<history::CalendarItem>& items) {
    const double radius = std::max(2.5, static_cast<double>(calendarPx(3)));
    const double step = radius * 2 + 3;
    const int columns = std::max(1, static_cast<int>(std::floor((area.width() + 3) / step)));
    const int rows = std::max(1, static_cast<int>(std::floor((area.height() + 3) / step)));
    const int room = columns * rows;
    const bool overflow = static_cast<int>(items.size()) > room;
    const std::vector<const history::CalendarItem*> shown =
        cv::pick_lines(items, overflow ? room - 1 : room);
    painter.setPen(Qt::NoPen);
    for (std::size_t index = 0; index < shown.size(); ++index) {
        const int row = static_cast<int>(index) / columns;
        const int column = static_cast<int>(index) % columns;
        painter.setBrush(ui::tokenColor(cv::kind_color(shown[index]->kind)));
        painter.drawEllipse(
            QPointF(area.left() + radius + column * step, area.top() + radius + row * step), radius,
            radius);
    }
    if (overflow) {
        const int row = static_cast<int>(shown.size()) / columns;
        const int column = static_cast<int>(shown.size()) % columns;
        painter.setPen(ui::tokenColor("text-secondary"));
        painter.setFont(calendarFont(font(), 9, QFont::Bold));
        painter.drawText(QRectF(area.left() + column * step - 1, area.top() + row * step - 2,
                                step + 2, step + 2),
                         Qt::AlignCenter, QStringLiteral("+"));
    }
}

void MonthGrid::paintKindBar(QPainter& painter, const QRectF& rect,
                             const history::DaySummary& summary) {
    const QRectF bar(rect.left() + 9, rect.bottom() - 7, rect.width() - 18, 3);
    QPainterPath clip;
    clip.addRoundedRect(bar, 1.5, 1.5);
    painter.save();
    painter.setClipPath(clip, Qt::IntersectClip);
    double x = bar.left();
    for (const std::string_view kind : history::KINDS) {
        const auto found = summary.by_kind.find(kind);
        const std::int64_t count = found == summary.by_kind.end() ? 0 : found->second;
        if (count == 0) {
            continue;
        }
        const double segment =
            bar.width() * static_cast<double>(count) / static_cast<double>(summary.count);
        painter.fillRect(QRectF(x, bar.top(), segment, bar.height()),
                         ui::tokenColor(cv::kind_color(kind)));
        x += segment;
    }
    painter.restore();
}

void MonthGrid::paintSelection(QPainter& painter) {
    if (!selectionRect_) {
        return;
    }
    const QRectF inner = selectionRect_->adjusted(1, 1, -1, -1);
    painter.setBrush(ui::tokenColor("accent-rgb", 22));
    painter.setPen(QPen(ui::tokenColor("accent"), 2));
    painter.drawRoundedRect(inner, kRadius - 1, kRadius - 1);
}

void MonthGrid::paintPulse(QPainter& painter) {
    if (!pulse_) {
        return;
    }
    const std::optional<QRectF> rect = dayRect(today_);
    if (!rect) {
        return;
    }
    const QRectF number = numberRect(*rect);
    const double radius = number.width() / 2 + calendarPx(20) * *pulse_;
    QColor color = ui::tokenColor("accent");
    setOpacity(color, 0.6 * (1.0 - *pulse_));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, 2));
    painter.drawEllipse(number.center(), radius, radius);
}

bool MonthGrid::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const std::optional<Day> day = dayAt(QPointF(help->pos()));
        if (!day) {
            QToolTip::hideText();
        } else if (monthOf(*day) != month_) {
            QToolTip::showText(
                help->globalPos(),
                q(std::format("Go to {} {}", cv::month_name(monthOf(*day)), yearOf(*day))), this);
        } else {
            const auto summary = details_.days.find(*day);
            const auto milestone = details_.milestones.find(*day);
            QToolTip::showText(
                help->globalPos(),
                q(cv::day_tooltip(*day, summary == details_.days.end() ? nullptr : &summary->second,
                                  milestone == details_.milestones.end()
                                      ? std::nullopt
                                      : std::optional<std::int64_t>(milestone->second))),
                this);
        }
        return true;
    }
    return QWidget::event(event);
}

void MonthGrid::mouseMoveEvent(QMouseEvent* event) {
    const std::optional<Day> day = dayAt(event->position());
    if (day != hover_) {
        hover_ = day;
        hoverAnim_->stop();
        hoverLevel_ = 0.0;
        if (day) {
            hoverAnim_->start();
        }
        setCursor(day ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void MonthGrid::leaveEvent(QEvent* event) {
    hover_.reset();
    update();
    QWidget::leaveEvent(event);
}

void MonthGrid::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (const std::optional<Day> day = dayAt(event->position())) {
            Q_EMIT daySelected(toQDate(*day));
        }
    }
    QWidget::mousePressEvent(event);
}

void MonthGrid::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (const std::optional<Day> day = dayAt(event->position())) {
            Q_EMIT dayActivated(toQDate(*day));
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void MonthGrid::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    int step = 0;
    switch (key) {
    case Qt::Key_Left:
        step = -1;
        break;
    case Qt::Key_Right:
        step = 1;
        break;
    case Qt::Key_Up:
        step = -7;
        break;
    case Qt::Key_Down:
        step = 7;
        break;
    default:
        break;
    }
    if (step != 0) {
        const Day base = selected_.value_or(today_);
        Q_EMIT daySelected(toQDate(addDays(base, step)));
    } else if (key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
        Q_EMIT monthStep(key == Qt::Key_PageUp ? -1 : 1);
    } else if (key == Qt::Key_Home || key == Qt::Key_T) {
        Q_EMIT todayRequested();
    } else if (key == Qt::Key_M) {
        Q_EMIT pickerRequested();
    } else if ((key == Qt::Key_Return || key == Qt::Key_Enter) && selected_) {
        Q_EMIT dayActivated(toQDate(*selected_));
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void MonthGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncSelection(false);
}

// ---- YearStrip ----

YearStrip::YearStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("calendarYearStrip"));
    setMouseTracking(true);
    setFixedHeight(calendarPx(50));
    setMinimumWidth(12 * 20);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    grow_ = new QVariantAnimation(this);
    grow_->setDuration(520);
    grow_->setEasingCurve(QEasingCurve::OutCubic);
    grow_->setStartValue(0.0);
    grow_->setEndValue(1.0);
    connect(grow_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        progress_ = value.toDouble();
        update();
    });
}

QSize YearStrip::sizeHint() const {
    return QSize(12 * calendarPx(24), calendarPx(50));
}

void YearStrip::setData(std::vector<MonthKey> months, std::map<MonthKey, std::int64_t> counts,
                        MonthKey current, MonthKey today) {
    std::int64_t peak = 0;
    for (const MonthKey& key : months) {
        const auto found = counts.find(key);
        peak = std::max(peak, found == counts.end() ? 0 : found->second);
    }
    if (peak == 0) {
        peak = 1;
    }
    std::vector<double> target;
    for (const MonthKey& key : months) {
        const auto found = counts.find(key);
        target.push_back(static_cast<double>(found == counts.end() ? 0 : found->second) /
                         static_cast<double>(peak));
    }
    std::vector<double> shown = heights();
    if (target_.empty() || shown.size() != target.size()) {
        shown.assign(target.size(), 0.0);
    }
    months_ = std::move(months);
    counts_ = std::move(counts);
    current_ = current;
    today_ = today;
    if (target != target_) {
        from_ = std::move(shown);
        target_ = std::move(target);
        grow_->stop();
        progress_ = 0.0;
        grow_->start();
    }
    update();
}

void YearStrip::finishAnimations() {
    if (grow_->state() != QAbstractAnimation::Stopped) {
        grow_->setCurrentTime(grow_->duration());
    }
}

std::vector<double> YearStrip::heights() const {
    if (from_.empty() || from_.size() != target_.size()) {
        return target_;
    }
    std::vector<double> result;
    for (std::size_t i = 0; i < from_.size(); ++i) {
        result.push_back(from_[i] + (target_[i] - from_[i]) * progress_);
    }
    return result;
}

QRectF YearStrip::slot(int index) const {
    const double slotWidth =
        static_cast<double>(width()) / std::max<int>(1, static_cast<int>(months_.size()));
    return QRectF(index * slotWidth, 0, slotWidth, height());
}

std::optional<int> YearStrip::indexAt(const QPointF& pos) const {
    for (int index = 0; index < static_cast<int>(months_.size()); ++index) {
        if (slot(index).contains(pos)) {
            return index;
        }
    }
    return std::nullopt;
}

void YearStrip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int labelHeight = calendarPx(15);
    const int chartBottom = height() - labelHeight - 3;
    const std::vector<double> levels = heights();
    painter.setFont(calendarFont(font(), 10, QFont::DemiBold));
    for (int index = 0; index < static_cast<int>(months_.size()); ++index) {
        const MonthKey key = months_[static_cast<std::size_t>(index)];
        const QRectF slotRect = slot(index);
        const double barWidth = std::max(6.0, slotRect.width() * 0.56);
        const double x = slotRect.center().x() - barWidth / 2;
        const double level =
            index < static_cast<int>(levels.size()) ? levels[static_cast<std::size_t>(index)] : 0.0;
        const auto found = counts_.find(key);
        const std::int64_t count = found == counts_.end() ? 0 : found->second;
        const double barHeight = count != 0 ? std::max(2.0, (chartBottom - 2) * level) : 2.0;
        const QRectF bar(x, chartBottom - barHeight, barWidth, barHeight);
        QColor color;
        if (current_ && key == *current_) {
            color = ui::tokenColor("accent");
        } else if (hover_ && index == *hover_) {
            color = ui::tokenColor("accent-soft");
        } else if (count != 0) {
            color = ui::tokenColor("accent-rgb", 105);
        } else {
            color = ui::tokenColor("overlay-rgb", 30);
        }
        QPainterPath path;
        path.addRoundedRect(bar, std::min(3.0, barWidth / 2), std::min(3.0, barHeight / 2));
        painter.fillPath(path, color);
        if (current_ && key == *current_) {
            painter.setPen(ui::tokenColor("text"));
        } else if (today_ && key == *today_) {
            painter.setPen(ui::tokenColor("accent-soft"));
        } else {
            painter.setPen(ui::tokenColor("text-secondary"));
        }
        const QString label = q(cv::month_abbr(key.month)).left(1);
        painter.drawText(QRectF(slotRect.left(), chartBottom + 3, slotRect.width(), labelHeight),
                         Qt::AlignCenter, label);
    }
}

bool YearStrip::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const std::optional<int> index = indexAt(QPointF(help->pos()));
        if (!index) {
            QToolTip::hideText();
        } else {
            const MonthKey key = months_[static_cast<std::size_t>(*index)];
            const auto found = counts_.find(key);
            const std::int64_t count = found == counts_.end() ? 0 : found->second;
            QToolTip::showText(
                help->globalPos(),
                q(std::format("{} {} \xC2\xB7 {}", cv::month_name(key.month), key.year,
                              count != 0 ? cv::plural(count, "recording")
                                         : std::string("nothing recorded"))),
                this);
        }
        return true;
    }
    return QWidget::event(event);
}

void YearStrip::mouseMoveEvent(QMouseEvent* event) {
    const std::optional<int> index = indexAt(event->position());
    if (index != hover_) {
        hover_ = index;
        setCursor(index ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void YearStrip::leaveEvent(QEvent* event) {
    hover_.reset();
    update();
    QWidget::leaveEvent(event);
}

void YearStrip::mousePressEvent(QMouseEvent* event) {
    const std::optional<int> index = indexAt(event->position());
    if (event->button() == Qt::LeftButton && index) {
        const MonthKey key = months_[static_cast<std::size_t>(*index)];
        Q_EMIT monthClicked(key.year, key.month);
    }
    QWidget::mousePressEvent(event);
}

// ---- MonthTitleButton ----

MonthTitleButton::MonthTitleButton(const QString& text, QWidget* parent)
    : QPushButton(text, parent) {
    setObjectName(QStringLiteral("calendarMonthTitle"));
    setCursor(Qt::PointingHandCursor);
    liftAnim_ = new QVariantAnimation(this);
    liftAnim_->setDuration(150);
    liftAnim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(liftAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        lift_ = value.toDouble();
        update();
    });
}

void MonthTitleButton::setTitle(const QString& full, const QString& shortTitle) {
    full_ = full;
    short_ = shortTitle;
    setAccessibleName(full);
    // A button's policy floors it at its size hint; this one may shrink to the short form.
    setSizePolicy(QSizePolicy::Preferred, sizePolicy().verticalPolicy());
    updateGeometry();
    fitTitle();
}

QSize MonthTitleButton::sizeFor(const QString& text) const {
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text = text;
    const QFontMetrics metrics = fontMetrics();
    const QSize contents(metrics.horizontalAdvance(text), metrics.height());
    return style()->sizeFromContents(QStyle::CT_PushButton, &option, contents, this);
}

QSize MonthTitleButton::sizeHint() const {
    return full_.isEmpty() ? QPushButton::sizeHint() : sizeFor(full_);
}

QSize MonthTitleButton::minimumSizeHint() const {
    return short_.isEmpty() ? QPushButton::minimumSizeHint() : sizeFor(short_);
}

void MonthTitleButton::fitTitle() {
    if (!full_.isEmpty()) {
        const bool fits = width() >= sizeFor(full_).width();
        setText(fits ? full_ : short_);
    }
}

void MonthTitleButton::resizeEvent(QResizeEvent* event) {
    QPushButton::resizeEvent(event);
    fitTitle();
}

void MonthTitleButton::setOpen(bool open) {
    open_ = open;
    liftTo(open || underMouse() ? 1.0 : 0.0);
    update();
}

void MonthTitleButton::liftTo(double value) {
    liftAnim_->stop();
    liftAnim_->setStartValue(lift_);
    liftAnim_->setEndValue(value);
    liftAnim_->start();
}

void MonthTitleButton::enterEvent(QEnterEvent* event) {
    QPushButton::enterEvent(event);
    liftTo(1.0);
}

void MonthTitleButton::leaveEvent(QEvent* event) {
    QPushButton::leaveEvent(event);
    if (!open_) {
        liftTo(0.0);
    }
}

void MonthTitleButton::paintEvent(QPaintEvent* event) {
    if (lift_ > 0.0) {
        QPainter pill(this);
        pill.setRenderHint(QPainter::Antialiasing);
        pill.fillPath(cv::rounded(QRectF(rect()), 10),
                      ui::tokenColor("overlay-rgb", truncate(255 * 0.06 * lift_)));
    }
    QPushButton::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color =
        cv::mix(ui::tokenColor("text-muted"), ui::tokenColor("accent-soft"), lift_);
    // Placed after the text, not from the right edge: the stylesheet's padding doesn't grow
    // with the UI font scale, but the title does.
    const int size = calendarPx(4.5);
    QStyleOptionButton option;
    initStyleOption(&option);
    const QRect contents = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
    const int textEnd = contents.left() + fontMetrics().horizontalAdvance(text());
    const int x = std::min(textEnd + calendarPx(8) + size, width() - size - 4);
    cv::paint_chevron(painter, QPointF(x, height() / 2.0 + 1), size,
                      open_ ? cv::Pointing::Up : cv::Pointing::Down, color, 2.0);
}

// ---- MonthPicker ----

MonthPicker::MonthPicker(QWidget* parent) : MonthPicker(system_clock(), parent) {}

MonthPicker::MonthPicker(const Clock& clock, QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
    setObjectName(QStringLiteral("calendarMonthPicker"));
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Choose a month"));
    const MonthKey today = history::month_key(localToday(clock));
    current_ = today;
    today_ = today;
    year_ = today.year;
    pageEnd_ = today.year;

    openAnim_ = new QVariantAnimation(this);
    openAnim_->setDuration(170);
    openAnim_->setEasingCurve(QEasingCurve::OutCubic);
    openAnim_->setStartValue(0.0);
    openAnim_->setEndValue(1.0);
    connect(openAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        shown_ = value.toDouble();
        update();
    });

    changeAnim_ = new QVariantAnimation(this);
    changeAnim_->setDuration(240);
    changeAnim_->setEasingCurve(QEasingCurve::OutCubic);
    changeAnim_->setStartValue(0.0);
    changeAnim_->setEndValue(1.0);
    connect(changeAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        change_ = value.toDouble();
        update();
    });
    connect(changeAnim_, &QVariantAnimation::finished, this, [this] {
        changeFrom_.reset();
        change_ = 1.0;
        update();
    });
}

void MonthPicker::setData(const std::map<MonthKey, std::int64_t>& counts, MonthKey current,
                          MonthKey today) {
    counts_.clear();
    for (const auto& [key, count] : counts) {
        if (count != 0) {
            counts_[key] = count;
        }
    }
    current_ = current;
    today_ = today;
    update();
}

void MonthPicker::setFocusIndex(std::optional<int> index) {
    focus_ = index;
    update();
}

int MonthPicker::firstYear() const {
    int first = std::min(current_.year, today_.year);
    for (const auto& [key, count] : counts_) {
        first = std::min(first, key.year);
    }
    return first;
}

int MonthPicker::lastYear() const {
    return std::max(current_.year, today_.year);
}

std::vector<int> MonthPicker::pageYears() const {
    const int oldest = std::max(firstYear(), pageEnd_ - kYearPage + 1);
    std::vector<int> years;
    for (int year = pageEnd_; year >= oldest; --year) {
        years.push_back(year);
    }
    return years;
}

bool MonthPicker::manyYears() const {
    return firstYear() < lastYear();
}

std::int64_t MonthPicker::yearTotal(int year) const {
    std::int64_t total = 0;
    for (int month = 1; month <= 12; ++month) {
        const auto found = counts_.find(MonthKey{year, month});
        total += found == counts_.end() ? 0 : found->second;
    }
    return total;
}

int MonthPicker::tileCount() const {
    return mode_ == Mode::Months ? 12 : static_cast<int>(pageYears().size());
}

bool MonthPicker::canStep(int step) const {
    if (mode_ == Mode::Months) {
        return firstYear() <= year_ + step && year_ + step <= lastYear();
    }
    if (step < 0) {
        return pageEnd_ - kYearPage >= firstYear();
    }
    return pageEnd_ + kYearPage <= lastYear();
}

void MonthPicker::popup(QWidget* anchor) {
    anchor_ = anchor;
    // Set again only when a press on the anchor closes it (mousePressEvent).
    setAttribute(Qt::WA_NoMouseReplay, false);
    mode_ = Mode::Months;
    year_ = current_.year;
    focus_ = current_.month - 1;
    hover_.reset();
    pressed_.reset();
    changeAnim_->stop();
    changeFrom_.reset();
    change_ = 1.0;
    setFixedSize(sizeHint());
    move(placeBy(anchor));
    shown_ = 0.0;
    show();
    setFocus();
    openAnim_->stop();
    openAnim_->start();
}

QPoint MonthPicker::placeBy(QWidget* anchor) const {
    // Below the anchor, or above it when the screen ends first; never off the side.
    const int topRoom = kShadow - kDrop;
    const int bottomRoom = kShadow + kDrop;
    const int gapPx = calendarPx(4);
    const QPoint below = anchor->mapToGlobal(QPoint(0, anchor->height() + gapPx));
    int x = below.x() - kShadow;
    int y = below.y() - topRoom;
    if (const QScreen* screen = anchor->screen()) {
        const QRect room = screen->availableGeometry();
        x = std::max(room.left() - kShadow, std::min(x, room.right() + 1 + kShadow - width()));
        if (y + height() - bottomRoom > room.bottom()) {
            y = anchor->mapToGlobal(QPoint(0, 0)).y() - gapPx - height() + bottomRoom;
        }
    }
    return QPoint(x, y);
}

void MonthPicker::choose(MonthKey key) {
    hide();
    Q_EMIT monthChosen(key.year, key.month);
}

void MonthPicker::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    openAnim_->stop();
    changeAnim_->stop();
    changeFrom_.reset();
    change_ = 1.0;
    shown_ = 1.0;
    Q_EMIT closed();
}

void MonthPicker::finishAnimations() {
    for (QVariantAnimation* animation : {openAnim_, changeAnim_}) {
        if (animation->state() != QAbstractAnimation::Stopped) {
            animation->setCurrentTime(animation->duration());
        }
    }
}

bool MonthPicker::step(int step) {
    if (!canStep(step)) {
        return false;
    }
    beginChange(step);
    if (mode_ == Mode::Months) {
        year_ += step;
    } else {
        pageEnd_ += step * kYearPage;
        focus_ = std::min(focus_.value_or(0), tileCount() - 1);
    }
    update();
    return true;
}

void MonthPicker::toggleYears() {
    if (mode_ == Mode::Months) {
        if (!manyYears()) {
            return;
        }
        beginChange(-2);
        mode_ = Mode::Years;
        // Pages count back from the newest year, so the newest always leads.
        pageEnd_ = lastYear() - (lastYear() - year_) / kYearPage * kYearPage;
        const std::vector<int> years = pageYears();
        focus_ = static_cast<int>(std::find(years.begin(), years.end(), year_) - years.begin());
    } else {
        openYear(year_);
    }
    update();
}

void MonthPicker::openYear(int year) {
    beginChange(2);
    mode_ = Mode::Months;
    year_ = year;
    if (year == current_.year) {
        focus_ = current_.month - 1;
    } else {
        int busiest = 0;
        for (int month = 1; month <= 12; ++month) {
            if (counts_.contains(MonthKey{year, month})) {
                busiest = month;
            }
        }
        focus_ = busiest != 0 ? busiest - 1 : 0;
    }
    update();
}

void MonthPicker::pick(int index) {
    if (mode_ == Mode::Months) {
        choose(MonthKey{year_, index + 1});
        return;
    }
    const std::vector<int> years = pageYears();
    if (0 <= index && index < static_cast<int>(years.size())) {
        openYear(years[static_cast<std::size_t>(index)]);
    }
}

void MonthPicker::moveFocus(int step) {
    const int count = tileCount();
    const int focus = focus_.value_or(0);
    const int target = focus + step;
    if (0 <= target && target < count) {
        focus_ = target;
    } else if (mode_ == Mode::Months) {
        // Left and right run on through the year's ends; up and down stop there.
        if (std::abs(step) == 1 && this->step(step)) {
            focus_ = ((target % 12) + 12) % 12;
        }
    } else {
        // Newest first, so past the bottom row lie older years.
        const bool older = target >= count;
        if (this->step(older ? -1 : 1)) {
            focus_ = older ? 0 : tileCount() - 1;
        }
    }
    update();
}

void MonthPicker::act(const Target& target) {
    switch (target.part) {
    case Part::Prev:
        step(-1);
        break;
    case Part::Next:
        step(1);
        break;
    case Part::Title:
        toggleYears();
        break;
    case Part::Today:
        choose(today_);
        break;
    case Part::Tile:
        pick(target.index);
        break;
    }
}

void MonthPicker::beginChange(int kind) {
    if (!isVisible()) {
        return;
    }
    changeAnim_->stop();
    change_ = 1.0;
    shown_ = 1.0;
    changeFrom_ = grab(tilesRect().adjusted(-3, -3, 3, 3).toAlignedRect());
    changeKind_ = kind;
    change_ = 0.0;
    changeAnim_->start();
}

int MonthPicker::pad() const {
    return calendarPx(10);
}

int MonthPicker::gap() const {
    return calendarPx(6);
}

QSize MonthPicker::tilesSize() const {
    const int gapPx = gap();
    return QSize(3 * calendarPx(78) + 2 * gapPx, 4 * calendarPx(52) + 3 * gapPx);
}

QSize MonthPicker::sizeHint() const {
    const QSize tiles = tilesSize();
    const int padPx = pad();
    return QSize(tiles.width() + 2 * padPx + 2 * kShadow,
                 padPx + calendarPx(34) + gap() + tiles.height() + calendarPx(4) + calendarPx(28) +
                     calendarPx(6) + 2 * kShadow);
}

QRectF MonthPicker::panel() const {
    return QRectF(kShadow, kShadow - kDrop, width() - 2 * kShadow, height() - 2 * kShadow);
}

QRectF MonthPicker::headerRect() const {
    const QRectF panelRect = panel();
    const int padPx = pad();
    return QRectF(panelRect.left() + padPx, panelRect.top() + padPx, panelRect.width() - 2 * padPx,
                  calendarPx(34));
}

QRectF MonthPicker::prevRect() const {
    const QRectF header = headerRect();
    return QRectF(header.left(), header.top(), header.height(), header.height());
}

QRectF MonthPicker::nextRect() const {
    const QRectF header = headerRect();
    return QRectF(header.right() - header.height(), header.top(), header.height(), header.height());
}

QString MonthPicker::titleText() const {
    if (mode_ == Mode::Months) {
        return QString::number(year_);
    }
    const std::vector<int> years = pageYears();
    if (years.size() > 1) {
        return QStringLiteral(u"%1 – %2").arg(years.back()).arg(years.front());
    }
    return QString::number(years.front());
}

QFont MonthPicker::titleFont() const {
    return calendarFont(font(), 14.5, QFont::Bold);
}

QRectF MonthPicker::titleRect() const {
    const QRectF header = headerRect();
    int titleWidth = QFontMetrics(titleFont()).horizontalAdvance(titleText()) + calendarPx(20);
    if (manyYears()) {
        titleWidth += calendarPx(14);
    }
    return QRectF(header.center().x() - titleWidth / 2.0, header.top() + calendarPx(3), titleWidth,
                  header.height() - calendarPx(6));
}

QRectF MonthPicker::tilesRect() const {
    const QRectF header = headerRect();
    const QSize tiles = tilesSize();
    return QRectF(header.left(), header.bottom() + gap(), tiles.width(), tiles.height());
}

QRectF MonthPicker::tileRect(int index) const {
    const QRectF tiles = tilesRect();
    const int gapPx = gap();
    if (mode_ == Mode::Months) {
        const int row = index / 3;
        const int column = index % 3;
        const double tileWidth = (tiles.width() - 2 * gapPx) / 3;
        const double tileHeight = (tiles.height() - 3 * gapPx) / 4;
        return QRectF(tiles.left() + column * (tileWidth + gapPx),
                      tiles.top() + row * (tileHeight + gapPx), tileWidth, tileHeight);
    }
    const int rows = std::max(kYearRowsMin, static_cast<int>(pageYears().size()));
    const double rowHeight = (tiles.height() - (rows - 1) * gapPx) / rows;
    return QRectF(tiles.left(), tiles.top() + index * (rowHeight + gapPx), tiles.width(),
                  rowHeight);
}

QRectF MonthPicker::footerRect() const {
    const QRectF tiles = tilesRect();
    return QRectF(tiles.left(), tiles.bottom() + calendarPx(4), tiles.width(), calendarPx(28));
}

QRectF MonthPicker::todayRect() const {
    const QRectF footer = footerRect();
    const int linkWidth =
        QFontMetrics(footerFont()).horizontalAdvance(QStringLiteral("This month")) + calendarPx(16);
    return QRectF(footer.left() - calendarPx(4), footer.top() + calendarPx(2), linkWidth,
                  footer.height() - calendarPx(4));
}

QFont MonthPicker::footerFont() const {
    return calendarFont(font(), 11.5, QFont::DemiBold);
}

QPoint MonthPicker::tileCenter(int index) const {
    return tileRect(index).center().toPoint();
}

std::optional<MonthPicker::Target> MonthPicker::targetAt(const QPointF& pos) const {
    if (prevRect().contains(pos)) {
        return Target{Part::Prev};
    }
    if (nextRect().contains(pos)) {
        return Target{Part::Next};
    }
    if (titleRect().contains(pos) && (manyYears() || mode_ == Mode::Years)) {
        return Target{Part::Title};
    }
    if (todayRect().contains(pos)) {
        return Target{Part::Today};
    }
    for (int index = 0; index < tileCount(); ++index) {
        if (tileRect(index).contains(pos)) {
            return Target{Part::Tile, index};
        }
    }
    return std::nullopt;
}

void MonthPicker::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    // Opening: it fades in while settling down the last few pixels.
    painter.setOpacity(shown_);
    painter.translate(0, -(1.0 - shown_) * calendarPx(6));
    const QRectF panelRect = panel();
    paintShadow(painter, panelRect);
    painter.fillPath(cv::rounded(panelRect, kRadius), ui::tokenColor("surface"));
    painter.setPen(QPen(ui::tokenColor("overlay-rgb", 26), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(panelRect.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    paintHeader(painter);
    paintTilesArea(painter);
    paintFooter(painter);
}

void MonthPicker::paintShadow(QPainter& painter, const QRectF& panelRect) {
    const int strength = ui::currentPalette().isDark() ? 22 : 9;
    constexpr int kLayers = 10;
    for (int layer = kLayers; layer > 0; --layer) {
        const double spread = layer * (kShadow - 2) / static_cast<double>(kLayers);
        const double fade = 1.0 - layer / static_cast<double>(kLayers + 1);
        const int alpha = truncate(strength * fade * fade);
        const QRectF rect =
            panelRect.adjusted(-spread, -spread + kDrop, spread, spread + kDrop / 2.0);
        painter.fillPath(cv::rounded(rect, kRadius + spread), QColor(0, 0, 0, alpha));
    }
}

void MonthPicker::paintHeader(QPainter& painter) {
    const std::array<std::pair<Target, cv::Pointing>, 2> arrows = {
        std::pair{Target{Part::Prev}, cv::Pointing::Left},
        std::pair{Target{Part::Next}, cv::Pointing::Right}};
    for (const auto& [target, pointing] : arrows) {
        const QRectF rect = target.part == Part::Prev ? prevRect() : nextRect();
        const bool enabled = canStep(pointing == cv::Pointing::Left ? -1 : 1);
        const bool hovered = hover_ && *hover_ == target;
        if (enabled && hovered) {
            painter.fillPath(cv::rounded(rect.adjusted(3, 3, -3, -3), rect.height() / 2 - 3),
                             ui::tokenColor("overlay-rgb", 22));
        }
        const QColor color =
            ui::tokenColor(enabled ? (hovered ? "accent-soft" : "text-secondary") : "text-faint");
        cv::paint_chevron(painter, rect.center(), calendarPx(4.5), pointing, color, 1.8);
    }

    const QRectF title = titleRect();
    const bool clickable = manyYears() || mode_ == Mode::Years;
    const bool hovered = clickable && hover_ && hover_->part == Part::Title;
    if (hovered) {
        painter.fillPath(cv::rounded(title, title.height() / 2), ui::tokenColor("overlay-rgb", 20));
    }
    painter.setFont(titleFont());
    painter.setPen(ui::tokenColor(hovered ? "accent-soft" : "text-heading"));
    const QRectF textRect = title.adjusted(
        calendarPx(10), 0, -(calendarPx(10) + (manyYears() ? calendarPx(14) : 0)), 0);
    painter.drawText(textRect, Qt::AlignCenter, titleText());
    if (manyYears()) {
        cv::paint_chevron(painter, QPointF(title.right() - calendarPx(14), title.center().y() + 1),
                          calendarPx(3.5),
                          mode_ == Mode::Years ? cv::Pointing::Up : cv::Pointing::Down,
                          ui::tokenColor(hovered ? "accent-soft" : "text-muted"), 1.6);
    }
}

void MonthPicker::paintTilesArea(QPainter& painter) {
    const QRectF area = tilesRect();
    painter.save();
    painter.setClipRect(area.adjusted(-3, -3, 3, 3));
    if (!changeFrom_ || change_ >= 1.0) {
        paintTiles(painter);
        painter.restore();
        return;
    }
    const double progress = change_;
    const int kind = changeKind_;
    const double opacity = painter.opacity();
    const QPointF origin = area.adjusted(-3, -3, 3, 3).topLeft();
    const auto scaleAbout = [&painter](const QPointF& center, double scale) {
        painter.translate(center);
        painter.scale(scale, scale);
        painter.translate(-center);
    };

    painter.save();
    painter.setOpacity(opacity * (1.0 - progress));
    if (kind == -1 || kind == 1) {
        painter.translate(-kind * area.width() * 0.3 * progress, 0);
    } else {
        scaleAbout(area.center(), 1.0 + (kind > 0 ? 0.08 : -0.08) * progress);
    }
    painter.drawPixmap(origin, *changeFrom_);
    painter.restore();

    painter.save();
    painter.setOpacity(opacity * progress);
    if (kind == -1 || kind == 1) {
        painter.translate(kind * area.width() * 0.3 * (1.0 - progress), 0);
    } else {
        scaleAbout(area.center(), 1.0 - (kind > 0 ? 0.08 : -0.08) * (1.0 - progress));
    }
    paintTiles(painter);
    painter.restore();
    painter.restore();
}

void MonthPicker::paintTiles(QPainter& painter) {
    if (mode_ == Mode::Months) {
        std::int64_t peak = 0;
        for (int month = 1; month <= 12; ++month) {
            const auto found = counts_.find(MonthKey{year_, month});
            peak = std::max(peak, found == counts_.end() ? 0 : found->second);
        }
        for (int index = 0; index < 12; ++index) {
            paintMonth(painter, tileRect(index), MonthKey{year_, index + 1}, peak,
                       focus_ && index == *focus_);
        }
        return;
    }
    const std::vector<int> years = pageYears();
    std::int64_t peak = 0;
    for (const int year : years) {
        peak = std::max(peak, yearTotal(year));
    }
    for (int index = 0; index < static_cast<int>(years.size()); ++index) {
        paintYear(painter, tileRect(index), years[static_cast<std::size_t>(index)], peak,
                  focus_ && index == *focus_);
    }
}

void MonthPicker::paintTileState(QPainter& painter, const QRectF& rect, bool current,
                                 bool focused) {
    // The shown month or year wears the grid's selection ring; focus gets its hover.
    if (focused) {
        painter.fillPath(cv::rounded(rect, 10), ui::tokenColor("overlay-rgb", 14));
    }
    if (current) {
        painter.setBrush(ui::tokenColor("accent-rgb", 22));
        painter.setPen(QPen(ui::tokenColor("accent"), 2));
        painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 9, 9);
    } else if (focused) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ui::tokenColor("accent-rgb", 128), 1));
        painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    }
}

void MonthPicker::paintMonth(QPainter& painter, const QRectF& rect, MonthKey key, std::int64_t peak,
                             bool focused) {
    const auto found = counts_.find(key);
    const std::int64_t count = found == counts_.end() ? 0 : found->second;
    const QPainterPath path = cv::rounded(rect, 10);
    painter.fillPath(path, ui::tokenColor("overlay-rgb", 9));
    if (count != 0) {
        painter.fillPath(path, cv::heat(count, peak));
    }
    paintTileState(painter, rect, key == current_, focused);

    const QString name = q(cv::month_abbr(key.month));
    painter.setFont(calendarFont(font(), 13, QFont::DemiBold));
    painter.setPen(ui::tokenColor(key > today_ ? "text-muted" : "text"));
    if (count != 0) {
        const double split = rect.top() + rect.height() * 0.56;
        painter.drawText(QRectF(rect.left(), rect.top(), rect.width(), split - rect.top()),
                         Qt::AlignHCenter | Qt::AlignBottom, name);
        painter.setFont(calendarFont(font(), 10.5, QFont::DemiBold));
        painter.setPen(ui::tokenColor(key == current_ ? "accent-soft" : "text-secondary-strong"));
        painter.drawText(QRectF(rect.left(), split + 1, rect.width(), rect.bottom() - split - 1),
                         Qt::AlignHCenter | Qt::AlignTop, q(cv::grouped(count)));
    } else {
        painter.drawText(rect, Qt::AlignCenter, name);
    }
    if (key == today_) {
        const double radius = std::max(2.5, static_cast<double>(calendarPx(3)));
        painter.setPen(Qt::NoPen);
        painter.setBrush(ui::tokenColor("accent"));
        painter.drawEllipse(QPointF(rect.right() - 9, rect.top() + 9), radius, radius);
    }
}

void MonthPicker::paintYear(QPainter& painter, const QRectF& rect, int year, std::int64_t peak,
                            bool focused) {
    const std::int64_t total = yearTotal(year);
    const QPainterPath path = cv::rounded(rect, 10);
    painter.fillPath(path, ui::tokenColor("overlay-rgb", 9));
    if (total != 0) {
        painter.fillPath(path, cv::heat(total, peak));
    }
    paintTileState(painter, rect, year == current_.year, focused);

    const int labelWidth = calendarPx(56);
    const int countWidth = calendarPx(52);
    painter.setFont(calendarFont(font(), 13, QFont::DemiBold));
    painter.setPen(ui::tokenColor("text"));
    painter.drawText(QRectF(rect.left() + calendarPx(12), rect.top(), labelWidth, rect.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, QString::number(year));
    painter.setFont(calendarFont(font(), 10.5, QFont::DemiBold));
    painter.setPen(ui::tokenColor(total != 0 ? "text-secondary-strong" : "text-muted"));
    painter.drawText(
        QRectF(rect.right() - countWidth - calendarPx(12), rect.top(), countWidth, rect.height()),
        Qt::AlignRight | Qt::AlignVCenter,
        total != 0 ? q(cv::grouped(total)) : QStringLiteral(u"—"));

    // Its twelve months in miniature, so a year's shape shows before it is opened.
    const double sparkHeight = std::min(rect.height() - calendarPx(18), double(calendarPx(26)));
    const QRectF spark(rect.left() + calendarPx(12) + labelWidth,
                       rect.center().y() - sparkHeight / 2,
                       rect.width() - labelWidth - countWidth - calendarPx(36), sparkHeight);
    std::array<std::int64_t, 12> months{};
    std::int64_t busiest = 0;
    for (int month = 1; month <= 12; ++month) {
        const auto found = counts_.find(MonthKey{year, month});
        months[static_cast<std::size_t>(month - 1)] = found == counts_.end() ? 0 : found->second;
        busiest = std::max(busiest, months[static_cast<std::size_t>(month - 1)]);
    }
    if (busiest == 0) {
        busiest = 1;
    }
    const double slotWidth = spark.width() / 12;
    const double barWidth = std::max(2.0, slotWidth * 0.58);
    painter.setPen(Qt::NoPen);
    for (int month = 1; month <= 12; ++month) {
        const std::int64_t count = months[static_cast<std::size_t>(month - 1)];
        const double barHeight = count != 0
                                     ? std::max(2.0, spark.height() * static_cast<double>(count) /
                                                         static_cast<double>(busiest))
                                     : 2.0;
        const QRectF bar(spark.left() + (month - 1) * slotWidth + (slotWidth - barWidth) / 2,
                         spark.bottom() - barHeight, barWidth, barHeight);
        QColor color;
        if (MonthKey{year, month} == current_) {
            color = ui::tokenColor("accent");
        } else if (count != 0) {
            color = ui::tokenColor("accent-rgb", 165);
        } else {
            color = ui::tokenColor("overlay-rgb", 30);
        }
        painter.fillPath(cv::rounded(bar, std::min(1.5, barWidth / 2)), color);
    }
}

void MonthPicker::paintFooter(QPainter& painter) {
    const QRectF link = todayRect();
    const bool hovered = hover_ && hover_->part == Part::Today;
    if (hovered) {
        painter.fillPath(cv::rounded(link, link.height() / 2), ui::tokenColor("accent-rgb", 30));
    }
    painter.setFont(footerFont());
    painter.setPen(ui::tokenColor(hovered ? "accent" : "accent-soft"));
    painter.drawText(link, Qt::AlignCenter, QStringLiteral("This month"));

    std::string summary;
    if (mode_ == Mode::Months) {
        const std::int64_t total = yearTotal(year_);
        summary = total != 0 ? std::format("{} in {}", cv::plural(total, "recording"), year_)
                             : std::format("Nothing in {}", year_);
    } else {
        std::int64_t total = 0;
        for (const auto& [key, count] : counts_) {
            total += count;
        }
        summary = total != 0
                      ? std::format("{} since {}", cv::plural(total, "recording"), firstYear())
                      : std::string("Nothing recorded yet");
    }
    const QRectF footer = footerRect();
    const QRectF room(link.right() + calendarPx(8), footer.top(),
                      footer.right() - link.right() - calendarPx(10), footer.height());
    const QFont summaryFont = calendarFont(font(), 11);
    painter.setFont(summaryFont);
    painter.setPen(ui::tokenColor("text-secondary"));
    painter.drawText(
        room, Qt::AlignRight | Qt::AlignVCenter,
        QFontMetrics(summaryFont).elidedText(q(summary), Qt::ElideRight, truncate(room.width())));
}

QString MonthPicker::tooltip(const Target& target) const {
    switch (target.part) {
    case Part::Tile: {
        std::int64_t count = 0;
        std::string name;
        if (mode_ == Mode::Months) {
            const auto found = counts_.find(MonthKey{year_, target.index + 1});
            count = found == counts_.end() ? 0 : found->second;
            name = std::format("{} {}", cv::month_name(target.index + 1), year_);
        } else {
            const int year = pageYears()[static_cast<std::size_t>(target.index)];
            count = yearTotal(year);
            name = std::to_string(year);
        }
        return q(name + " \xC2\xB7 " +
                 (count != 0 ? cv::plural(count, "recording") : std::string("nothing recorded")));
    }
    case Part::Prev:
    case Part::Next: {
        const int stepBy = target.part == Part::Prev ? -1 : 1;
        if (!canStep(stepBy)) {
            return QString();
        }
        if (mode_ == Mode::Years) {
            return stepBy < 0 ? QStringLiteral("Earlier years") : QStringLiteral("Later years");
        }
        return stepBy < 0 ? QStringLiteral("Previous year (Page Up)")
                          : QStringLiteral("Next year (Page Down)");
    }
    case Part::Title:
        return mode_ == Mode::Months ? QStringLiteral("Every year (Y)")
                                     : QStringLiteral("Back to %1 (Y)").arg(year_);
    case Part::Today:
        break;
    }
    return QStringLiteral("Go to this month (T)");
}

bool MonthPicker::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const std::optional<Target> target = targetAt(QPointF(help->pos()));
        const QString text = target ? tooltip(*target) : QString();
        if (!text.isEmpty()) {
            QToolTip::showText(help->globalPos(), text, this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(event);
}

void MonthPicker::mouseMoveEvent(QMouseEvent* event) {
    const std::optional<Target> target = targetAt(event->position());
    if (target != hover_) {
        hover_ = target;
        if (target && target->part == Part::Tile) {
            focus_ = target->index;
        }
        setCursor(target ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void MonthPicker::leaveEvent(QEvent* event) {
    hover_.reset();
    update();
    QWidget::leaveEvent(event);
}

void MonthPicker::mousePressEvent(QMouseEvent* event) {
    if (!panel().contains(event->position())) {
        // A press on the title that opened it only closes it: replayed to the title, the press
        // would open it straight back up.
        if (anchor_ != nullptr &&
            anchor_->rect().contains(anchor_->mapFromGlobal(event->globalPosition().toPoint()))) {
            setAttribute(Qt::WA_NoMouseReplay, true);
        }
        hide();
        return;
    }
    const bool left = event->button() == Qt::LeftButton;
    pressed_ = left ? targetAt(event->position()) : std::nullopt;
    event->accept();
}

void MonthPicker::mouseReleaseEvent(QMouseEvent* event) {
    const std::optional<Target> target = targetAt(event->position());
    const std::optional<Target> pressed = std::exchange(pressed_, std::nullopt);
    if (event->button() == Qt::LeftButton && target && target == pressed) {
        act(*target);
    }
    event->accept();
}

void MonthPicker::wheelEvent(QWheelEvent* event) {
    wheel_ += event->angleDelta().y();
    while (std::abs(wheel_) >= 120) {
        const int stepBy = wheel_ > 0 ? -1 : 1;
        wheel_ += 120 * stepBy;
        step(stepBy);
    }
    event->accept();
}

void MonthPicker::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    const int vertical = mode_ == Mode::Months ? 3 : 1;
    int move = 0;
    switch (key) {
    case Qt::Key_Left:
        move = -1;
        break;
    case Qt::Key_Right:
        move = 1;
        break;
    case Qt::Key_Up:
        move = -vertical;
        break;
    case Qt::Key_Down:
        move = vertical;
        break;
    default:
        break;
    }
    if (move != 0) {
        moveFocus(move);
    } else if (key == Qt::Key_PageUp || key == Qt::Key_PageDown) {
        step(key == Qt::Key_PageUp ? -1 : 1);
    } else if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
        if (focus_) {
            pick(*focus_);
        }
    } else if (key == Qt::Key_Y) {
        toggleYears();
    } else if (key == Qt::Key_Home || key == Qt::Key_T) {
        choose(today_);
    } else if (key == Qt::Key_Escape) {
        if (mode_ == Mode::Years) {
            toggleYears();
        } else {
            hide();
        }
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

// ---- KindChip ----

KindChip::KindChip(std::string kind, QWidget* parent)
    : QPushButton(parent), kind_(std::move(kind)) {
    setObjectName(QStringLiteral("calendarKindChip"));
    setCheckable(true);
    setChecked(true);
    setFixedHeight(30);
    setCursor(Qt::PointingHandCursor);
    setToolTip(q(std::format("Show or hide {}", history::kind_plural(kind_))));
    setCount(std::nullopt);
}

void KindChip::setCount(std::optional<std::int64_t> count) {
    QString name;
    if (kind_ == history::DICTATION_KIND) {
        name = QStringLiteral("Dictations");
    } else if (kind_ == history::FILE_KIND) {
        name = QStringLiteral("Files");
    } else {
        name = QStringLiteral("Meetings");
    }
    setText(count ? QStringLiteral("%1  %2").arg(name, q(cv::grouped(*count))) : name);
}

void KindChip::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor color = ui::tokenColor(cv::kind_color(kind_));
    if (!isChecked()) {
        setOpacity(color, 0.35);
    }
    const double radius = std::max(3.0, static_cast<double>(calendarPx(4)));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(12 + radius, height() / 2.0), radius, radius);
}

// ---- AgendaCard ----

AgendaCard::AgendaCard(history::CalendarItem item, std::optional<std::int64_t> milestone,
                       QWidget* parent)
    : QFrame(parent), item_(std::move(item)) {
    setObjectName(QStringLiteral("calendarAgendaCard"));
    ui::setStyleProperty(this, ui::prop::kind, q(item_.kind));
    setCursor(Qt::PointingHandCursor);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &AgendaCard::showMenu);
    setAccessibleName(
        q(std::format("{} at {}", history::kind_label(item_.kind), cv::clock(item_.when))));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 12, 11);
    layout->setSpacing(6);

    auto* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(6);
    timeLabel_ = new QLabel(q(cv::clock(item_.when)));
    timeLabel_->setObjectName(QStringLiteral("calendarCardTime"));
    top->addWidget(timeLabel_);
    kindPill_ = new QLabel(q(history::kind_label(item_.kind)));
    kindPill_->setObjectName(QStringLiteral("calendarKindPill"));
    ui::setStyleProperty(kindPill_, ui::prop::kind, q(item_.kind));
    top->addWidget(kindPill_);
    top->addStretch();
    const auto [location, tooltip] = item_.location();
    if (!location.empty()) {
        auto* chip = new QLabel(q(location));
        chip->setObjectName(QStringLiteral("calendarPlaceChip"));
        chip->setToolTip(q(tooltip));
        top->addWidget(chip);
    }
    if (item_.seconds >= 1) {
        auto* length = new QLabel(q(format_audio_duration(item_.seconds)));
        length->setObjectName(QStringLiteral("calendarLengthChip"));
        length->setToolTip(QStringLiteral("Length of the recording"));
        top->addWidget(length);
    }
    layout->addLayout(top);

    if (!item_.title.empty()) {
        titleLabel_ = new WrappedLabel(q(item_.title));
        titleLabel_->setObjectName(QStringLiteral("calendarCardTitle"));
        layout->addWidget(titleLabel_);
    }
    const std::string preview =
        !item_.preview.empty() && item_.preview != item_.title ? item_.preview : std::string();
    if (!preview.empty() || item_.title.empty()) {
        previewLabel_ = new WrappedLabel(preview.empty() ? QStringLiteral("Nothing was transcribed")
                                                         : q(preview));
        previewLabel_->setObjectName(QStringLiteral("calendarCardPreview"));
        layout->addWidget(previewLabel_);
    }
    if (milestone && *milestone != 0) {
        auto* star =
            new QLabel(q(std::string("\xE2\x9C\xA6 ") + history::milestone_text(*milestone)));
        star->setObjectName(QStringLiteral("calendarMilestone"));
        layout->addWidget(star);
    }
    progressLabel_ = new QLabel(QString());
    progressLabel_->setObjectName(QStringLiteral("calendarCardProgress"));
    progressLabel_->setWordWrap(true);
    progressLabel_->hide();
    layout->addWidget(progressLabel_);
}

void AgendaCard::setProgress(const QString& text) {
    progressLabel_->setText(text);
    progressLabel_->setVisible(!text.isEmpty());
}

void AgendaCard::open() {
    Q_EMIT activated(q(item_.record_id));
}

void AgendaCard::copy() {
    Q_EMIT copyRequested(q(item_.record_id));
}

void AgendaCard::showMenu(const QPoint& pos) {
    QMenu menu(this);
    ui::setLocalStyleSheet(&menu, QString::fromUtf8(kMenuStyle));
    connect(menu.addAction(QStringLiteral("Open")), &QAction::triggered, this, &AgendaCard::open);
    QAction* copyAction =
        menu.addAction(item_.kind == history::MEETING_KIND ? QStringLiteral("Copy transcript")
                                                           : QStringLiteral("Copy text"));
    connect(copyAction, &QAction::triggered, this, &AgendaCard::copy);
    menu.exec(mapToGlobal(pos));
}

void AgendaCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        open();
    }
    QFrame::mousePressEvent(event);
}

// ---- fade_in ----

void fadeIn(const QList<QWidget*>& widgets, int staggerMs, int durationMs) {
    // Only the first few are staggered, so a long list doesn't keep appearing.
    const int count = std::min<int>(8, static_cast<int>(widgets.size()));
    for (int index = 0; index < count; ++index) {
        QWidget* widget = widgets[index];
        auto* effect = new QGraphicsOpacityEffect(widget);
        effect->setOpacity(0.0);
        widget->setGraphicsEffect(effect);
        auto* group = new QSequentialAnimationGroup(widget);
        if (index != 0) {
            group->addPause(index * staggerMs);
        }
        auto* fade = new QPropertyAnimation(effect, "opacity", group);
        fade->setDuration(durationMs);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);
        fade->setEasingCurve(QEasingCurve::OutCubic);
        group->addAnimation(fade);
        QObject::connect(group, &QAbstractAnimation::finished, widget,
                         [widget] { widget->setGraphicsEffect(nullptr); });
        group->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void finishFadeIn(QWidget* widget) {
    for (QSequentialAnimationGroup* group :
         widget->findChildren<QSequentialAnimationGroup*>(Qt::FindDirectChildrenOnly)) {
        group->stop();
    }
    widget->setGraphicsEffect(nullptr);
}

} // namespace fv
