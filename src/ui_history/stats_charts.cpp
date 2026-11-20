#include "ui_history/stats_charts.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <utility>

#include <QBrush>
#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QSizePolicy>
#include <QToolTip>

#include "appkit/qt_bridge.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/palette.h"
#include "ui_theme/theme.h"

namespace fv {
namespace stats_view {
namespace {

constexpr std::array<const char*, 7> kWeekdayNames = {"Monday", "Tuesday",  "Wednesday", "Thursday",
                                                      "Friday", "Saturday", "Sunday"};
constexpr std::array<const char*, 12> kMonthNames = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

} // namespace

std::string thousands(std::int64_t count) {
    const bool negative = count < 0;
    std::string digits = std::to_string(negative ? -count : count);
    std::string out;
    const std::size_t lead = digits.size() % 3;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && index >= lead && (index - lead) % 3 == 0) {
            out += ',';
        }
        out += digits[index];
    }
    return negative ? "-" + out : out;
}

std::string words_text(std::int64_t count) {
    return thousands(count) + (count == 1 ? " word" : " words");
}

std::string hour_text(int hour) {
    const int twelve = hour % 12 == 0 ? 12 : hour % 12;
    return std::format("{} {}", twelve, hour % 24 < 12 ? "AM" : "PM");
}

std::string weekday_name(int weekday) {
    return kWeekdayNames.at(static_cast<std::size_t>(weekday));
}

std::string weekday_abbr(int weekday) {
    return weekday_name(weekday).substr(0, 3);
}

std::string month_name(unsigned month) {
    return kMonthNames.at(month - 1);
}

std::string month_abbr(unsigned month) {
    return month_name(month).substr(0, 3);
}

int weekday_of(dictation_stats::Day day) {
    return static_cast<int>(std::chrono::weekday{std::chrono::sys_days{day}}.iso_encoding()) - 1;
}

std::vector<std::int64_t> quartile_thresholds(const std::vector<std::int64_t>& words) {
    std::vector<std::int64_t> busy;
    for (const std::int64_t count : words) {
        if (count != 0) {
            busy.push_back(count);
        }
    }
    if (busy.empty()) {
        return {};
    }
    std::sort(busy.begin(), busy.end());
    std::vector<std::int64_t> thresholds;
    for (const std::size_t step : {1, 2, 3}) {
        thresholds.push_back(busy[(busy.size() - 1) * step / 4]);
    }
    return thresholds;
}

int level(const std::vector<std::int64_t>& thresholds, std::int64_t words) {
    if (words == 0) {
        return 0;
    }
    int above = 0;
    for (const std::int64_t threshold : thresholds) {
        if (words > threshold) {
            ++above;
        }
    }
    return 1 + above;
}

std::optional<std::pair<int, int>> grid_peak(const HourWords& grid) {
    // max((words, -weekday, -hour)): the most words, then the earliest weekday and hour.
    std::int64_t best = 0;
    std::pair<int, int> at{0, 0};
    for (int weekday = 0; weekday < 7; ++weekday) {
        for (int hour = 0; hour < 24; ++hour) {
            const std::int64_t words =
                grid[static_cast<std::size_t>(weekday)][static_cast<std::size_t>(hour)];
            if (words > best) {
                best = words;
                at = {weekday, hour};
            }
        }
    }
    if (best == 0) {
        return std::nullopt;
    }
    return at;
}

} // namespace stats_view

namespace {

namespace sv = stats_view;

// Shades of the activity map and the hour grid, from the least to the busiest quarter.
constexpr std::array<int, 4> kLevelAlphas = {70, 125, 185, 255};

QString q(const std::string& utf8) {
    return qt::toQString(utf8);
}

QColor withAlpha(QColor color, std::optional<int> alpha) {
    if (alpha) {
        color.setAlpha(*alpha);
    }
    return color;
}

// token_color(tone, alpha) for the tones the charts are given.
QColor toneColor(const QString& tone, std::optional<int> alpha) {
    const ui::Theme& theme = ui::currentTheme();
    if (tone == u"accent") {
        return withAlpha(theme.accent, alpha);
    }
    if (tone == u"success") {
        return withAlpha(theme.success, alpha);
    }
    return ui::tokenColor(qt::toUtf8(tone), alpha);
}

QFont smallFont(const QWidget& widget) {
    QFont font(widget.font());
    font.setPointSizeF(std::max(6.5, font.pointSizeF() * 0.8));
    return font;
}

void rounded(QPainter& painter, const QRectF& rect, double radius, const QBrush& brush) {
    QPainterPath path;
    radius = std::min({radius, rect.width() / 2, rect.height() / 2});
    path.addRoundedRect(rect, radius, radius);
    painter.fillPath(path, brush);
}

// Python's floor division.
int floorDiv(int a, int b) {
    int quotient = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --quotient;
    }
    return quotient;
}

std::int64_t percent(std::int64_t words, std::int64_t total) {
    return static_cast<std::int64_t>(
        std::nearbyint(static_cast<double>(words * 100) / static_cast<double>(total)));
}

dictation_stats::Day addDays(dictation_stats::Day day, int count) {
    return dictation_stats::Day{std::chrono::sys_days{day} + std::chrono::days{count}};
}

int dayDistance(dictation_stats::Day later, dictation_stats::Day earlier) {
    return static_cast<int>(
        (std::chrono::sys_days{later} - std::chrono::sys_days{earlier}).count());
}

// QLocale().firstDayOfWeek() with Monday 0.
int localeFirstWeekday() {
    return static_cast<int>(QLocale().firstDayOfWeek()) - 1;
}

} // namespace

// ---- BarChart ----------------------------------------------------------------------------

BarChart::BarChart(int height, const QString& tone, QWidget* parent, ValueText valueText)
    : QWidget(parent), plotHeight_(ui::scaled(height)), tone_(tone),
      valueText_(valueText ? std::move(valueText)
                           : ValueText([](std::int64_t n) { return q(sv::words_text(n)); })) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(plotHeight_);
}

void BarChart::setBars(std::vector<ChartBar> bars, const QString& name,
                       std::optional<int> highlight, double average) {
    bars_ = std::move(bars);
    highlight_ = highlight;
    average_ = average;
    hover_.reset();
    const bool axis = std::any_of(bars_.begin(), bars_.end(),
                                  [](const ChartBar& bar) { return !bar.axis.isEmpty(); });
    setFixedHeight(plotHeight_ + (axis ? axisHeight() : 0));
    // Screen readers get every column; the chart only shows them on hover.
    setAccessibleName(name);
    QStringList parts;
    for (const ChartBar& bar : bars_) {
        parts << bar.name + QStringLiteral(": ") + valueText_(bar.words);
    }
    setAccessibleDescription(parts.join(QStringLiteral("; ")));
    update();
}

int BarChart::axisHeight() const {
    return fontMetrics().height() + 4;
}

std::vector<QRectF> BarChart::slots() const {
    const auto count = static_cast<double>(bars_.size());
    if (bars_.empty()) {
        return {};
    }
    const double full = width();
    double gap = std::max(2.0, std::min(10.0, full / count * 0.3));
    const double widest = ui::scaled(36);
    const double column = std::max(2.0, std::min(widest, (full - gap * (count - 1)) / count));
    if (column == widest) {
        gap = std::max(gap, (full - column * count) / count);
    }
    const double left = (full - (column * count + gap * (count - 1))) / 2;
    std::vector<QRectF> rects;
    for (std::size_t index = 0; index < bars_.size(); ++index) {
        rects.emplace_back(left + static_cast<double>(index) * (column + gap), 0, column,
                           plotHeight_);
    }
    return rects;
}

void BarChart::paintEvent(QPaintEvent* /*event*/) {
    const ui::Theme& theme = ui::currentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    std::int64_t peak = 0;
    for (const ChartBar& bar : bars_) {
        peak = std::max(peak, bar.words);
    }
    painter.setFont(smallFont(*this));
    const std::vector<QRectF> rects = slots();
    for (std::size_t index = 0; index < rects.size(); ++index) {
        const QRectF& slot = rects[index];
        const ChartBar& bar = bars_[index];
        const int at = static_cast<int>(index);
        const bool strong = (highlight_ && *highlight_ == at) || (hover_ && *hover_ == at);
        if (bar.words != 0 && peak != 0) {
            const double height = std::max(std::min(6.0, slot.height() / 3),
                                           slot.height() * static_cast<double>(bar.words) /
                                               static_cast<double>(peak));
            const QRectF columnRect(slot.left(), slot.bottom() - height, slot.width(), height);
            QLinearGradient fill(columnRect.topLeft(), columnRect.bottomLeft());
            fill.setColorAt(0, toneColor(tone_, strong ? 255 : 150));
            fill.setColorAt(1, toneColor(tone_, strong ? 190 : 80));
            rounded(painter, columnRect, std::min(5.0, slot.height() / 8), QBrush(fill));
        } else {
            const double stubHeight = std::min(4.0, slot.height() / 6);
            const QRectF stub(slot.left(), slot.bottom() - stubHeight, slot.width(), stubHeight);
            rounded(painter, stub, 2, withAlpha(theme.overlayRgb, 30));
        }
        if (!bar.axis.isEmpty()) {
            // Centred under its column, but kept inside the chart at the ends.
            const double text = painter.fontMetrics().horizontalAdvance(bar.axis) + 2;
            const double x = std::max(0.0, std::min(width() - text, slot.center().x() - text / 2));
            painter.setPen(theme.slateText4);
            painter.drawText(QRectF(x, slot.bottom() + 3, text, axisHeight()),
                             Qt::AlignHCenter | Qt::AlignTop, bar.axis);
        }
    }
    if (average_ != 0.0 && peak != 0) {
        const double y = plotHeight_ - plotHeight_ * std::min(1.0, average_ / peak);
        QPen pen(withAlpha(theme.slateText3, 150), 1, Qt::DashLine);
        pen.setDashPattern({3, 3});
        painter.setPen(pen);
        painter.drawLine(QPointF(0, y), QPointF(width(), y));
    }
}

std::optional<int> BarChart::indexAt(double x) const {
    const std::vector<QRectF> rects = slots();
    for (std::size_t index = 0; index < rects.size(); ++index) {
        if (rects[index].left() - 2 <= x && x <= rects[index].right() + 2) {
            return static_cast<int>(index);
        }
    }
    return std::nullopt;
}

void BarChart::mouseMoveEvent(QMouseEvent* event) {
    const std::optional<int> index = indexAt(event->position().x());
    if (index != hover_) {
        hover_ = index;
        update();
    }
    if (!index) {
        QToolTip::hideText();
        return;
    }
    const ChartBar& bar = bars_[static_cast<std::size_t>(*index)];
    QToolTip::showText(event->globalPosition().toPoint(),
                       bar.name + QStringLiteral(" · ") + valueText_(bar.words), this);
}

void BarChart::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    hover_.reset();
    update();
}

// ---- ActivityMap -------------------------------------------------------------------------

ActivityMap::ActivityMap(QWidget* parent)
    : QWidget(parent), cell_(ui::scaled(11)), gap_(std::max(2, ui::scaled(3))),
      firstWeekday_(localeFirstWeekday()) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const int label = fontMetrics().height();
    setFixedHeight(label + 4 + 7 * (cell_ + gap_) + label + 6);
    setMinimumWidth(gutter() + 8 * (cell_ + gap_));
}

void ActivityMap::setDays(std::vector<std::pair<dictation_stats::Day, std::int64_t>> days) {
    days_ = std::move(days);
    std::vector<std::int64_t> words;
    for (const auto& [day, count] : days_) {
        words.push_back(count);
    }
    thresholds_ = sv::quartile_thresholds(words);
    const auto active = std::count_if(days_.begin(), days_.end(),
                                      [](const auto& item) { return item.second != 0; });
    setAccessibleName(QStringLiteral("Activity, a square per day"));
    if (active != 0) {
        // max(days, key=(words, day)): the most words, the later day on a tie.
        const auto best =
            std::max_element(days_.begin(), days_.end(), [](const auto& a, const auto& b) {
                return std::pair(a.second, std::chrono::sys_days{a.first}) <
                       std::pair(b.second, std::chrono::sys_days{b.first});
            });
        setAccessibleDescription(
            q(std::format("{} days with words; the busiest, {} {}, had {}", active,
                          sv::month_name(static_cast<unsigned>(best->first.month())),
                          static_cast<unsigned>(best->first.day()), sv::words_text(best->second))));
    } else {
        setAccessibleDescription(QStringLiteral("No days with words yet"));
    }
    update();
}

int ActivityMap::gutter() const {
    return fontMetrics().horizontalAdvance(QStringLiteral("Wed")) + 6;
}

int ActivityMap::row(dictation_stats::Day day) const {
    return ((sv::weekday_of(day) - firstWeekday_) % 7 + 7) % 7;
}

void ActivityMap::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(smallFont(*this));
    cells_.clear();
    if (!days_.empty()) {
        paintGrid(painter);
    }
    paintLegend(painter);
}

void ActivityMap::paintGrid(QPainter& painter) {
    const int step = cell_ + gap_;
    const int gutterWidth = gutter();
    const int weeks = std::max(1, std::min(kWeeks, floorDiv(width() - gutterWidth + gap_, step)));
    const int top = fontMetrics().height() + 4;
    const dictation_stats::Day today = days_.back().first;
    const dictation_stats::Day thisWeek = addDays(today, -row(today));
    const int left = width() - weeks * step + gap_;
    const QColor muted = ui::currentTheme().slateText4;
    painter.setPen(muted);
    for (int each = 0; each < 7; ++each) {
        const int weekday = (firstWeekday_ + each) % 7;
        if (weekday == 0 || weekday == 2 || weekday == 4) {
            static constexpr std::array<const char*, 3> kNames = {"Mon", "Wed", "Fri"};
            painter.drawText(QRectF(0, top + each * step - 2, gutterWidth - 6, cell_ + 4),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QString::fromLatin1(kNames[static_cast<std::size_t>(weekday / 2)]));
        }
    }
    int lastLabel = -99;
    for (const auto& [day, words] : days_) {
        const int column = weeks - 1 - floorDiv(dayDistance(thisWeek, addDays(day, -row(day))), 7);
        if (column < 0) {
            continue;
        }
        const int x = left + column * step;
        const QRectF cell(x, top + row(day) * step, cell_, cell_);
        paintCell(painter, cell, level(words), day == today);
        cells_.push_back({cell, day, words});
        if (static_cast<unsigned>(day.day()) <= 7 && row(day) == 0 && column - lastLabel >= 3) {
            painter.setPen(muted);
            painter.drawText(QRectF(x, 0, 60, top), Qt::AlignLeft,
                             q(sv::month_abbr(static_cast<unsigned>(day.month()))));
            lastLabel = column;
        }
    }
}

void ActivityMap::paintCell(QPainter& painter, const QRectF& cell, int level, bool today) const {
    const ui::Theme& theme = ui::currentTheme();
    const QColor colour =
        level == 0 ? withAlpha(theme.overlayRgb, 26)
                   : withAlpha(theme.accent, kLevelAlphas[static_cast<std::size_t>(level - 1)]);
    rounded(painter, cell, 3, colour);
    if (today) {
        painter.setPen(QPen(theme.slateText3, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(cell.adjusted(-1.5, -1.5, 1.5, 1.5), 4, 4);
    }
}

void ActivityMap::paintLegend(QPainter& painter) const {
    const QFontMetrics metrics = fontMetrics();
    const int y = height() - metrics.height() - 1;
    const int step = cell_ + gap_;
    const int right = width();
    const int more = metrics.horizontalAdvance(QStringLiteral("More"));
    const QColor muted = ui::currentTheme().slateText4;
    painter.setPen(muted);
    painter.drawText(QRectF(right - more, y, more + 2, metrics.height()), Qt::AlignLeft,
                     QStringLiteral("More"));
    const int x = right - more - 6 - 5 * step;
    for (int each = 0; each < 5; ++each) {
        const QRectF cell(x + each * step, y + (metrics.height() - cell_) / 2.0, cell_, cell_);
        paintCell(painter, cell, each, false);
    }
    const int less = metrics.horizontalAdvance(QStringLiteral("Less"));
    painter.setPen(muted);
    painter.drawText(QRectF(x - less - 6, y, less + 2, metrics.height()), Qt::AlignLeft,
                     QStringLiteral("Less"));
}

void ActivityMap::mouseMoveEvent(QMouseEvent* event) {
    const QPointF position = event->position();
    for (const Cell& cell : cells_) {
        if (cell.rect.adjusted(-1, -1, 1, 1).contains(position)) {
            const int weekday = sv::weekday_of(cell.day);
            QToolTip::showText(
                event->globalPosition().toPoint(),
                q(std::format("{}, {} {} · {}", sv::weekday_abbr(weekday),
                              sv::month_abbr(static_cast<unsigned>(cell.day.month())),
                              static_cast<unsigned>(cell.day.day()), sv::words_text(cell.words))),
                this);
            return;
        }
    }
    QToolTip::hideText();
}

// ---- HourGrid ----------------------------------------------------------------------------

HourGrid::HourGrid(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const int first = localeFirstWeekday();
    for (int offset = 0; offset < 7; ++offset) {
        order_[static_cast<std::size_t>(offset)] = (first + offset) % 7;
    }
    setMinimumWidth(gutter() + 24 * (3 + gap_));
    fitHeight();
}

void HourGrid::setGrid(const sv::HourWords& grid) {
    grid_ = grid;
    std::vector<std::int64_t> words;
    for (const auto& day : grid) {
        words.insert(words.end(), day.begin(), day.end());
    }
    thresholds_ = sv::quartile_thresholds(words);
    setAccessibleName(QStringLiteral("Words by weekday and hour"));
    if (const std::optional<std::pair<int, int>> busiest = peak()) {
        const auto [weekday, hour] = *busiest;
        setAccessibleDescription(q(std::format(
            "Busiest on {}s around {}, with {}", sv::weekday_name(weekday), sv::hour_text(hour),
            sv::words_text(
                grid[static_cast<std::size_t>(weekday)][static_cast<std::size_t>(hour)]))));
    } else {
        setAccessibleDescription(QStringLiteral("No words yet"));
    }
    update();
}

std::optional<std::pair<int, int>> HourGrid::peak() const {
    if (!grid_) {
        return std::nullopt;
    }
    return sv::grid_peak(*grid_);
}

int HourGrid::gutter() const {
    return fontMetrics().horizontalAdvance(QStringLiteral("Wed")) + 6;
}

double HourGrid::cell() const {
    const double room = (width() - gutter() - 23.0 * gap_) / 24;
    return std::max(3.0, std::min(static_cast<double>(ui::scaled(20)), room));
}

void HourGrid::fitHeight() {
    setFixedHeight(static_cast<int>(std::nearbyint(7 * (cell() + gap_))) + fontMetrics().height() +
                   4);
}

void HourGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    fitHeight();
}

void HourGrid::paintEvent(QPaintEvent* /*event*/) {
    const ui::Theme& theme = ui::currentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(smallFont(*this));
    const double side = cell();
    const int gutterWidth = gutter();
    const double step = side + gap_;
    const QColor muted = theme.slateText4;
    cells_.clear();
    for (int row = 0; row < 7; ++row) {
        const int weekday = order_[static_cast<std::size_t>(row)];
        const double top = row * step;
        if (row % 2 == 0) {
            painter.setPen(muted);
            painter.drawText(QRectF(0, top - 2, gutterWidth - 6, side + 4),
                             Qt::AlignLeft | Qt::AlignVCenter, q(sv::weekday_abbr(weekday)));
        }
        for (int hour = 0; hour < 24; ++hour) {
            const std::int64_t words =
                grid_ ? (*grid_)[static_cast<std::size_t>(weekday)][static_cast<std::size_t>(hour)]
                      : 0;
            const QRectF rect(gutterWidth + hour * step, top, side, side);
            const int shade = level(words);
            const QColor colour =
                shade == 0
                    ? withAlpha(theme.overlayRgb, 26)
                    : withAlpha(theme.accent, kLevelAlphas[static_cast<std::size_t>(shade - 1)]);
            rounded(painter, rect, 2, colour);
            cells_.push_back({rect, weekday, hour, words});
        }
    }
    painter.setPen(muted);
    const double bottom = 7 * step + 2;
    const QFontMetrics metrics = painter.fontMetrics();
    for (const int hour : {0, 6, 12, 18}) {
        const QString text = q(sv::hour_text(hour));
        const double x =
            std::min(gutterWidth + hour * step,
                     static_cast<double>(width() - metrics.horizontalAdvance(text) - 1));
        painter.drawText(QRectF(x, bottom, metrics.horizontalAdvance(text) + 2, metrics.height()),
                         Qt::AlignLeft, text);
    }
}

void HourGrid::mouseMoveEvent(QMouseEvent* event) {
    const QPointF position = event->position();
    for (const Cell& cell : cells_) {
        if (cell.rect.adjusted(-1, -1, 1, 1).contains(position)) {
            QToolTip::showText(event->globalPosition().toPoint(),
                               q(std::format("{}s, {} – {} · {}", sv::weekday_name(cell.weekday),
                                             sv::hour_text(cell.hour), sv::hour_text(cell.hour + 1),
                                             sv::words_text(cell.words))),
                               this);
            return;
        }
    }
    QToolTip::hideText();
}

// ---- SplitBar ----------------------------------------------------------------------------

SplitBar::SplitBar(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFixedHeight(8);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SplitBar::setParts(const std::vector<Part>& parts, const QString& name) {
    parts_.clear();
    std::int64_t total = 0;
    for (const Part& part : parts) {
        if (part.words > 0) {
            parts_.push_back(part);
            total += part.words;
        }
    }
    setAccessibleName(name);
    QStringList shares;
    for (const Part& part : parts_) {
        shares << part.label + QStringLiteral(": ") + QString::number(percent(part.words, total)) +
                      QStringLiteral("%");
    }
    setAccessibleDescription(shares.join(QStringLiteral("; ")));
    update();
}

std::vector<QRectF> SplitBar::rects() const {
    std::int64_t total = 0;
    for (const Part& part : parts_) {
        total += part.words;
    }
    if (total == 0) {
        return {};
    }
    constexpr double kGap = 2.0;
    const double room = std::max(0.0, width() - kGap * static_cast<double>(parts_.size() - 1));
    std::vector<QRectF> out;
    double x = 0.0;
    for (const Part& part : parts_) {
        const double share = room * static_cast<double>(part.words) / static_cast<double>(total);
        out.emplace_back(x, 0, share, height());
        x += share + kGap;
    }
    return out;
}

void SplitBar::paintEvent(QPaintEvent* /*event*/) {
    static constexpr std::array<int, 5> kAlphas = {255, 175, 120, 80, 50};
    const ui::Theme& theme = ui::currentTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (parts_.empty()) {
        rounded(painter, QRectF(rect()), 4, withAlpha(theme.overlayRgb, 24));
    }
    std::size_t shade = 0;
    const std::vector<QRectF> shares = rects();
    for (std::size_t index = 0; index < shares.size(); ++index) {
        QColor colour;
        if (parts_[index].named) {
            colour = withAlpha(theme.accent, kAlphas[std::min(shade, kAlphas.size() - 1)]);
            ++shade;
        } else {
            colour = withAlpha(theme.overlayRgb, 46);
        }
        rounded(painter, shares[index], 3, colour);
    }
}

void SplitBar::mouseMoveEvent(QMouseEvent* event) {
    std::int64_t total = 0;
    for (const Part& part : parts_) {
        total += part.words;
    }
    const std::vector<QRectF> shares = rects();
    const double x = event->position().x();
    for (std::size_t index = 0; index < shares.size(); ++index) {
        if (shares[index].left() - 1 <= x && x <= shares[index].right() + 1) {
            const Part& part = parts_[index];
            QToolTip::showText(event->globalPosition().toPoint(),
                               part.label + q(std::format(" · {} · {}%", sv::words_text(part.words),
                                                          percent(part.words, total))),
                               this);
            return;
        }
    }
    QToolTip::hideText();
}

// ---- ShareBar ----------------------------------------------------------------------------

ShareBar::ShareBar(double fraction, const QString& tone, int height, QWidget* parent)
    : QWidget(parent), tone_(tone) {
    setFixedHeight(height);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFraction(fraction);
}

void ShareBar::setFraction(double fraction) {
    fraction_ = std::max(0.0, std::min(1.0, fraction));
    update();
}

void ShareBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area(rect());
    rounded(painter, area, area.height() / 2, withAlpha(ui::currentTheme().overlayRgb, 24));
    if (fraction_ != 0.0) {
        const QRectF filled(area.left(), area.top(),
                            std::max(area.height(), area.width() * fraction_), area.height());
        QLinearGradient fill(filled.topLeft(), filled.topRight());
        fill.setColorAt(0, toneColor(tone_, 150));
        fill.setColorAt(1, toneColor(tone_, 255));
        rounded(painter, filled, area.height() / 2, QBrush(fill));
    }
}

} // namespace fv
