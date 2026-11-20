#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include "ui_walkthrough/walkthrough_column.h"
#include "ui_widgets/drag_move.h"

class QPainter;

namespace fv {

class Clock;
class ScreenshotFlight;
class SnapMenu;

// The column at the side of the screen that writes a walkthrough as it records
// (ui_qt/widgets/walkthrough_panel.py). The voice shows as rows of ticks, like lines of
// writing, and each screenshot kept drops in below them, numbered; the section being spoken
// sits in an open slot until the next screenshot closes it. Hovering a screenshot offers to
// move it up or down or remove it, with its words. The footer takes screenshots (its menu picks
// the kind Snap repeats), holds notes for the AI write-up, undoes the last screenshot, pauses,
// cancels and finishes. At Done the ticks close up into lines while the document is written,
// the header says what it came to, and the column fades once it is ready.
//
// The column never takes focus, so the app being shown keeps the keyboard; it can be dragged
// aside by anything that is not a button, and each walkthrough opens it at the screen's edge
// again. Everything is painted: hit-testing uses the rectangles the last frame drew. Time comes
// from the injected clock (its steady time stands for Python's time.monotonic() and for the
// recording's QElapsedTimer), and the 16 ms frame timer calls advanceFrame().
// Affinity: GUI thread.
class WalkthroughPanel : public QWidget {
    Q_OBJECT

public:
    enum class State { Recording, Writing, Done, Hidden };

    // A footer button or a screenshot's hover tool: where the last frame drew it, and whether
    // it takes a click now.
    struct Control {
        QString key;
        QRectF rect;
        bool enabled = false;
    };

    // clock must outlive the panel.
    explicit WalkthroughPanel(const Clock& clock, QWidget* parent = nullptr);
    ~WalkthroughPanel() override;

    // ---- what the walkthrough does ----

    // A walkthrough started recording: an empty column on the pointer's screen.
    void begin();
    void hear(double level);
    // Screenshot number (as kept) joins the column as a thumbnail, keptAtS into the recording.
    // With sourceRect (global) and the column on screen, the picture flies there first.
    void addShot(int number, const QImage& image, double keptAtS = 0.0,
                 std::optional<QRect> sourceRect = std::nullopt);
    // Lands a flying screenshot: before the next capture, or when the walkthrough closes.
    void endCaptureAnimation();
    // Screenshot number (as kept) left the walkthrough; its words stay.
    void removeShot(int number);
    // Screenshot number (as kept) moved step sections, with its words.
    void moveShot(int number, int step);
    // Whether the column is on screen showing the walkthrough, recording or being written: the
    // recording overlay would only repeat it.
    bool showsRecording() const;
    // What Snap takes, and the keys of the Screenshot shortcut, which repeats it; without
    // shortcut the keys stay as they were.
    void setSnapMode(const QString& mode);
    void setSnapMode(const QString& mode, const QString& shortcut);
    void setAutoMode(bool enabled);
    // Seconds until an "In 3 seconds" snap, shown on Snap; 0 when none is coming.
    void setCountdown(int seconds);
    // The recording paused, or resumed: the clock stops with it.
    void setPaused(bool paused);
    // Whether Hold for note works: only an AI write-up reads notes.
    void setNotesOn(bool on);
    // The kinds of screenshot this desktop can take: WalkthroughSnapMode values, "window" only
    // where the window under the pointer can be found.
    QStringList snapModes() const;
    // The recording stopped: close the ticks into lines while it is written.
    void finish();
    // The document is ready: say what it came to, then fade.
    void complete(int shots, double durationS);
    // Canceled or failed: fade at once.
    void dismiss();

    State state() const { return state_; }
    bool paused() const { return paused_; }
    const QString& snapMode() const { return snapMode_; }

    // ---- what it shows (OpenWhisper's tests read these) ----

    const ui_walkthrough::WalkthroughColumn& column() const { return column_; }
    // OpenWhisper's column attribute is public: the reference builder writes ticks into it.
    ui_walkthrough::WalkthroughColumn& column() { return column_; }
    const QString& summary() const { return summary_; }
    int countdown() const { return countdown_; }
    double scroll() const { return scroll_; }
    int hoverShot() const { return hoverShot_; }
    const QString& hovered() const { return hover_; }
    bool notesOn() const { return notesOn_; }
    bool autoMode() const { return autoMode_; }
    SnapMenu* menu() const;
    ScreenshotFlight* flight() const { return flight_; }
    bool framesRunning() const { return frame_.isActive(); }
    // Stops the frame timer, as OpenWhisper's fixtures do before closing.
    void stopFrames() { frame_.stop(); }

    QRectF card() const;
    QRectF inner() const;
    // Snap, its menu, Hold for note, pause, undo, Cancel and Done, in that order.
    std::vector<Control> footer() const;
    Control footerControl(const QString& key) const;
    // Screenshot number's tools, "remove:N", "down:N", "up:N"; it must have been drawn.
    std::vector<Control> shotTools(int number) const;
    // Where the last frame drew each screenshot, by number as kept, in the order drawn.
    const std::vector<std::pair<int, QRectF>>& shotRects() const { return shotRects_; }
    std::optional<QRectF> shotRect(int number) const;
    // The control under position that takes a click now, or "".
    QString hit(QPointF position) const;
    // The pointer is at position (nullopt: it left the column).
    void setHover(std::optional<QPointF> position);
    // Starts or ends a note; the walkthrough hears of it through noteHeld.
    void hold(bool on);
    // Seconds recorded so far.
    double recorded() const;
    // One frame: the flight, the ticks, the fade.
    void advanceFrame();

Q_SIGNALS:
    // The footer's buttons, while it records (Cancel also while it is written).
    void snapRequested();
    // A pick from the Snap menu: a WalkthroughSnapMode, to take now and repeat.
    void snapModeChosen(const QString& mode);
    // Hold for note pressed (true) or let go (false).
    void noteHeld(bool held);
    // Pause (true) or resume (false).
    void pauseRequested(bool pause);
    void undoRequested();
    void cancelRequested();
    void finishRequested();
    // A screenshot's hover tools: its number as kept, and -1 (up) or 1 (down).
    void moveRequested(int number, int step);
    void removeRequested(int number);

protected:
    bool event(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    enum class Kind { Words, Note, Shot, Slot };
    // A block of the body: its top and height from the body's top. Words and notes point at a
    // Paragraph, a shot at a ShotItem; the slot holds the section being spoken.
    struct Block {
        Kind kind = Kind::Words;
        const ui_walkthrough::Item* item = nullptr;
        std::vector<const ui_walkthrough::Paragraph*> open;
        double top = 0.0;
        double height = 0.0;
    };

    double now() const;
    std::int64_t elapsedMs() const;
    void enter(State state);
    double since() const;
    void place();
    std::vector<Control> controls() const;
    void settleHover();
    void click(const QString& key);
    void openMenu();
    void closeMenu();
    std::optional<QRectF> flightTarget(int number) const;

    double footerTop() const;
    QRectF bodyRect() const;
    double cancelWidening() const;
    double slotTicksWidth(double width) const;
    std::vector<Block> layout(double width) const;
    std::pair<double, double> shotSize(const ui_walkthrough::ShotItem& shot, double width) const;
    double noteLength(const ui_walkthrough::Paragraph& note) const;
    double sectionStarted() const;

    void paintHeader(QPainter& painter, const QRectF& inner);
    void paintBody(QPainter& painter, const QRectF& body);
    void paintTicks(QPainter& painter, const ui_walkthrough::Paragraph& paragraph, double left,
                    double top, double width, double now, double morph, const QColor& color);
    void paintNote(QPainter& painter, const ui_walkthrough::Paragraph& note, const QRectF& rect,
                   double now, double morph);
    void paintCaret(QPainter& painter, double x, double centre);
    void paintSlot(QPainter& painter, const std::vector<const ui_walkthrough::Paragraph*>& items,
                   const QRectF& rect, double now);
    void paintShot(QPainter& painter, const ui_walkthrough::ShotItem& shot, double left, double top,
                   double width, double now);
    void paintBadge(QPainter& painter, const ui_walkthrough::ShotItem& shot, const QRectF& rect);
    void paintShotTools(QPainter& painter, int number);
    void paintFooter(QPainter& painter);
    void paintButton(QPainter& painter, const QString& key, const QRectF& rect);
    void paintLabel(QPainter& painter, const QRectF& rect, std::string_view icon,
                    const QString& text, const QColor& color);

    const Clock& clock_;
    DragToMove drag_;
    State state_ = State::Hidden;
    ui_walkthrough::WalkthroughColumn column_;
    // When the recording's elapsed-time clock started (QElapsedTimer::start).
    std::chrono::steady_clock::time_point clockStart_;
    double recordedS_ = 0.0;
    QString summary_;
    double stateAt_ = 0.0;
    double last_ = 0.0;
    double scroll_ = 0.0;
    // How far the user scrolled back from the newest, in pixels.
    double scrolledBack_ = 0.0;
    double opacity_ = 0.0;
    // The control under the pointer and the one being pressed: "snap", "undo", "cancel",
    // "done", or a screenshot's "up:N", "down:N", "remove:N".
    QString hover_;
    QString pressed_;
    // The screenshot under the pointer (its number as kept), or 0.
    int hoverShot_ = 0;
    std::vector<std::pair<int, QRectF>> shotRects_;
    ScreenshotFlight* flight_ = nullptr;
    // What Snap takes, the Screenshot shortcut's keys, and the seconds left before an "In 3
    // seconds" snap (0: none counting down).
    QString snapMode_;
    bool autoMode_ = false;
    QString snapShortcut_;
    // Whether a note has anyone to read it: the AI write-up is on.
    bool notesOn_ = true;
    // Paused, and how long the pauses so far lasted, in ms of the recording's clock.
    bool paused_ = false;
    std::int64_t pausedMs_ = 0;
    std::int64_t pausedAt_ = 0;
    int countdown_ = 0;
    QPointer<SnapMenu> menu_;
    QTimer frame_;
};

// A column (make_panel). Desktops whose compositor places windows (Wayland) get none in
// OpenWhisper; FioriVoce runs on Windows only, where there always is one.
std::unique_ptr<WalkthroughPanel> makePanel(const Clock& clock);

} // namespace fv
