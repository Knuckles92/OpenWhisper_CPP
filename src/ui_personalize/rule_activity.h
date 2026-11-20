#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string_view>
#include <vector>

#include <QColor>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QWidget>

#include "ui_personalize/detail/elapsed_timer.h"

class QFrame;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPropertyAnimation;
class QTimer;
class QVariantAnimation;

namespace fv {

class Clock;
class ElidingLabel;
namespace detail {
class ActivityGlyph;
} // namespace detail

// The step the learned-rule composer has in flight (rule_activity.py IDLE, LISTENING,
// TRANSCRIBING, POLISHING).
enum class RuleActivityState { Idle, Listening, Transcribing, Polishing };

// The names the panel's `state` property carries and the theme selects on
// (QFrame#ruleActivityStrip[state="listening"]).
std::string_view ruleActivityStateName(RuleActivityState state);

// "m:ss" of a whole number of seconds (_format_clock).
QString formatRuleClock(int seconds);

// Blends two colours, t clamped to 0..1 over each channel rounded half to even, with the given
// alpha clamped to 0..1 (_mix).
QColor mixColors(const QColor& start, const QColor& end, double t, double alpha);

// Scrolling bars of the microphone level; a sweep once capture ends. While listening each 60 ms
// step banks one bar holding the loudest level heard during it, and the bars slide left
// continuously between steps. When the microphone closes the bars hold still and a highlight
// passes over them while the clip is worked on.
//
// Time comes from `clock` (the system clock unless given), which a test moves by hand.
// Affinity: GUI thread.
class LevelTrace : public QWidget {
    Q_OBJECT
public:
    // One bar per step, about sixteen a second: slow enough to read as speech.
    static constexpr double kStepMs = 60.0;
    static constexpr double kBarWidth = 3.0;
    static constexpr double kBarPitch = 5.0;
    // How far each tick moves a shown height toward its target.
    static constexpr double kEase = 0.35;
    static constexpr int kSweepMs = 1300;
    static constexpr std::size_t kMaxBars = 480;

    explicit LevelTrace(QWidget* parent = nullptr, const Clock* clock = nullptr);

    QSize sizeHint() const override;

    // Target heights of the banked bars (0-1), oldest first.
    std::vector<double> barHeights() const;
    // The heights as drawn (eased toward the targets), oldest first.
    std::vector<double> shownHeights() const;
    RuleActivityState state() const { return state_; }
    // The loudest level heard in the step in flight.
    double peak() const { return peak_; }

    void startListening();
    // Takes one level from the recorder; the loudest in each step wins. Ignored unless
    // listening.
    void pushLevel(double level);
    // Stops scrolling and sweeps the captured bars while `state` runs.
    void hold(RuleActivityState state);
    void clear();
    // One animation tick: banks the steps that ended and eases the bars.
    void advance();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    RuleActivityState state_ = RuleActivityState::Idle;
    // {target, shown} heights in 0-1, oldest first.
    std::deque<std::array<double, 2>> bars_;
    double peak_ = 0.0;
    std::int64_t steps_ = 0;
    detail::ElapsedTimer clock_;
};

// Names the composer's step in flight and where that step runs. Hidden while idle. It opens
// under the input when dictation starts, and after a short delay when a step that may fail at
// once starts, and closes when the step ends. The panel keeps its full height and this host
// reveals it by clipping, so it slides open instead of being squashed.
// Affinity: GUI thread.
class RuleActivityStrip : public QWidget {
    Q_OBJECT
public:
    // Space above the panel. It lives inside the animated height, so the tile grows smoothly
    // instead of jumping by a layout spacing.
    static constexpr int kGap = 8;
    // Work that ends this fast (a missing API key fails at once) never opens the strip, so it
    // does not flash open and shut behind the review dialog.
    static constexpr int kWorkRevealDelayMs = 150;
    // The clock turns amber this close to the automatic stop.
    static constexpr int kCapWarningSeconds = 10;
    static constexpr int kTickMs = 16;

    // capSeconds: the length of the dictation's automatic stop, shown beside the running clock
    // (at least 1). clock: the system clock unless given; it must outlive the strip.
    explicit RuleActivityStrip(int capSeconds, QWidget* parent = nullptr,
                               const Clock* clock = nullptr);
    ~RuleActivityStrip() override;

    RuleActivityState state() const { return state_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Opens at once: the microphone is live from this moment. `detail` is the input device.
    void showListening(const QString& detail);
    // The dictation engine has the clip; `detail` says where it runs. Opens after 150 ms.
    void showTranscribing(const QString& detail);
    // The cleanup model is rewriting the words; `detail` names it. Opens after 150 ms.
    void showPolishing(const QString& detail);
    void pushLevel(double level);
    // Closes the strip; its last frame holds still while it slides shut.
    void finish(bool animate = true);
    // One animation frame (the 16 ms timer's): eases the trace, redraws the glyph and, while
    // listening, updates the clock.
    void tick();
    // Shows the time listened beside the cap ("0:12 / 1:00") and turns it amber within 10 s of
    // the cap (nearCap); tick() does it while listening.
    void updateClock();

    QFrame* panel() const { return panel_; }
    QLabel* titleLabel() const { return title_; }
    ElidingLabel* detailLabel() const { return detail_; }
    QLabel* timeLabel() const { return time_; }
    LevelTrace* trace() const { return trace_; }
    QPropertyAnimation* heightAnimation() const { return heightAnim_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void showWork(RuleActivityState state, const QString& title, const QString& detail);
    void enter(RuleActivityState state, const QString& title, const QString& detail);
    void open();
    void onOpened();
    void onClosed();
    void reset();
    void syncTimer();
    void repolishClock(bool nearStop);
    void placePanel();

    int capSeconds_;
    RuleActivityState state_ = RuleActivityState::Idle;
    bool closing_ = false;
    bool nearCap_ = false;
    detail::ElapsedTimer listenClock_;

    QFrame* panel_;
    detail::ActivityGlyph* glyph_;
    QLabel* title_;
    ElidingLabel* detail_;
    LevelTrace* trace_;
    QLabel* time_;
    QTimer* timer_;
    QTimer* revealTimer_;
    QPropertyAnimation* heightAnim_;
};

// Washes one list row in the accent colour and lets it fade. Marks a rule the moment it lands
// in the library. The wash is painted on a transparent layer over the view's viewport, so the
// list's own item styling is left alone.
// Affinity: GUI thread.
class ItemGlow : public QWidget {
    Q_OBJECT
public:
    static constexpr int kGlowMs = 1600;
    // Matches the rule rows' radius in theme.qss.
    static constexpr double kRadius = 9.0;

    // Draws over view's viewport; it goes with the view.
    explicit ItemGlow(QListWidget* view);

    // 1 right after flash(), falling to 0.
    double strength() const { return strength_; }
    // The fade, so a test can shorten it.
    QVariantAnimation* animation() const { return anim_; }

    // Scrolls `item` into view and washes it.
    void flash(QListWidgetItem* item);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void onValue(const QVariant& value);
    void followViewport();

    QListWidget* view_;
    QPersistentModelIndex index_;
    double strength_ = 0.0;
    QVariantAnimation* anim_;
};

} // namespace fv
