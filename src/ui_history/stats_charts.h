#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QRectF>
#include <QString>
#include <QWidget>

#include "history/stats_summary.h"

class QPainter;

// ui_qt/widgets/stats_charts.py: the painted charts of the Stats window (columns, heat maps and
// share bars) and their words. Each chart reads the typed theme colours at paint time, so a
// theme switch only needs a repaint, and gives screen readers its numbers through its accessible
// name and description.
namespace fv {

namespace stats_view {

// "1,234": Python's f"{count:,}".
std::string thousands(std::int64_t count);
// words_text: "1 word", "1,234 words".
std::string words_text(std::int64_t count);
// hour_text: "12 AM", "9 AM", "12 PM", "6 PM"; 24 reads as "12 AM".
std::string hour_text(int hour);
// WEEKDAYS: the English names strftime gives, Monday 0. "Monday" (%A) and "Mon" (%a).
std::string weekday_name(int weekday);
std::string weekday_abbr(int weekday);
// The English month names, January 1: "January" (%B) and "Jan" (%b).
std::string month_name(unsigned month);
std::string month_abbr(unsigned month);
// date.weekday() of a day: Monday 0.
int weekday_of(dictation_stats::Day day);

// The quartile thresholds of the days or slots with words, so one huge day doesn't wash out the
// rest: busy[(n - 1) * k // 4] for k = 1, 2, 3 over the sorted non-zero counts; none without any.
std::vector<std::int64_t> quartile_thresholds(const std::vector<std::int64_t>& words);
// level: 0 for no words, else 1 to 4 by the quartile of words.
int level(const std::vector<std::int64_t>& thresholds, std::int64_t words);

// Words by weekday (Monday first), then hour.
using HourWords = std::array<std::array<std::int64_t, 24>, 7>;
// HourGrid.peak: the (weekday, hour) with the most words, ties to the earlier weekday and hour;
// none without words.
std::optional<std::pair<int, int>> grid_peak(const HourWords& grid);

} // namespace stats_view

// stats_charts.Bar (named apart from uimath's Bar): one column, its name read out and shown on
// hover ("Tue, Oct 21"), its words, and the label drawn under it ("" for none).
struct ChartBar {
    QString name;
    std::int64_t words = 0;
    QString axis;
};

// BarChart: words as rounded columns, with one in full colour and a dashed average. tone is a
// colour token ("accent" or "success"); valueText words a column's number for hover and screen
// readers (words_text by default).
// Affinity: GUI thread.
class BarChart : public QWidget {
    Q_OBJECT
public:
    using ValueText = std::function<QString(std::int64_t)>;

    explicit BarChart(int height, const QString& tone = QStringLiteral("accent"),
                      QWidget* parent = nullptr, ValueText valueText = {});

    const std::vector<ChartBar>& bars() const { return bars_; }
    void setBars(std::vector<ChartBar> bars, const QString& name,
                 std::optional<int> highlight = std::nullopt, double average = 0.0);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    int axisHeight() const;
    // Column rects; a few columns stay slim and centred instead of filling the width.
    std::vector<QRectF> slots() const;
    std::optional<int> indexAt(double x) const;

    int plotHeight_;
    QString tone_;
    ValueText valueText_;
    std::vector<ChartBar> bars_;
    std::optional<int> highlight_;
    double average_ = 0.0;
    std::optional<int> hover_;
};

// ActivityMap: a square per day, a column per week, as many weeks as fit; today at the right.
// Rows follow the locale's first weekday.
// Affinity: GUI thread.
class ActivityMap : public QWidget {
    Q_OBJECT
public:
    static constexpr int kWeeks = 53;

    explicit ActivityMap(QWidget* parent = nullptr);

    const std::vector<std::pair<dictation_stats::Day, std::int64_t>>& days() const { return days_; }
    void setDays(std::vector<std::pair<dictation_stats::Day, std::int64_t>> days);
    // 0 for no words, else 1 to 4 by quartile of the days with words.
    int level(std::int64_t words) const { return stats_view::level(thresholds_, words); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    struct Cell {
        QRectF rect;
        dictation_stats::Day day;
        std::int64_t words;
    };

    int gutter() const;
    int row(dictation_stats::Day day) const;
    void paintGrid(QPainter& painter);
    void paintCell(QPainter& painter, const QRectF& cell, int level, bool today) const;
    void paintLegend(QPainter& painter) const;

    int cell_;
    int gap_;
    std::vector<std::pair<dictation_stats::Day, std::int64_t>> days_;
    std::vector<std::int64_t> thresholds_;
    std::vector<Cell> cells_;
    // Monday 0.
    int firstWeekday_;
};

// HourGrid: words by weekday and hour, a row per weekday (in the locale's order), a square per
// hour.
// Affinity: GUI thread.
class HourGrid : public QWidget {
    Q_OBJECT
public:
    explicit HourGrid(QWidget* parent = nullptr);

    const std::optional<stats_view::HourWords>& grid() const { return grid_; }
    // grid is words by weekday, Monday first, then hour.
    void setGrid(const stats_view::HourWords& grid);
    std::optional<std::pair<int, int>> peak() const;
    int level(std::int64_t words) const { return stats_view::level(thresholds_, words); }

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    struct Cell {
        QRectF rect;
        int weekday;
        int hour;
        std::int64_t words;
    };

    int gutter() const;
    double cell() const;
    void fitHeight();

    int gap_ = 2;
    // Unset until setGrid; then every slot is drawn from it.
    std::optional<stats_view::HourWords> grid_;
    std::vector<std::int64_t> thresholds_;
    std::vector<Cell> cells_;
    std::array<int, 7> order_{};
};

// SplitBar: one track cut into shares, the largest named one in full colour and the rest lighter;
// an unnamed share is grey.
// Affinity: GUI thread.
class SplitBar : public QWidget {
    Q_OBJECT
public:
    struct Part {
        QString label;
        std::int64_t words = 0;
        bool named = true;
    };

    explicit SplitBar(QWidget* parent = nullptr);

    const std::vector<Part>& parts() const { return parts_; }
    // Shares without words are left out.
    void setParts(const std::vector<Part>& parts, const QString& name);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
    std::vector<QRectF> rects() const;

    std::vector<Part> parts_;
};

// ShareBar: a thin track filled to a fraction (clamped to 0..1).
// Affinity: GUI thread.
class ShareBar : public QWidget {
    Q_OBJECT
public:
    explicit ShareBar(double fraction, const QString& tone = QStringLiteral("success"),
                      int height = 6, QWidget* parent = nullptr);

    double fraction() const { return fraction_; }
    void setFraction(double fraction);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString tone_;
    double fraction_ = 0.0;
};

} // namespace fv
