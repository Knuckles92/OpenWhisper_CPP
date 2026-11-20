#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QColor>
#include <QDate>
#include <QFont>
#include <QFrame>
#include <QList>
#include <QPainterPath>
#include <QPixmap>
#include <QPointF>
#include <QPushButton>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include "history/calendar.h"
#include "pyport/datetime.h"

class QLabel;
class QPainter;
class QVariantAnimation;

namespace fv {

class Clock;
class WrappedLabel;

// The pieces of the History calendar (ui_qt/widgets/history_calendar.py): month grid, year strip,
// month title and picker, kind chips and agenda cards. The grid and the picker paint their cells
// themselves; colours are read from the palette at paint time, so a theme change needs only a
// repaint, and every painted size follows the UI font scale (calendarPx).
// Affinity: GUI thread (the free text helpers are pure and thread-safe).

// _px: a designed pixel size at the current UI font scale, max(1, round(size * scale)) with
// Python's half-to-even round.
int calendarPx(double size);
// _font: base at calendarPx(size) pixels and the given weight.
QFont calendarFont(const QFont& base, double size, QFont::Weight weight = QFont::Normal);

// QDate and history::Day are the same local day.
QDate toQDate(history::Day day);
history::Day toDay(const QDate& date);
// Today in the clock's zone.
history::Day localToday(const Clock& clock);

namespace calendar_view {

// Palette roles per kind: fills and dots ("accent", "purple", "warning"), type on the surface
// ("accent-soft", "purple-text", "warning-text"); "" for another kind.
std::string_view kind_color(std::string_view kind);
std::string_view kind_text(std::string_view kind);

// English names, as Python's calendar module and strftime give them in the C locale.
// month is 1..12; weekday is Python's (0 is Monday).
std::string_view month_name(int month);
std::string_view month_abbr(int month);
std::string_view day_name(int weekday);
std::string_view day_abbr(int weekday);
// Python's date.weekday() of a day: 0 is Monday.
int weekday(history::Day day);

// "1,234": f"{number:,}".
std::string grouped(std::int64_t number);
// "9:02 AM": strftime("%I:%M %p") without the leading zero.
std::string clock(const py::DateTime& when);
// "3 recordings", "1 dictation": many defaults to one + "s".
std::string plural(std::int64_t count, std::string_view one,
                   std::optional<std::string_view> many = std::nullopt);
// "4 dictations · 1 file · 1 meeting", in KINDS order, kinds with no count left out.
std::string kind_breakdown(const std::map<std::string, std::int64_t, std::less<>>& by_kind);
// The indexes of the `capacity` records a crowded day cell shows, in time order: each kind
// gets a line before any kind gets a second, so a day of twenty dictations and one meeting
// still shows the meeting. kinds are the records' kinds in time order.
std::vector<std::size_t> pick_line_indexes(const std::vector<std::string_view>& kinds,
                                           std::int64_t capacity);
// pick_lines: the same, for calendar items.
std::vector<const history::CalendarItem*>
pick_lines(const std::vector<history::CalendarItem>& items, std::int64_t capacity);
// "Sunday, September 27" and what the day holds: "Nothing recorded", or the count with its
// audio, the kinds when more than one, and the milestone star.
std::string day_tooltip(history::Day day, const history::DaySummary* summary,
                        std::optional<std::int64_t> milestone);

// _heat: the picker's tint for count recordings, brighter as it nears peak; gentler than the
// grid's days, so twelve neighbouring tiles of a steady year do not read as one block.
QColor heat(std::int64_t count, std::int64_t peak);
// _rounded and _mix.
QPainterPath rounded(const QRectF& rect, double radius);
QColor mix(const QColor& a, const QColor& b, double amount);

enum class Pointing { Left, Right, Up, Down };
// A thin open chevron.
void paint_chevron(QPainter& painter, const QPointF& center, double size, Pointing pointing,
                   const QColor& color, double width = 1.6);

} // namespace calendar_view

// What the grid shows of a month: each day's counts, its records in time order, and the days
// holding a milestone with the largest on each.
struct MonthDetails {
    std::map<history::Day, history::DaySummary> days;
    std::map<history::Day, std::vector<history::CalendarItem>> items;
    std::map<history::Day, std::int64_t> milestones;
};

// One month of days, painted (MonthGrid): heat for how much was recorded, a line per recording,
// a bar for the mix of kinds. daySelected follows a click or an arrow key (the day may belong
// to a neighbouring month), dayActivated a double click or Enter, monthStep Page Up or Down,
// todayRequested T or Home, pickerRequested M. The selection ring glides (190 ms), hover eases
// in (130 ms), a new month slides in from the side it came from (280 ms) and today pulses
// (1100 ms).
//
// Subclasses (the backup calendar) paint their own days by overriding paintCell, and may
// extend event().
class MonthGrid : public QWidget {
    Q_OBJECT
public:
    static constexpr int kGap = 6;
    static constexpr int kRadius = 10;

    // Today and the first month shown come from the clock (the system's by default), which must
    // outlive the grid.
    explicit MonthGrid(QWidget* parent = nullptr);
    explicit MonthGrid(const Clock& clock, QWidget* parent = nullptr);

    history::MonthKey month() const { return {year_, month_}; }
    const std::vector<std::vector<history::Day>>& weeks() const { return weeks_; }
    int firstWeekday() const { return firstWeekday_; }
    history::Day today() const { return today_; }
    const MonthDetails& details() const { return details_; }

    // Shows a month. A non-zero direction slides it in (1: from the right) when it changed and
    // the grid is showing.
    void setMonth(int year, int month, MonthDetails details, history::Day today,
                  int firstWeekday = history::SUNDAY, int direction = 0);
    // New counts or lines for the month already shown.
    void setDetails(MonthDetails details);
    void setSelected(std::optional<history::Day> day, bool animate = true);
    std::optional<history::Day> selected() const { return selected_; }
    // A ring spreads once from today's date, to show where today is.
    void pulseToday();
    // The pulse's progress (0..1) while it runs.
    std::optional<double> pulse() const { return pulse_; }

    std::optional<history::Day> dayAt(const QPointF& pos) const;
    QRectF cellRect(int row, int column) const;
    // The ring's rectangle now, or null.
    std::optional<QRectF> selectionRect() const { return selectionRect_; }
    // Drives the running animations (selection, hover, slide, pulse) to their end values.
    void finishAnimations();

Q_SIGNALS:
    void daySelected(const QDate& day);
    void dayActivated(const QDate& day);
    void monthStep(int step);
    void todayRequested();
    void pickerRequested();

protected:
    // _paint_cell: one day's cell, for every day of the shown weeks.
    virtual void paintCell(QPainter& painter, const QRectF& rect, const QDate& day);

    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    int headerHeight() const;
    QRectF gridRect() const;
    QRectF numberRect(const QRectF& rect) const;
    std::optional<QRectF> dayRect(std::optional<history::Day> day) const;

private:
    void syncSelection(bool animate);
    void paintWeekdays(QPainter& painter);
    void paintCells(QPainter& painter);
    void paintLines(QPainter& painter, const QRectF& rect, double top,
                    const std::vector<history::CalendarItem>& items);
    void paintDots(QPainter& painter, const QRectF& area,
                   const std::vector<history::CalendarItem>& items);
    void paintKindBar(QPainter& painter, const QRectF& rect, const history::DaySummary& summary);
    void paintSelection(QPainter& painter);
    void paintPulse(QPainter& painter);

    int year_;
    int month_;
    int firstWeekday_ = history::SUNDAY;
    std::vector<std::vector<history::Day>> weeks_;
    MonthDetails details_;
    history::Day today_;
    std::int64_t peak_ = 1;
    std::optional<history::Day> selected_;
    std::optional<history::Day> hover_;

    std::optional<QRectF> selectionRect_;
    QVariantAnimation* selectionAnim_;
    double hoverLevel_ = 1.0;
    QVariantAnimation* hoverAnim_;
    std::optional<QPixmap> slideFrom_;
    int slideDirection_ = 0;
    double slideProgress_ = 1.0;
    QVariantAnimation* slideAnim_;
    std::optional<double> pulse_;
    QVariantAnimation* pulseAnim_;
};

// A bar per month for the year around the one shown (YearStrip); a click on one goes there.
// Bars grow in the first time counts arrive and ease to new heights (520 ms) when a filter or
// the window of months changes.
class YearStrip : public QWidget {
    Q_OBJECT
public:
    explicit YearStrip(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    void setData(std::vector<history::MonthKey> months,
                 std::map<history::MonthKey, std::int64_t> counts, history::MonthKey current,
                 history::MonthKey today);
    const std::vector<history::MonthKey>& months() const { return months_; }
    // Drives the grow animation to its end.
    void finishAnimations();

Q_SIGNALS:
    void monthClicked(int year, int month);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    std::vector<double> heights() const;
    QRectF slot(int index) const;
    std::optional<int> indexAt(const QPointF& pos) const;

    std::vector<history::MonthKey> months_;
    std::map<history::MonthKey, std::int64_t> counts_;
    std::optional<history::MonthKey> current_;
    std::optional<history::MonthKey> today_;
    std::vector<double> from_;
    std::vector<double> target_;
    double progress_ = 1.0;
    std::optional<int> hover_;
    QVariantAnimation* grow_;
};

// The month's name, which opens the month picker (MonthTitleButton). It reads as a heading
// until the pointer finds it: a faint chevron says it opens something, and a soft pill eases in
// on hover (150 ms) and stays while the picker is open. Given a short form with setTitle, it
// shows that when the full one does not fit.
class MonthTitleButton : public QPushButton {
    Q_OBJECT
public:
    explicit MonthTitleButton(const QString& text = QString(), QWidget* parent = nullptr);

    void setTitle(const QString& full, const QString& shortTitle);
    void setOpen(bool open);
    bool isOpen() const { return open_; }

    // From the stored titles, not the one showing, so swapping them never changes what the
    // layout asks for.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QSize sizeFor(const QString& text) const;
    void fitTitle();
    void liftTo(double value);

    QString full_;
    QString short_;
    bool open_ = false;
    double lift_ = 0.0;
    QVariantAnimation* liftAnim_;
};

// A popup for leaping to any month (MonthPicker): a year's months tinted by how much each
// holds, or, from the year between the arrows, the years themselves newest first, each with a
// bar per month. Arrow keys move, Page Up and Page Down (or the wheel) change the year, Y
// switches between months and years, Enter picks, T picks this month, Escape steps back out.
// Picking a month emits monthChosen; closed follows every close.
class MonthPicker : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Months, Years };
    // Years on one page of the years view; fewer share at least YEAR_ROWS_MIN rows' height.
    static constexpr int kYearPage = 4;
    static constexpr int kYearRowsMin = 3;
    // Room around the panel for its painted shadow; it falls a little lower.
    static constexpr int kShadow = 16;
    static constexpr int kDrop = 5;
    static constexpr int kRadius = 14;

    // Today's month comes from the clock (the system's by default), which must outlive it.
    explicit MonthPicker(QWidget* parent = nullptr);
    MonthPicker(const Clock& clock, QWidget* parent);

    // Recordings per month, the month the calendar shows, and this month.
    void setData(const std::map<history::MonthKey, std::int64_t>& counts, history::MonthKey current,
                 history::MonthKey today);
    const std::map<history::MonthKey, std::int64_t>& counts() const { return counts_; }

    Mode mode() const { return mode_; }
    int year() const { return year_; }
    int firstYear() const;
    int lastYear() const;
    // The years on the page shown, newest first.
    std::vector<int> pageYears() const;
    // The tile under the pointer or the arrow keys: a month index or a page row.
    std::optional<int> focusIndex() const { return focus_; }
    void setFocusIndex(std::optional<int> index);

    // Opens under anchor on the year shown, with its month in focus.
    void popup(QWidget* anchor);
    // Back (-1) or on (1) a year, or a page of years; false at either end.
    bool step(int step);
    // Between a year's months and every year.
    void toggleYears();
    // What a click on a tile does: choose its month, or open its year.
    void pick(int index);
    QPoint tileCenter(int index) const;
    QSize sizeHint() const override;
    // Drives the open and change animations to their ends.
    void finishAnimations();

Q_SIGNALS:
    void monthChosen(int year, int month);
    void closed();

protected:
    bool event(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class Part { Prev, Next, Title, Today, Tile };
    struct Target {
        Part part;
        int index = 0;
        friend bool operator==(const Target&, const Target&) = default;
    };

    bool manyYears() const;
    std::int64_t yearTotal(int year) const;
    int tileCount() const;
    bool canStep(int step) const;
    QPoint placeBy(QWidget* anchor) const;
    void choose(history::MonthKey key);
    void openYear(int year);
    void moveFocus(int step);
    void act(const Target& target);
    void beginChange(int kind);

    int pad() const;
    int gap() const;
    QSize tilesSize() const;
    QRectF panel() const;
    QRectF headerRect() const;
    QRectF prevRect() const;
    QRectF nextRect() const;
    QString titleText() const;
    QFont titleFont() const;
    QRectF titleRect() const;
    QRectF tilesRect() const;
    QRectF tileRect(int index) const;
    QRectF footerRect() const;
    QRectF todayRect() const;
    QFont footerFont() const;
    std::optional<Target> targetAt(const QPointF& pos) const;
    QString tooltip(const Target& target) const;

    void paintShadow(QPainter& painter, const QRectF& panel);
    void paintHeader(QPainter& painter);
    void paintTilesArea(QPainter& painter);
    void paintTiles(QPainter& painter);
    void paintTileState(QPainter& painter, const QRectF& rect, bool current, bool focused);
    void paintMonth(QPainter& painter, const QRectF& rect, history::MonthKey key, std::int64_t peak,
                    bool focused);
    void paintYear(QPainter& painter, const QRectF& rect, int year, std::int64_t peak,
                   bool focused);
    void paintFooter(QPainter& painter);

    QWidget* anchor_ = nullptr;
    std::map<history::MonthKey, std::int64_t> counts_;
    history::MonthKey current_;
    history::MonthKey today_;
    Mode mode_ = Mode::Months;
    int year_;
    // The newest year on the years page shown.
    int pageEnd_;
    std::optional<int> focus_;
    std::optional<Target> hover_;
    std::optional<Target> pressed_;
    int wheel_ = 0;

    double shown_ = 1.0;
    QVariantAnimation* openAnim_;
    // The tiles as they were, sliding or zooming away under the new ones.
    std::optional<QPixmap> changeFrom_;
    // -1 or 1: the new tiles come from that side; 2 zooms in, -2 out.
    int changeKind_ = 0;
    double change_ = 1.0;
    QVariantAnimation* changeAnim_;
};

// A legend entry that doubles as a filter (KindChip): its kind's colour dot, name and count.
class KindChip : public QPushButton {
    Q_OBJECT
public:
    explicit KindChip(std::string kind, QWidget* parent = nullptr);

    const std::string& kind() const { return kind_; }
    // "Dictations", or "Dictations  3" with a count.
    void setCount(std::optional<std::int64_t> count);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::string kind_;
};

// One recording in the day agenda (AgendaCard): time, kind, where it is kept, how long, and
// what was said. A click opens it; its menu opens or copies it.
class AgendaCard : public QFrame {
    Q_OBJECT
public:
    explicit AgendaCard(history::CalendarItem item,
                        std::optional<std::int64_t> milestone = std::nullopt,
                        QWidget* parent = nullptr);

    const history::CalendarItem& item() const { return item_; }
    QLabel* timeLabel() const { return timeLabel_; }
    QLabel* kindPill() const { return kindPill_; }
    WrappedLabel* titleLabel() const { return titleLabel_; }
    WrappedLabel* previewLabel() const { return previewLabel_; }
    QLabel* progressLabel() const { return progressLabel_; }
    void setProgress(const QString& text);
    // The right-click menu's choices, as its "Open" and "Copy text" / "Copy transcript" do.
    void open();
    void copy();

Q_SIGNALS:
    void activated(const QString& recordId);
    void copyRequested(const QString& recordId);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void showMenu(const QPoint& pos);

    history::CalendarItem item_;
    QLabel* timeLabel_ = nullptr;
    QLabel* kindPill_ = nullptr;
    WrappedLabel* titleLabel_ = nullptr;
    WrappedLabel* previewLabel_ = nullptr;
    QLabel* progressLabel_ = nullptr;
};

// fade_in: fades the first eight widgets in one after another (stagger_ms apart, over
// duration_ms, OutCubic); each one's effect is removed when it is done.
void fadeIn(const QList<QWidget*>& widgets, int staggerMs = 35, int durationMs = 220);
// Ends a fadeIn at once: the widget fully shown, without its effect.
void finishFadeIn(QWidget* widget);

} // namespace fv
