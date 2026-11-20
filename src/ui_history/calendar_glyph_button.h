#pragma once

#include <QPushButton>

namespace fv {

class Clock;

// A little calendar page showing today's day of the month (history_calendar.py
// CalendarGlyphButton). A click asks for the History calendar (calendarRequested); the calendar
// dialog itself is M2's.
// Affinity: GUI thread.
class CalendarGlyphButton : public QPushButton {
    Q_OBJECT
public:
    // clock gives "today" in its zone, and must outlive the button.
    explicit CalendarGlyphButton(QWidget* parent = nullptr);
    explicit CalendarGlyphButton(const Clock& clock, QWidget* parent = nullptr);

    // Today's day of the month in the clock's zone.
    int dayOfMonth() const;

Q_SIGNALS:
    void calendarRequested();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    const Clock& clock_;
};

} // namespace fv
