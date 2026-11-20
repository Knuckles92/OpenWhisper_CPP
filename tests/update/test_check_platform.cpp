// CheckContext::platform: the apply mode is decided for the product's platform, not the system
// the check runs on, so the Windows flow is testable anywhere and Windows itself is unchanged.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "base/platform_info.h"
#include "base/testing/fake_clock.h"
#include "settings/testing/memory_store.h"
#include "update/check.h"
#include "update/testing/fake_feed.h"
#include "update/update_copy.h"

namespace update = fv::update;
using update::testing::FakeFeed;

namespace {

update::InstallProbe registered_install() {
    update::InstallProbe probe;
    probe.registered = true;
    probe.install_dir = "C:/Programs/App";
    probe.running_dir = "C:/Programs/App";
    return probe;
}

update::UpdateCheckResult check_on(const std::string& platform) {
    FakeFeed feed;
    feed.latest = feed.release({"0.2.0"});
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    const auto config = feed.config();
    const auto install = registered_install();
    update::CheckContext context{feed, config, settings, install, clock, {}, "0.1.0"};
    context.platform = platform;
    return update::check_for_update(context, false);
}

} // namespace

TEST_CASE("a check for win32 offers the verified setup of a registered install", "[update]") {
    const auto result = check_on("win32");
    CHECK(result.status == update::UpdateStatus::UpdateAvailable);
    CHECK(result.channel == update::InstallChannel::Installer);
    CHECK(result.apply_mode == update::ApplyMode::Setup);
    CHECK(result.can_apply);
}

TEST_CASE("a check for another platform only notifies", "[update]") {
    const auto result = check_on("linux");
    CHECK(result.apply_mode == update::ApplyMode::NotifyOnly);
    CHECK_FALSE(result.can_apply);
}

TEST_CASE("a check without a platform decides for this build's", "[update]") {
    const auto result = check_on("");
    const bool windows = fv::python_platform() == "win32";
    CHECK(result.can_apply == windows);
    CHECK(result.apply_mode ==
          (windows ? update::ApplyMode::Setup : update::ApplyMode::NotifyOnly));
}

// The dialog's body names the channel of the platform the check decided for, not the system the
// dialog runs on (app_update_dialog.py _body_text, channel_label(sys.platform)).
TEST_CASE("the update dialog's body names the channel of the checked platform", "[update]") {
    const auto windows = check_on("win32");
    CHECK(windows.platform == "win32");
    CHECK(update::body_text(&windows, "").starts_with("You have 0.1.0 (Windows installer)."));
    CHECK(update::body_text(&windows, "") == update::body_text(&windows, "", "win32"));
    const auto linux_check = check_on("linux");
    CHECK(update::body_text(&linux_check, "").starts_with("You have 0.1.0 (Linux package)."));
    const auto here = check_on("");
    CHECK(here.platform == fv::python_platform());
    CHECK(update::body_text(&here, "") == update::body_text(&here, "", fv::python_platform()));
    // A result built without a check describes the product's platform.
    update::UpdateCheckResult built;
    built.channel = update::InstallChannel::Installer;
    built.current_version = "0.1.0";
    CHECK(update::body_text(&built, "") == "You have 0.1.0 (Windows installer).");
}
