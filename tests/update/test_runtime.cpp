// The updater runtime: the check schedule of OpenWhisper's tests/test_application_controller.py,
// the beta-track side effect of tests/test_settings_binder.py, and downloads, cancels and
// leftovers through the runtime, on manual executors and a fake clock.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "download_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "update/runtime.h"
#include "update/testing/fake_feed.h"
#include "update_support.h"

namespace update = fv::update;
using namespace std::chrono_literals;
using fv::Json;
using update::testing::FakeFeed;
using namespace fv::update::test;

namespace {

using CheckReport = std::tuple<std::optional<update::UpdateCheckResult>, std::string, bool>;

// A runtime whose UI timers follow the fake clock; the jobs and components strands run only when
// a test runs them.
struct Fixture {
    explicit Fixture(Json initial = Json::object())
        : settings(initial),
          runtime(update::CheckContext{client, feed, settings, probe, clock, {}, "0.1.0"},
                  root.path() / "updates", ui, jobs, components, clock, "win32") {
        checks_subscription = runtime.check_finished.connect(
            ui, [this](std::optional<update::UpdateCheckResult> result, std::string error,
                       bool manual) { checks.emplace_back(std::move(result), error, manual); });
        progress_subscription = runtime.download_progress.connect(ui, [this](std::string phase,
                                                                             std::uint64_t done,
                                                                             std::uint64_t total) {
            progress.push_back(phase + " " + std::to_string(done) + "/" + std::to_string(total));
        });
        finished_subscription = runtime.download_finished.connect(
            ui, [this](std::optional<update::Handoff> handoff, std::string error) {
                finished.emplace_back(std::move(handoff), std::move(error));
            });
    }

    std::string at(std::chrono::seconds offset) const { return iso(clock.utc() + offset); }

    fv::testkit::TempDataRoot root;
    FakeFeed client{std::string(kRepo)};
    update::FeedConfig feed = client.config();
    update::InstallProbe probe = installed_probe();
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    fv::ManualExecutor ui{clock};
    fv::ManualExecutor jobs;
    fv::ManualExecutor components;
    update::UpdaterRuntime runtime;
    std::vector<CheckReport> checks;
    std::vector<std::string> progress;
    std::vector<std::pair<std::optional<update::Handoff>, std::string>> finished;
    fv::Subscription checks_subscription, progress_subscription, finished_subscription;
};

std::string key(const auto& setting) {
    return std::string(setting.key());
}

} // namespace

// from
// tests/test_application_controller.py::test_finished_check_arms_the_next_one_from_the_saved_times
TEST_CASE("finished check arms the next one from the saved times",
          "[test_application_controller]") {
    Fixture fixture;
    // The last success was an hour ago; this check fails, which waits only an hour.
    fixture.settings.set_contents(Json{{key(fv::keys::update_last_check_at), fixture.at(-1h)}});
    fixture.client.api_error = FakeFeed::offline();
    fixture.runtime.request_check(false);
    fixture.jobs.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.checks.size() == 1);
    CHECK_FALSE(std::get<0>(fixture.checks[0]).has_value());
    CHECK_FALSE(std::get<1>(fixture.checks[0]).empty());
    CHECK_FALSE(std::get<2>(fixture.checks[0]));
    CHECK(fixture.runtime.check_timer_delay() == std::chrono::milliseconds(3 * 3600 * 1000));
}

// from
// tests/test_application_controller.py::test_unsaved_check_times_never_mean_checking_again_at_once
TEST_CASE("unsaved check times never mean checking again at once",
          "[test_application_controller]") {
    Fixture fixture;
    fixture.client.api_error = FakeFeed::offline();
    // The failure time cannot be written, so the saved times say the next check is due now.
    fixture.settings.file().fail_next_replace("disk full");
    fixture.runtime.request_check(false);
    fixture.jobs.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.checks.size() == 1);
    CHECK_FALSE(fixture.settings.contents().contains(key(fv::keys::update_last_failed_check_at)));
    CHECK(fixture.runtime.check_timer_delay() == update::kUpdateRecheckFloor);
}

// from tests/test_application_controller.py::test_update_tick_checks_when_due_and_otherwise_waits
TEST_CASE("update tick checks when due and otherwise waits", "[test_application_controller]") {
    Fixture fixture;
    // Off: the tick arms nothing.
    fixture.settings.set_contents(Json{{key(fv::keys::update_check_enabled), false}});
    fixture.runtime.schedule_check();
    CHECK(fixture.runtime.check_timer_delay() == update::kUpdateCheckDelay);
    fixture.ui.advance(update::kUpdateCheckDelay);
    CHECK_FALSE(fixture.runtime.check_timer_delay());
    CHECK(fixture.jobs.queued() == 0);

    // Due in 90 s when the tick runs: it waits that long.
    fixture.settings.set_contents(Json{
        {key(fv::keys::update_last_check_at), iso(fixture.clock.utc() + update::kUpdateCheckDelay -
                                                  update::kUpdateCheckInterval + 90s)}});
    fixture.runtime.schedule_check();
    fixture.ui.advance(update::kUpdateCheckDelay);
    CHECK(fixture.runtime.check_timer_delay() == std::chrono::milliseconds(90'000));
    CHECK(fixture.jobs.queued() == 0);

    // Due now: it checks, as an automatic check.
    fixture.ui.advance(90s);
    CHECK(fixture.jobs.queued() == 1);
    fixture.client.api_error = FakeFeed::offline();
    fixture.jobs.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.checks.size() == 1);
    CHECK_FALSE(std::get<2>(fixture.checks[0]));
}

// from tests/test_application_controller.py::test_launch_check_skips_the_interval_once
TEST_CASE("launch check skips the interval once", "[test_application_controller]") {
    Fixture fixture;
    // A success an hour ago (the interval would wait three more), a failure 30 minutes ago.
    fixture.settings.set_contents(
        Json{{key(fv::keys::update_last_check_at), fixture.at(-1h)},
             {key(fv::keys::update_last_failed_check_at), fixture.at(-30min)}});
    fixture.runtime.schedule_check(true);
    fixture.ui.advance(update::kUpdateCheckDelay);
    // Still the launch check after the failure back-off.
    CHECK(fixture.jobs.queued() == 0);
    CHECK(fixture.runtime.check_timer_delay() == std::chrono::milliseconds(1800s - 8s));
    fixture.ui.advance(1800s - 8s);
    CHECK(fixture.jobs.queued() == 1);
    CHECK_FALSE(fixture.runtime.check_timer_delay());

    // The usual interval only once it has run.
    fixture.runtime.schedule_check();
    fixture.ui.advance(update::kUpdateCheckDelay);
    // Due three hours after the launch time; this tick runs 1,808 s after it.
    CHECK(fixture.runtime.check_timer_delay() ==
          std::chrono::milliseconds(update::kUpdateCheckInterval - 1h - 1800s - 8s));
    CHECK(fixture.jobs.queued() == 1);
}

// from tests/test_settings_binder.py::test_beta_track_toggle_makes_the_next_update_check_due
TEST_CASE("beta track toggle makes the next update check due", "[test_settings_binder]") {
    Fixture fixture(Json{{key(fv::keys::update_beta_track_enabled), true},
                         {key(fv::keys::update_last_check_at), "2026-11-11T00:00:00+00:00"},
                         {key(fv::keys::update_api_retry_at), "2026-11-11T01:00:00+00:00"}});
    // As the General page's tile writes it.
    fixture.settings.save_setting(fv::keys::update_beta_track_enabled.key(), false);
    fixture.ui.run_all();
    const auto settings = fixture.settings.contents();
    CHECK(settings[key(fv::keys::update_beta_track_enabled)] == false);
    CHECK_FALSE(settings.contains(key(fv::keys::update_last_check_at)));
    CHECK(settings.contains(key(fv::keys::update_api_retry_at)));
    // And the automatic check is due again.
    CHECK(fixture.runtime.check_timer_delay() == update::kUpdateCheckDelay);
}

TEST_CASE("turning checks back on schedules the check, and the notice switch does too",
          "[update]") {
    Fixture fixture(Json{{key(fv::keys::update_check_enabled), false},
                         {key(fv::keys::update_last_check_at), "2026-01-01T00:00:00+00:00"}});
    fixture.settings.save_setting(fv::keys::update_check_enabled.key(), true);
    fixture.ui.run_all();
    CHECK(fixture.runtime.check_timer_delay() == update::kUpdateCheckDelay);
    // Not a track change: the last check time stays.
    CHECK(fixture.settings.contents().contains(key(fv::keys::update_last_check_at)));
    fixture.runtime.stop();
    CHECK_FALSE(fixture.runtime.check_timer_delay());
    fixture.settings.save_setting(fv::keys::update_notify_enabled.key(), false);
    fixture.ui.run_all();
    CHECK_FALSE(fixture.runtime.check_timer_delay()); // stopped: the watch is gone
}

TEST_CASE("a manual check reports the release it found", "[update]") {
    Fixture fixture;
    fixture.client.latest = fixture.client.release(FakeFeed::ReleaseSpec{"0.2.0"});
    fixture.runtime.request_check(true);
    fixture.jobs.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.checks.size() == 1);
    const auto& [result, error, manual] = fixture.checks[0];
    CHECK(error.empty());
    CHECK(manual);
    REQUIRE(result.has_value());
    CHECK(result->status == update::UpdateStatus::UpdateAvailable);
#ifdef _WIN32
    CHECK(result->apply_mode == update::ApplyMode::Setup);
#endif
    // A success waits the full interval.
    CHECK(fixture.runtime.check_timer_delay() ==
          std::chrono::milliseconds(update::kUpdateCheckInterval));
}

TEST_CASE("a build without an update source says so and never schedules", "[update]") {
    Fixture fixture;
    fixture.feed.source = update::UpdateSource{};
    fixture.runtime.schedule_check(true);
    fixture.ui.advance(update::kUpdateCheckDelay);
    CHECK_FALSE(fixture.runtime.check_timer_delay());
    CHECK(fixture.jobs.queued() == 0);
    fixture.runtime.request_check(true);
    fixture.jobs.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.checks.size() == 1);
    CHECK(std::get<1>(fixture.checks[0]) == "This build of OpenWhisper_CPP has no update source.");
    CHECK(fixture.client.requests().empty());
}

TEST_CASE("a download hands off the verified setup with its command line", "[update]") {
    Fixture fixture;
    fixture.client.routes[setup_asset().url] = ok(kBody);
    fixture.runtime.request_download(available(release_with(setup_asset())));
    fixture.components.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.finished.size() == 1);
    const auto& [handoff, error] = fixture.finished[0];
    CHECK(error.empty());
    REQUIRE(handoff.has_value());
    const auto updates = fixture.root.path() / "updates";
    CHECK(handoff->setup_exe == updates / setup_asset().name);
    CHECK(read_bytes(handoff->setup_exe) == kBody);
    CHECK(handoff->args == update::setup_command(handoff->setup_exe, fixture.probe, updates).args);
    CHECK_FALSE(fixture.progress.empty());
    CHECK(fixture.progress.back() == "verifying 26/26");
}

TEST_CASE("a download without a release, and a failed one, report why", "[update]") {
    Fixture fixture;
    auto nothing = available(release_with(setup_asset()));
    nothing.release.reset();
    fixture.runtime.request_download(nothing);
    fixture.components.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.finished.size() == 1);
    CHECK(fixture.finished[0].second == "No installer is available.");

    // A development build cannot run the setup.
    fixture.probe.running_dir = "D:/build/bin";
    fixture.runtime.request_download(available(release_with(setup_asset())));
    fixture.components.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.finished.size() == 2);
    CHECK(fixture.finished[1].second == update::cannot_install_message());
}

TEST_CASE("cancel stops a download between pieces", "[update]") {
    Fixture fixture;
    fv::http::ScriptedResponse slow = ok(kBody);
    slow.chunks = {4, 4, 4};
    slow.on_chunk = [&fixture] {
        fixture.runtime.cancel_download();
    };
    fixture.client.routes[setup_asset().url] = slow;
    fixture.runtime.request_download(available(release_with(setup_asset())));
    fixture.components.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.finished.size() == 1);
    CHECK_FALSE(fixture.finished[0].first.has_value());
    CHECK(fixture.finished[0].second == "The update was canceled.");
    CHECK(fixture.progress.empty());
    CHECK_FALSE(std::filesystem::exists(fixture.root.path() / "updates" / setup_asset().name));
}

TEST_CASE("leftovers are pruned on the components strand", "[update]") {
    Fixture fixture;
    const auto updates = fixture.root.path() / "updates";
    std::filesystem::create_directories(updates);
    write_bytes(updates / "OpenWhisper_CPP-Setup-0.0.9.exe", "old");
    // Older than the keep window by the runtime's clock.
    fixture.clock.advance(std::chrono::hours(24 * 365 * 30));
    fixture.runtime.prune_leftovers();
    CHECK(std::filesystem::exists(updates / "OpenWhisper_CPP-Setup-0.0.9.exe"));
    fixture.components.run_all();
    CHECK_FALSE(std::filesystem::exists(updates / "OpenWhisper_CPP-Setup-0.0.9.exe"));
}

TEST_CASE("results that arrive after the runtime is gone are dropped", "[update]") {
    FakeFeed client{std::string(kRepo)};
    client.api_error = FakeFeed::offline();
    const auto feed = client.config();
    const auto probe = installed_probe();
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    fv::ManualExecutor ui;
    fv::ManualExecutor jobs;
    fv::ManualExecutor components;
    fv::testkit::TempDataRoot root;
    int delivered = 0;
    {
        update::UpdaterRuntime runtime(
            update::CheckContext{client, feed, settings, probe, clock, {}, "0.1.0"}, root.path(),
            ui, jobs, components, clock, "win32");
        auto subscription = runtime.check_finished.connect(
            ui, [&](const auto&, const auto&, bool) { ++delivered; });
        runtime.request_check(true);
        jobs.run_all();
        CHECK(ui.queued() == 1);
    }
    // The result's task still runs on the UI executor, and finds no runtime to deliver to.
    CHECK(ui.run_all() == 1);
    CHECK(delivered == 0);
}
