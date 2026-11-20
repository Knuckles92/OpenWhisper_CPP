#pragma once

#include <chrono>
#include <memory>

#include <QApplication>
#include <QString>

#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "meeting/platform_policy.h"
#include "ui_meeting/meeting_mode_tab.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {

// The application styled the way OpenWhisper's render_reference.py styles it.
class AppTheme {
public:
    explicit AppTheme(const QString& theme, int scale = 100) : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scale, *qApp, manager_);
    }
    ~AppTheme() {
        fv::ui::applyUiFontScale(100, *qApp, manager_);
        qApp->setStyleSheet(QString());
    }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

// A tab on a FakeClock (steady time at `start` seconds, wall time 2026-11-14 10:00 in New York)
// on Windows, as the Python tests build MeetingModeTab() with settings that leave the switch
// and developer mode off; the rest of `options` is the caller's.
struct TabFixture {
    explicit TabFixture(double start = 1000.0, fv::MeetingModeTabOptions options = {})
        : clock(std::chrono::sys_days{std::chrono::year{2026} / 11 / 14} + std::chrono::hours{15},
                "America/New_York") {
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(start)));
        options.clock = &clock;
        options.platform = fv::meeting::PlatformInfo{"win32", "AMD64", ""};
        tab = std::make_unique<fv::MeetingModeTab>(options);
    }
    ~TabFixture() {
        tab.reset();
        QApplication::processEvents();
    }
    TabFixture(const TabFixture&) = delete;
    TabFixture& operator=(const TabFixture&) = delete;

    // Seconds on the clock's steady().
    double now() const {
        return std::chrono::duration<double>(clock.steady().time_since_epoch()).count();
    }
    void advance(double seconds) {
        clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(seconds)));
    }
    fv::MeetingModeTab* operator->() const { return tab.get(); }
    fv::MeetingModeTab& operator*() const { return *tab; }

    fv::FakeClock clock;
    std::unique_ptr<fv::MeetingModeTab> tab;
};

} // namespace fvtest
