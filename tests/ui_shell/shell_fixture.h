#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include <QApplication>
#include <QElapsedTimer>
#include <QRect>
#include <QString>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {

// The application styled the way OpenWhisper's QtApplication styles it (ThemeManager, theme,
// font scale), for the duration of one test case.
class AppTheme {
public:
    explicit AppTheme(const QString& theme = QStringLiteral("dark"), int scalePercent = 100)
        : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() { qApp->setStyleSheet(QString()); }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

    fv::ui::ThemeManager& manager() { return manager_; }

private:
    fv::ui::ThemeManager manager_;
};

// What Bootstrap builds for the shell, without the parts a widget test does not need: a data
// folder of its own, an in-memory settings file, the GUI executor, an AppContext with the shell
// feature installed.
class ShellFixture {
public:
    explicit ShellFixture(const fv::Json& settings = fv::Json::object())
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), settings_(settings),
          ui_(qApp), context_(services()) {
        shell_.install(context_);
    }

    fv::AppContext& context() { return context_; }
    fv::MemorySettingsStore& settings() { return settings_; }
    const fv::Paths& paths() const { return paths_; }

    // A main window on a 1920 x 1080 screen, as OpenWhisper's window tests pin it.
    std::unique_ptr<fv::MainWindow> window() {
        auto made = std::make_unique<fv::MainWindow>(context_);
        made->availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        return made;
    }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
};

// Runs the event loop until done() or the time is up; returns done().
inline bool waitUntil(const std::function<bool()>& done,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout.count()) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

// Runs work on a thread of its own while this thread keeps its event loop turning, and returns
// what it returned. A later launch talks to a SingleInstance server that answers on this
// thread's event loop, and its write completes only once the server has read it, so the peer
// must not run here (Python's server had an accept thread of its own).
template <class Fn> auto onOtherThread(Fn work) {
    std::optional<std::invoke_result_t<Fn&>> result;
    std::atomic_bool done{false};
    std::thread worker([&] {
        result.emplace(work());
        done = true;
    });
    while (!done) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    worker.join();
    return std::move(*result);
}

// OpenWhisper's _settle: two window animations can run back to back.
inline void settle() {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 800) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    QApplication::processEvents();
}

} // namespace fvtest
