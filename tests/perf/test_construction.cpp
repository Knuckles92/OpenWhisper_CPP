// First and warm construction of the main window and of the Settings window (OpenWhisper's
// performance-implementation-2026-10-03, desktop.after_main_window and
// after_settings_and_history.settings), with the child widget counts. Offscreen, over the
// controller's real feature graph and its fakes (tests/ui_controller/controller_support.h).
//
// Where OpenWhisper_CPP differs: its main window is built by the features as they install (the
// shell feature constructs it, each feature plugs its screens in, completeSetup() fills the tabs),
// so "construction" here is the feature graph's install through completeSetup and the tray, the
// span Bootstrap reports between its "features" and "main_window_created" steps; OpenWhisper's
// number is MainWindow's constructor. The Settings window is the SettingsDialog's constructor, as
// OpenWhisper's is. Each Catch2 case runs in its own process under CTest, so "first" is the first
// in the process; the warm builds follow in the same process, as OpenWhisper's did.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QWidget>

#include "../ui_controller/controller_support.h"
#include "support/perf_support.h"
#include "ui_settings/settings_dialog.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace {

constexpr int kBuilds = 3; // the first and two warm, as OpenWhisper recorded

// The application styled as the app styles it (Bootstrap's Application applies the theme).
class Styled {
public:
    Styled() : manager_(QStringLiteral("dark")) {
        fv::ui::applyUiTheme(QStringLiteral("dark"), *qApp, manager_);
        fv::ui::applyUiFontScale(100, *qApp, manager_);
    }
    ~Styled() { qApp->setStyleSheet(QString()); }
    Styled(const Styled&) = delete;
    Styled& operator=(const Styled&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

double showAndSettle(QWidget& widget) {
    const auto started = fvperf::Clock::now();
    widget.show();
    QApplication::processEvents();
    QApplication::processEvents();
    return fvperf::msSince(started);
}

} // namespace

TEST_CASE("main window first and warm construction", "[perf]") {
    Styled styled;
    std::vector<double> builds, shows, widgets;
    for (int build = 0; build < kBuilds; ++build) {
        std::optional<fvperf::Clock::time_point> started;
        fvtest::RigOptions options;
        options.beforeFeatures = [&started](fv::AppContext&) {
            started = fvperf::Clock::now();
        };
        auto rig = std::make_unique<fvtest::ControllerRig>(std::move(options));
        REQUIRE(started.has_value());
        builds.push_back(fvperf::msSince(*started));
        widgets.push_back(static_cast<double>(rig->window().findChildren<QWidget*>().size()));
        shows.push_back(showAndSettle(rig->window()));
        rig.reset();
        QApplication::processEvents();
    }

    const std::string test = "perf.main window first and warm construction";
    const fv::Json notes{{"measures", "feature install through completeSetup and the tray"}};
    const auto first = fvperf::record("main_window_first_ms", "ms", {builds[0]}, test, notes);
    CHECK(fvperf::underCeiling(first, first.p50));
    const auto warm = fvperf::record("main_window_warm_ms", "ms",
                                     {builds.begin() + 1, builds.end()}, test, notes);
    CHECK(fvperf::underCeiling(warm, warm.p50));
    fvperf::record("main_window_show_events_ms", "ms", shows, test);
    fvperf::record("main_window_widgets", "widgets", {widgets[0]}, test);
}

TEST_CASE("settings window first and warm construction", "[perf]") {
    Styled styled;
    fvtest::ControllerRig rig;
    std::vector<double> builds, shows, widgets;
    for (int build = 0; build < kBuilds; ++build) {
        const auto started = fvperf::Clock::now();
        auto* dialog = new fv::SettingsDialog(rig.context(), &rig.window());
        builds.push_back(fvperf::msSince(started));
        widgets.push_back(static_cast<double>(dialog->findChildren<QWidget*>().size()));
        shows.push_back(showAndSettle(*dialog));
        dialog->hide();
        delete dialog;
        QApplication::processEvents();
    }

    const std::string test = "perf.settings window first and warm construction";
    const auto first = fvperf::record("settings_first_ms", "ms", {builds[0]}, test);
    CHECK(fvperf::underCeiling(first, first.p50));
    const auto warm =
        fvperf::record("settings_warm_ms", "ms", {builds.begin() + 1, builds.end()}, test);
    CHECK(fvperf::underCeiling(warm, warm.p50));
    fvperf::record("settings_show_events_ms", "ms", shows, test);
    fvperf::record("settings_widgets", "widgets", {widgets[0]}, test);
}
