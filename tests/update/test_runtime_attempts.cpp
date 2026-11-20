// Download attempts through the updater runtime (updater.py's attempt counter and its error
// fallback; audit A04-76): a second download supersedes the first, whose prepared update is
// discarded unreported, and a failure without a message says "The download failed.".

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "download_support.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "update/apply.h"
#include "update/apply_error.h"
#include "update/runtime.h"
#include "update/testing/fake_feed.h"

namespace update = fv::update;
using update::testing::FakeFeed;
using namespace fv::update::test;

namespace {

// A runtime over native update actions the test scripts; its executors run when the test runs
// them.
struct Fixture {
    explicit Fixture(update::NativeUpdateActions native)
        : runtime(update::CheckContext{client, feed, settings, probe, clock, {}, "0.1.0"},
                  root.path() / "updates", ui, jobs, components, clock, "win32",
                  std::move(native)) {
        finished_subscription = runtime.download_finished.connect(
            ui, [this](std::optional<update::Handoff> handoff, std::string error) {
                finished.emplace_back(std::move(handoff), std::move(error));
            });
    }

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
    std::vector<std::pair<std::optional<update::Handoff>, std::string>> finished;
    fv::Subscription finished_subscription;
};

// A release whose native archive is ready, as a check in native mode found it; `client` serves
// the archive.
update::UpdateCheckResult native_release(FakeFeed& client) {
    auto result = available(release_with(setup_asset()));
    auto archive = setup_asset();
    archive.name = update::archive_asset_name(result.release->version);
    archive.url = download_url(result.release->version, archive.name);
    result.release->native_asset = archive;
    result.apply_mode = update::ApplyMode::Native;
    result.current_version = "0.1.0";
    client.routes[archive.url] = ok(kBody);
    return result;
}

// Native actions whose prepare fails with `failure`.
update::NativeUpdateActions failing(std::function<void()> failure) {
    update::NativeUpdateActions actions;
    actions.prepare = [failure = std::move(failure)](const std::filesystem::path&, std::string_view,
                                                     std::string_view, std::string_view,
                                                     const fv::CancelToken&) -> update::Handoff {
        failure();
        return {};
    };
    return actions;
}

} // namespace

TEST_CASE("a second download supersedes the first and discards what it prepared", "[update]") {
    int prepared = 0;
    std::vector<std::string> discarded;
    update::NativeUpdateActions actions;
    actions.prepare = [&prepared](const std::filesystem::path&, std::string_view, std::string_view,
                                  std::string_view, const fv::CancelToken&) {
        ++prepared;
        return update::Handoff{
            "helper.exe", {"--apply"}, "transaction-" + std::to_string(prepared)};
    };
    actions.discard = [&discarded](const update::Handoff& handoff) {
        discarded.push_back(handoff.transaction_id);
    };
    Fixture fixture(actions);
    const auto result = native_release(fixture.client);

    fixture.runtime.request_download(result);
    fixture.components.run_all(); // the first is prepared; its result waits for the UI
    fixture.runtime.request_download(result);
    fixture.ui.run_all();
    // The first's result arrived after the second began: not reported, its update discarded.
    CHECK(fixture.finished.empty());

    fixture.components.run_all();
    fixture.ui.run_all();
    CHECK(prepared == 2);
    CHECK(discarded == std::vector<std::string>{"transaction-1"});
    REQUIRE(fixture.finished.size() == 1);
    REQUIRE(fixture.finished[0].first.has_value());
    CHECK(fixture.finished[0].first->transaction_id == "transaction-2");
    CHECK(fixture.finished[0].second.empty());
}

TEST_CASE("a download that fails without a reason says the download failed", "[update]") {
    // An update error with no text (AppUpdateError's branch), and any other exception with none.
    const std::vector<std::pair<std::string, std::function<void()>>> failures{
        {"update error",
         [] {
             throw update::UpdateApplyError("");
         }},
        {"other error",
         [] {
             throw std::runtime_error("");
         }},
    };
    for (const auto& [name, failure] : failures) {
        INFO(name);
        Fixture fixture(failing(failure));
        fixture.runtime.request_download(native_release(fixture.client));
        fixture.components.run_all();
        fixture.ui.run_all();
        REQUIRE(fixture.finished.size() == 1);
        CHECK_FALSE(fixture.finished[0].first.has_value());
        CHECK(fixture.finished[0].second == "The download failed.");
    }
}
