// What a launch by the native update helper (--update-health, UpdateHealthLaunch in the context)
// leaves out once the main window shows (ui_qt/bootstrap.py:347-361): a normal launch shows the
// leftover update error and offers guided setup; a health launch does neither, and still starts
// the engine (notify_main_ui_ready). Both steps are composed here, in the controller's
// mainWindowShown and GuidedSetup::install, as Bootstrap runs them.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

#include "appkit/app_context.h"
#include "controller_support.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_shell/bootstrap.h"
#include "update/apply_error.h"
#include "update/download.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;

namespace {

RigOptions launch(bool health) {
    RigOptions options;
    // An install whose guided setup is still to be offered.
    options.settings["guided_setup_state"] = "pending";
    if (health) {
        // What Bootstrap provides before the features run when main() passed the update gate
        // with a valid health token.
        options.beforeFeatures = [](fv::AppContext& context) {
            context.provide(std::make_shared<fv::UpdateHealthLaunch>());
        };
    }
    return options;
}

} // namespace

TEST_CASE("a normal launch shows the update error and offers setup once the window shows",
          "[widget]") {
    ControllerRig rig(launch(false));
    const std::filesystem::path updates = fv::update::updates_root(rig.paths());
    fv::update::write_apply_error(updates, "The update failed.");
    fv::GuidedSetup* setup = rig.context().find<fv::GuidedSetup>();
    REQUIRE(setup != nullptr);

    rig.mainWindowShown();

    CHECK(rig.calls().has("warning:Update failed:The update failed."));
    CHECK_FALSE(std::filesystem::exists(fv::update::apply_error_path(updates)));
    CHECK(setup->offerTimer()->isActive());
    CHECK(setup->offerTimer()->interval() == fv::GuidedSetup::kOfferDelayMs);
}

TEST_CASE("a health launch shows no update error and offers no setup, and still starts",
          "[widget]") {
    ControllerRig rig(launch(true));
    const std::filesystem::path updates = fv::update::updates_root(rig.paths());
    fv::update::write_apply_error(updates, "The update failed.");
    fv::GuidedSetup* setup = rig.context().find<fv::GuidedSetup>();
    REQUIRE(setup != nullptr);

    // mainWindowShown, then until the engine is loaded: notify_main_ui_ready runs as on a
    // normal launch.
    REQUIRE(rig.startEngine());

    CHECK_FALSE(rig.calls().has("warning:Update failed:The update failed."));
    // Left for the next normal launch, as show_apply_error_if_any is never called.
    CHECK(std::filesystem::exists(fv::update::apply_error_path(updates)));
    CHECK_FALSE(setup->offerTimer()->isActive());
    CHECK(setup->dialog() == nullptr);
}
