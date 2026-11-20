// Checklist lines of docs/port/map/a32-user-facing-features.md section 21 that the ported
// upstream tests leave open: GS-02 ("each page opens with focus on Continue") and GS-05 (the
// Output step's clipboard switch writes through Settings like its siblings).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAbstractButton>
#include <QPushButton>
#include <QTest>

#include "guided_setup_support.h"
#include "ui_settings/settings_dialog.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::GuidedSetupDialog;
using fv::Json;
using fvtest::SetupRig;

} // namespace

TEST_CASE("every step opens with focus on Continue, the Ready page on its try-it box",
          "[test_guided_setup][widget]") {
    const int page = GENERATE(int(GuidedSetupDialog::Record), int(GuidedSetupDialog::CleanUp),
                              int(GuidedSetupDialog::Output));
    CAPTURE(page);
    SetupRig rig;
    GuidedSetupDialog& setup = rig.setup();
    setup.show();
    setup.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&setup));
    // Move focus elsewhere first, so the page change is what puts it back.
    setup.showPage(GuidedSetupDialog::Ready);
    REQUIRE(setup.focusWidget() != setup.nextButton());
    setup.showPage(page);
    CHECK(setup.focusWidget() == setup.nextButton());
    CHECK(setup.focusWidget() != setup.allSettingsButton());
}

TEST_CASE("the Output step's clipboard switch saves through Settings and restores a failed save",
          "[test_guided_setup][widget]") {
    SetupRig rig(Json{{"copy_clipboard", false}});
    GuidedSetupDialog& setup = rig.setup();
    setup.showPage(GuidedSetupDialog::Output);
    CHECK_FALSE(setup.copySwitch()->isChecked());
    rig.store().file().fail_next_replace("Disk full");
    setup.copySwitch()->click();
    CHECK_FALSE(setup.copySwitch()->isChecked());
    auto* source = qobject_cast<QAbstractButton*>(rig.settings()->control("copy_clipboard_check"));
    REQUIRE(source != nullptr);
    CHECK_FALSE(source->isChecked());
    CHECK(setup.errorLabel()->text().contains(QStringLiteral("Disk full")));
    setup.copySwitch()->click();
    CHECK(setup.copySwitch()->isChecked());
    CHECK(source->isChecked());
    CHECK(rig.saved("copy_clipboard") == true);
    CHECK(setup.errorLabel()->isHidden());
}
