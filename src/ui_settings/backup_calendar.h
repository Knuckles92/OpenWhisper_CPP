#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <QWidget>

#include "base/clock.h"
#include "history/calendar.h"
#include "ui_history/history_calendar.h"

class QLabel;
class QToolButton;

namespace fv {

class WrappedLabel;

// ui_qt/widgets/backup_calendar.py: a month calendar of the latest backup and the saved
// automatic schedule. It is the History calendar's month grid, title button and month picker
// with backups in place of recordings.
namespace backup_calendar {

// LATEST_TEXT and PLANNED_TEXT.
inline constexpr std::string_view kLatestText = "Latest backup saved";
inline constexpr std::string_view kPlannedText = "Automatic backup planned";
// PICKER_HORIZON_DAYS: how far ahead the month picker marks planned backups.
inline constexpr int kPickerHorizonDays = 366;

// BackupMonthGrid.is_planned: a day is planned when it is `next_due` or a whole number of
// `interval_days` after it.
bool is_planned(history::Day day, std::optional<history::Day> next_due, int interval_days);

// _month_counts: the backups of each month for the picker's tint: the latest, and the planned
// ones up to kPickerHorizonDays after `today`.
std::map<history::MonthKey, std::int64_t> month_counts(std::optional<history::Day> latest,
                                                       std::optional<history::Day> next_due,
                                                       int interval_days, history::Day today);

} // namespace backup_calendar

// A month whose days can carry the latest backup ("Saved") and planned ones ("Planned"); a cell
// too small for words shows a dot per event. Its tooltip lists what the day holds.
// Affinity: GUI thread.
class BackupMonthGrid : public MonthGrid {
    Q_OBJECT
public:
    explicit BackupMonthGrid(const Clock& clock, QWidget* parent = nullptr);

    void setDates(std::optional<history::Day> latest, std::optional<history::Day> nextDue,
                  int intervalDays);
    const std::optional<history::Day>& latest() const { return latest_; }
    const std::optional<history::Day>& nextDue() const { return nextDue_; }
    int intervalDays() const { return intervalDays_; }

    bool isPlanned(history::Day day) const;
    // (short label, long description) for what the day holds, the latest backup first.
    std::vector<std::pair<std::string_view, std::string_view>> events(history::Day day) const;

protected:
    void paintCell(QPainter& painter, const QRectF& rect, const QDate& date) override;
    bool event(QEvent* event) override;

private:
    void paintLabels(QPainter& painter, const QRectF& rect, bool latest, bool planned);

    std::optional<history::Day> latest_;
    std::optional<history::Day> nextDue_;
    int intervalDays_ = 0;
};

// Month grid with a title that opens the month picker, as in History, a legend and the selected
// day's line. Months slide in from the side they come from; Today (T), the arrows (Page Up and
// Page Down on the grid) and the picker move between them. Today and the first month shown come
// from the clock, which must outlive the widget.
// Affinity: GUI thread.
class BackupCalendar : public QWidget {
    Q_OBJECT
public:
    explicit BackupCalendar(const Clock& clock = system_clock(), QWidget* parent = nullptr);

    history::MonthKey view() const { return view_; }
    history::Day selectedDate() const { return selected_; }
    bool isPlanned(history::Day day) const { return grid_->isPlanned(day); }

    // The latest backup's day, the next planned day and the days between planned ones (0 when
    // none is planned).
    void setDates(std::optional<history::Day> latest, std::optional<history::Day> nextDue,
                  int intervalDays);

    // Shows a month; `selected` becomes the selected day when it falls in it, else today when
    // today does, else the first.
    void showMonth(history::MonthKey key, std::optional<history::Day> selected = std::nullopt);
    void stepMonth(int step);
    void selectDay(history::Day day);
    void goToday();
    void openMonthPicker();

    BackupMonthGrid* grid() const { return grid_; }
    MonthPicker* monthPicker() const { return picker_; }
    MonthTitleButton* titleButton() const { return title_; }
    QToolButton* todayButton() const { return today_; }
    QToolButton* previousButton() const { return previous_; }
    QToolButton* nextButton() const { return next_; }
    WrappedLabel* detailLabel() const { return detail_; }

private:
    QToolButton* navButton(const QString& text, const QString& tip, const char* name);
    void render(int direction);
    void refreshPicker();
    void updateDetail();

    const Clock& clock_;
    history::MonthKey view_;
    history::Day selected_;
    std::optional<history::Day> latest_;
    std::optional<history::Day> nextDue_;
    int intervalDays_ = 0;

    MonthTitleButton* title_ = nullptr;
    QToolButton* today_ = nullptr;
    QToolButton* previous_ = nullptr;
    QToolButton* next_ = nullptr;
    BackupMonthGrid* grid_ = nullptr;
    MonthPicker* picker_ = nullptr;
    WrappedLabel* detail_ = nullptr;
};

} // namespace fv
