#pragma once

#include <chrono>
#include <cmath>
#include <optional>

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QPointF>
#include <QTest>

#include "base/testing/fake_clock.h"
#include "ui_snap/desktop.h"
#include "ui_snap/testing/fake_desktop.h"
#include "ui_walkthrough/walkthrough_panel.h"

// What the panel tests share: OpenWhisper's fixtures (tests/test_walkthrough_panel.py's clock
// and panel, _run, _image, _click), over a FakeClock in place of the patched time.monotonic.
namespace fvtest {

// About -26 dBFS: speech on a desk microphone.
inline constexpr double kLoud = 0.05;

// A desktop-sized picture of one colour (OpenWhisper's _image).
inline QImage image(int width = 1600, int height = 900) {
    QImage picture(width, height, QImage::Format_RGB32);
    picture.fill(QColor(0x33, 0x66, 0x99));
    return picture;
}

// Capture exclusion goes to a fake desktop, so no test calls SetWindowDisplayAffinity.
class FakeDesktopScope {
public:
    FakeDesktopScope() : previous_(fv::ui_snap::setWin32Desktop(&desktop_)) {}
    ~FakeDesktopScope() { fv::ui_snap::setWin32Desktop(previous_); }
    FakeDesktopScope(const FakeDesktopScope&) = delete;
    FakeDesktopScope& operator=(const FakeDesktopScope&) = delete;

    fv::ui_snap::testing::FakeWin32Desktop desktop_;

private:
    fv::ui_snap::Win32Desktop* previous_;
};

// OpenWhisper's clock and panel fixtures: time at 100 s, a column that stops its frames and
// hides when the test ends.
struct PanelFixture {
    PanelFixture() { clock.advance(std::chrono::seconds(100)); }
    ~PanelFixture() {
        panel.stopFrames();
        panel.hide();
        panel.close();
    }
    PanelFixture(const PanelFixture&) = delete;
    PanelFixture& operator=(const PanelFixture&) = delete;

    // Frames of 16 ms for seconds, hearing level before each (OpenWhisper's _run).
    void run(double seconds, std::optional<double> level = std::nullopt) {
        const auto frames = static_cast<int>(std::ceil(seconds / 0.016 - 1e-9));
        for (int i = 0; i < frames; ++i) {
            clock.advance(std::chrono::milliseconds(16));
            if (level) {
                panel.hear(*level);
            }
            panel.advanceFrame();
        }
    }

    QPointF footerButton(const char* key) const {
        return panel.footerControl(QString::fromLatin1(key)).rect.center();
    }

    FakeDesktopScope desktop;
    fv::FakeClock clock;
    fv::WalkthroughPanel panel{clock};
};

inline void click(QWidget& widget, QPointF point) {
    QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, point.toPoint());
}

// Deletes what closed and lets the events run (OpenWhisper's _pump_events).
inline void pumpEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

} // namespace fvtest
