#pragma once

// What the overlay's tests share: the access OpenWhisper's tests have to the overlay's private
// state (they read and set its attributes), a pinned clock, the palette for one test, and the
// tick-and-paint loops of tests/test_overlay_moments.py and tests/test_recording_look.py.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QApplication>
#include <QFont>
#include <QList>
#include <QMouseEvent>
#include <QRect>
#include <QTimer>

#include "base/testing/fake_clock.h"
#include "ui_overlay/visual.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_theme/palette.h"
#include "ui_widgets/buttons.h"
#include "uimath/particles.h"
#include "uimath/voice_ribbon.h"

namespace fv {

struct WaveformOverlayTestAccess {
    explicit WaveformOverlayTestAccess(WaveformOverlay& overlay) : o(overlay) {}

    void updateAnimation() { o.updateAnimation(); }
    void finishHide() { o.finishHide(); }
    double now() const { return o.now(); }
    double& animationTime() { return o.animationTime_; }
    double& lastFrameTime() { return o.lastFrameTime_; }
    std::vector<Particle>& sttParticles() { return o.sttParticles_; }
    const std::vector<CollapsingDot>& collapseFrom() const { return o.collapseFrom_; }
    const Moment* moment() const { return o.moment(); }
    int transientMs() const { return o.transientMs(); }
    static double cancelSeconds() { return WaveformOverlay::cancelSeconds(); }
    QString workLabel() const { return o.workLabel(); }
    QString& labelFrom() { return o.labelFrom_; }
    double& labelChanged() { return o.labelChanged_; }
    std::optional<double>& fadingSince() { return o.fadingSince_; }
    double& listenStarted() { return o.listenStarted_; }
    double& workStarted() { return o.workStarted_; }
    double& workRate() { return o.workRate_; }
    double& workClock() { return o.workClock_; }
    double& captionStarted() { return o.captionStarted_; }
    QTimer& hiddenTimer() { return *o.hiddenTimer_; }
    QTimer& captionTimer() { return *o.captionTimer_; }
    const QString& caption() const { return o.caption_; }
    const QString& notice() const { return o.notice_; }
    int noticeMs() const { return o.noticeMs(); }
    QString fitNotice(const QFont& font, const QRect& rect) const {
        return o.fitNotice(font, rect);
    }
    ParticleStyle& style() { return *o.style_; }
    VoiceRibbon& ribbon() { return o.ribbon_; }
    Visual* visual() { return o.visual_.get(); }
    bool paintErrorLogged() const { return o.paintErrorLogged_; }
    int baseHeight() const { return o.baseHeight_; }
    const std::vector<double>& audioLevels() const { return o.audioLevels_; }
    bool placed() const { return o.placed(); }

    // The live preview, the language chip and the pointer.
    PreviewLayout& streamingLayout() { return o.streamingLayout_; }
    const QRectF& headerRoom() const { return o.headerRoom_; }
    const QRectF& badgeRect() const { return o.badgeRect_; }
    const QRectF& chipRect() const { return o.chipRect_; }
    Margins previewTextMargins() const { return o.previewTextMargins(); }
    QRect statusRect() const { return o.statusRect(); }
    int headerHeight() const { return o.headerHeight(); }
    int bandTop() const { return o.bandTop(); }
    double languageFlash() const { return o.languageFlash(); }
    bool chipVisible() const { return o.chipVisible(); }
    bool chipClickable() const { return o.chipClickable(); }
    const QString& hover() const { return o.hover_; }
    QString languageText() const { return o.languageText(); }
    QString languageName() const { return o.languageName(); }
    // _platform_takes_overlay_clicks patched.
    void setTakesClicks(bool takes) { o.takesClicksForTesting_ = takes; }

    // A walkthrough's Snap and Done, the corner and the hint.
    // _control_rects: "snap" and "finish" as last painted.
    std::map<QString, QRectF> controlRects() const {
        return {o.controlRects_.begin(), o.controlRects_.end()};
    }
    QRectF controlRect(const QString& key) const { return controlRects().at(key); }
    QPoint controlCenter(const QString& key) const { return controlRect(key).center().toPoint(); }
    bool cornered() const { return o.cornered_; }
    HotkeyHoverHint* hotkeyHint() const { return o.hotkeyHint_.data(); }
    const HeaderRow& headerRow() const { return o.headerRow_; }
    std::unique_ptr<Visual>& visualSlot() { return o.visual_; }
    bool hasDrag() const { return o.drag_ != nullptr; }

    WaveformOverlay& o;
};

} // namespace fv

namespace fvtest {

// A clock pinned well past the steady epoch, as time.monotonic() is.
inline std::unique_ptr<fv::FakeClock> pinnedClock() {
    auto clock = std::make_unique<fv::FakeClock>();
    clock->advance(std::chrono::seconds(1000));
    return clock;
}

// The overlay of OpenWhisper's fixture, on a pinned clock, closed at the end of the test.
struct Overlay {
    // settings, when given, must outlive the overlay.
    explicit Overlay(const fv::SettingsStore* settings = nullptr)
        : clock(pinnedClock()),
          widget(std::make_unique<fv::WaveformOverlay>(settings, clock.get())), access(*widget) {}
    ~Overlay() {
        widget->hide();
        widget->close();
    }
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    fv::WaveformOverlay& operator*() { return *widget; }
    fv::WaveformOverlay* operator->() { return widget.get(); }

    std::unique_ptr<fv::FakeClock> clock;
    std::unique_ptr<fv::WaveformOverlay> widget;
    fv::WaveformOverlayTestAccess access;
};

// set_current_palette for one test.
class ScopedPalette {
public:
    explicit ScopedPalette(const fv::ui::Palette& palette) : previous_(fv::ui::currentPalette()) {
        fv::ui::setCurrentPalette(palette);
    }
    ~ScopedPalette() { fv::ui::setCurrentPalette(previous_); }
    ScopedPalette(const ScopedPalette&) = delete;
    ScopedPalette& operator=(const ScopedPalette&) = delete;

private:
    const fv::ui::Palette& previous_;
};

// widget.grab() with no paint error logged.
inline void paint(Overlay& overlay) {
    overlay->grab();
    CHECK_FALSE(overlay.access.paintErrorLogged());
}

// One frame of 1/30 s, as the tests step it, then a paint. The clock moves by the frame too:
// OpenWhisper's tests ran on time.monotonic(), so the live label, the bars' grow-in, the work
// morph and the caption fade, which read the clock rather than the frame time, moved on as well.
inline void tick(Overlay& overlay) {
    overlay.clock->advance(std::chrono::nanoseconds(1'000'000'000 / 30));
    overlay.access.lastFrameTime() = overlay.access.now() - 1.0 / 30;
    overlay.access.updateAnimation();
    overlay->timer().stop();
    overlay->grab();
}

// _paint_through: tick and paint the current state up to `until` seconds; no errors.
inline void paintThrough(Overlay& overlay, double until) {
    overlay->timer().stop();
    while (overlay.access.animationTime() < until) {
        tick(overlay);
    }
    CHECK_FALSE(overlay.access.paintErrorLogged());
}

// _run: `seconds` of frames with the microphone at `level`.
inline void run(Overlay& overlay, double seconds, float level) {
    overlay->timer().stop();
    const double end = overlay.access.animationTime() + seconds;
    while (overlay.access.animationTime() < end) {
        overlay->updateAudioLevels(QList<float>(20, level));
        tick(overlay);
    }
    CHECK_FALSE(overlay.access.paintErrorLogged());
}

// timer.timeout.emit(): runs what the timeout is connected to, now.
inline void fireTimeout(QTimer& timer) {
    QMetaObject::invokeMethod(&timer, "timeout", Qt::DirectConnection);
}

// A mouse event as QApplication delivers it to the overlay.
inline void sendMouse(QWidget& widget, QEvent::Type type, const QPoint& local) {
    const Qt::MouseButtons held =
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent event(type, QPointF(local), QPointF(widget.mapToGlobal(local)), Qt::LeftButton,
                      held, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

} // namespace fvtest
