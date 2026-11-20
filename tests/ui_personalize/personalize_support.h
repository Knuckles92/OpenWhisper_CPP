#pragma once

// Helpers the Personalize tests share: the Settings window over fakes (the ui_settings tests'
// SettingsRig, theme and event-loop helpers) and a wait that turns the event loop until a
// condition holds.

#include <chrono>
#include <functional>
#include <thread>

#include <QApplication>
#include <QElapsedTimer>

#include "../ui_settings/settings_support.h"

namespace fvtest {

// QTest-style: turns the event loop until predicate holds or the time is up; the last answer.
inline bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QApplication::processEvents();
    return predicate();
}

} // namespace fvtest
