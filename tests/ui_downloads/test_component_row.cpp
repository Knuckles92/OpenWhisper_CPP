// The component row of Settings > Downloads: badge, size and buttons per component state, the
// install progress, and the Remove guard that follows an install (component_row_widget.py).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include <QProgressBar>
#include <QSignalSpy>
#include <QTest>

#include "models/catalog.h"
#include "ui_downloads/component_row_widget.h"
#include "ui_downloads_support.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"

using fv::ComponentRowWidget;
using fv::models::ComponentInfo;
using fv::models::ComponentState;
namespace install_phase = fv::models::install_phase;

namespace {

ComponentInfo info(ComponentState state) {
    ComponentInfo component;
    component.component_id = "asr-nvidia-cuda";
    component.display_name = "NVIDIA Speech GPU";
    component.summary = "Runs speech models on an NVIDIA GPU";
    component.state = state;
    component.download_bytes = 117'000'000;
    component.install_bytes = 410'000'000;
    return component;
}

bool shown(const QWidget* widget) {
    return !widget->isHidden();
}

QString badgeTone(const ComponentRowWidget& row) {
    return row.badge()->property("tone").toString();
}

// Runs the event loop for at least this long, which a single-shot timer needs to fire.
void waitFor(std::chrono::milliseconds duration) {
    QTest::qWait(static_cast<int>(duration.count()));
}

} // namespace

TEST_CASE("a component row that is not installed offers Install and the download size",
          "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    row.updateState(info(ComponentState::NotInstalled), false);

    CHECK(row.nameLabel()->text() == "NVIDIA Speech GPU");
    CHECK(row.summaryLabel()->text() == "Runs speech models on an NVIDIA GPU");
    CHECK(row.badge()->text() == "Not installed");
    CHECK(badgeTone(row) == "idle");
    CHECK(row.sizeLabel()->text() == "117 MB download");
    CHECK(shown(row.installButton()));
    CHECK(row.installButton()->text() == "Install");
    CHECK(row.installButton()->isEnabled());
    CHECK_FALSE(shown(row.removeButton()));
    CHECK_FALSE(shown(row.progressBar()));
}

TEST_CASE("the badge, button label and Remove follow the component's state", "[widget]") {
    fvtest::AppTheme theme;
    struct Row {
        ComponentState state;
        const char* badge;
        const char* tone;
        const char* button; // empty: Install hidden
        bool remove;
    };
    const Row rows[] = {
        {ComponentState::NotInstalled, "Not installed", "idle", "Install", false},
        {ComponentState::External, "Available", "installed", "", false},
        {ComponentState::Installed, "Installed", "installed", "", true},
        {ComponentState::UpdateAvailable, "Update available", "warning", "Update", true},
        {ComponentState::Incompatible, "Update required", "warning", "Reinstall", true},
        {ComponentState::Broken, "Damaged", "warning", "Repair", true},
    };
    for (const Row& expected : rows) {
        CAPTURE(std::string(fv::models::state_name(expected.state)));
        ComponentRowWidget row("asr-nvidia-cuda");
        row.updateState(info(expected.state), false);
        CHECK(row.badge()->text() == expected.badge);
        CHECK(badgeTone(row) == expected.tone);
        CHECK(shown(row.installButton()) == (std::string(expected.button) != ""));
        if (std::string(expected.button) != "") {
            CHECK(row.installButton()->text() == expected.button);
        }
        CHECK(shown(row.removeButton()) == expected.remove);
    }
}

TEST_CASE("an installed component shows its size on disk and an external one says so", "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    row.updateState(info(ComponentState::Installed), false);
    CHECK(row.sizeLabel()->text() == "410 MB");

    row.updateState(info(ComponentState::External), false);
    CHECK(row.sizeLabel()->text() == "Existing setup");

    ComponentInfo unknown = info(ComponentState::NotInstalled);
    unknown.download_bytes = 0;
    row.updateState(unknown, false);
    CHECK(row.sizeLabel()->text().isEmpty());
}

TEST_CASE("the reason joins the summary", "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    ComponentInfo component = info(ComponentState::Broken);
    component.reason = "A file is missing";
    row.updateState(component, false);
    CHECK(row.summaryLabel()->text() ==
          QString::fromUtf8("Runs speech models on an NVIDIA GPU  —  A file is missing"));

    component.summary.clear();
    row.updateState(component, false);
    CHECK(row.summaryLabel()->text() == "A file is missing");
}

TEST_CASE("an installing row turns Install into Cancel and tracks the phases", "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    QSignalSpy install(&row, &ComponentRowWidget::installClicked);
    QSignalSpy cancel(&row, &ComponentRowWidget::cancelClicked);

    row.updateState(info(ComponentState::NotInstalled), false);
    row.installButton()->click();
    REQUIRE(install.count() == 1);
    CHECK(install.takeFirst().at(0).toString() == "asr-nvidia-cuda");

    row.updateState(info(ComponentState::NotInstalled), true);
    CHECK(row.installing());
    CHECK(row.badge()->text() == QString::fromUtf8("Downloading…"));
    CHECK(badgeTone(row) == "downloading");
    CHECK(row.installButton()->text() == "Cancel");
    CHECK(row.installButton()->isEnabled());
    CHECK_FALSE(shown(row.removeButton()));
    CHECK(shown(row.progressBar()));

    row.installButton()->click();
    REQUIRE(cancel.count() == 1);
    CHECK(cancel.takeFirst().at(0).toString() == "asr-nvidia-cuda");
    CHECK(install.count() == 0);

    row.setProgress(install_phase::kDownloading, 60'000'000, 117'000'000);
    CHECK(row.progressBar()->maximum() == 100);
    CHECK(row.progressBar()->value() == 51);
    CHECK(row.sizeLabel()->text() == "60 MB of 117 MB");

    row.setProgress(install_phase::kVerifying, 1, 2);
    CHECK(row.badge()->text() == QString::fromUtf8("Verifying…"));
    CHECK(row.sizeLabel()->text().isEmpty());

    row.setProgress(install_phase::kExtracting, 1, 2);
    CHECK(row.badge()->text() == QString::fromUtf8("Installing…"));
    row.setProgress(install_phase::kFinalizing, 1, 2);
    CHECK(row.badge()->text() == QString::fromUtf8("Finishing…"));
    row.setProgress(install_phase::kResolving, 0, 0);
    CHECK(row.badge()->text() == QString::fromUtf8("Preparing…"));
    CHECK(row.progressBar()->maximum() == 0);
    row.setProgress("something-new", 1, 2);
    CHECK(row.badge()->text() == QString::fromUtf8("Working…"));
}

TEST_CASE("progress is ignored while no install is in flight", "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    row.updateState(info(ComponentState::NotInstalled), false);
    row.setProgress(install_phase::kDownloading, 1, 2);
    CHECK(row.badge()->text() == "Not installed");
    CHECK(row.sizeLabel()->text() == "117 MB download");
}

TEST_CASE("Remove stays disabled for 600 ms after an install ends", "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    CHECK(ComponentRowWidget::kRemoveGuardMs == 600);

    QSignalSpy removes(&row, &ComponentRowWidget::removeClicked);

    row.updateState(info(ComponentState::NotInstalled), true);
    row.updateState(info(ComponentState::Installed), false);
    CHECK(shown(row.removeButton()));
    CHECK_FALSE(row.removeButton()->isEnabled());
    CHECK(row.removeGuardActive());
    // The second click of a double-click lands on nothing.
    row.removeButton()->click();

    // A refresh while the guard runs does not lift it.
    row.updateState(info(ComponentState::Installed), false);
    CHECK_FALSE(row.removeButton()->isEnabled());
    CHECK(removes.count() == 0);

    waitFor(std::chrono::milliseconds(750));
    CHECK_FALSE(row.removeGuardActive());
    CHECK(row.removeButton()->isEnabled());
    row.removeButton()->click();
    REQUIRE(removes.count() == 1);
    CHECK(removes.takeFirst().at(0).toString() == "asr-nvidia-cuda");
}

TEST_CASE("an installed component gets Remove enabled at once", "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    row.updateState(info(ComponentState::Installed), false);
    CHECK(shown(row.removeButton()));
    CHECK(row.removeButton()->isEnabled());
    CHECK_FALSE(row.removeGuardActive());

    QSignalSpy remove(&row, &ComponentRowWidget::removeClicked);
    row.removeButton()->click();
    REQUIRE(remove.count() == 1);
    CHECK(remove.takeFirst().at(0).toString() == "asr-nvidia-cuda");
}

TEST_CASE("a row opens its details on a body click, keys and not on its buttons", "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    row.updateState(info(ComponentState::NotInstalled), false);
    row.resize(640, 80);
    row.show();
    QSignalSpy details(&row, &ComponentRowWidget::detailsRequested);

    QTest::mouseClick(row.installButton(), Qt::LeftButton);
    CHECK(details.count() == 0);

    QTest::mouseClick(&row, Qt::LeftButton, Qt::NoModifier, QPoint(300, 40));
    REQUIRE(details.count() == 1);
    CHECK(details.takeFirst().at(0).toString() == "asr-nvidia-cuda");

    for (const Qt::Key key : {Qt::Key_Return, Qt::Key_Enter, Qt::Key_Space}) {
        QTest::keyClick(&row, key);
        CHECK(details.count() == 1);
        details.clear();
    }

    CHECK(row.toolTip() == "Click to view component details");
    CHECK(row.accessibleName() == "asr-nvidia-cuda component");
    CHECK(row.accessibleDescription() ==
          "Open component details. Install and remove actions are separate.");
}

TEST_CASE("the component row's buttons have the compact sizes", "[widget]") {
    fvtest::AppTheme theme;
    ComponentRowWidget row("asr-nvidia-cuda");
    CHECK(row.installButton()->minimumWidth() == 92);
    CHECK(row.installButton()->maximumWidth() == 92);
    CHECK(row.removeButton()->minimumWidth() == 80);
    CHECK(row.removeButton()->maximumWidth() == 80);
    CHECK(row.installButton()->objectName() == "componentInstallButton");
    CHECK(row.removeButton()->objectName() == "componentRemoveButton");
    CHECK(row.sizeLabel()->minimumWidth() == 110);
}
