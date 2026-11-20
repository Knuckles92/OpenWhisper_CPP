// Interrupted downloads and late results: OpenWhisper's tests/test_update_resilience.py (the
// resume rules of the setup download, and the attempt rules of the updater runtime).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "base/hash.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "download_support.h"
#include "http/testing/fake_http_client.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "update/apply.h"
#include "update/download.h"
#include "update/runtime.h"

namespace update = fv::update;
using fv::http::FakeHttpClient;
using fv::http::Request;
using namespace fv::update::test;

namespace {

constexpr std::string_view kComplete = "complete-update";

// _download_verified("https://github.com/update.exe", ...) as the Python fixture calls it.
update::ReleaseAsset update_exe() {
    update::ReleaseAsset asset;
    asset.name = "update.exe";
    asset.url = "https://github.com/update.exe";
    asset.size_bytes = static_cast<std::int64_t>(kComplete.size());
    asset.sha256 = fv::sha256_hex(kComplete);
    return asset;
}

std::filesystem::path fetch(FakeHttpClient& client, const std::filesystem::path& dir) {
    return update::download_release_asset(client, github_feed(), update_exe(), dir);
}

struct Collected {
    std::vector<std::pair<std::optional<update::Handoff>, std::string>> finished;
};

// An updater runtime over manual executors, a fake feed and a temporary updates folder.
struct Runtime {
    Runtime()
        : feed(github_feed()), probe(installed_probe()), ui(clock), jobs(clock), components(clock),
          runtime(update::CheckContext{client, feed, settings, probe, clock, {}, "0.1.0"},
                  root.path(), ui, jobs, components, clock, "win32") {
        subscription = runtime.download_finished.connect(
            ui, [this](std::optional<update::Handoff> handoff, std::string error) {
                collected.finished.emplace_back(std::move(handoff), std::move(error));
            });
    }

    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    update::FeedConfig feed;
    update::InstallProbe probe;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    fv::ManualExecutor ui;
    fv::ManualExecutor jobs;
    fv::ManualExecutor components;
    update::UpdaterRuntime runtime;
    Collected collected;
    fv::Subscription subscription;
};

} // namespace

// from tests/test_update_resilience.py::test_complete_partial_is_verified_without_range_request
TEST_CASE("complete partial is verified without range request", "[test_update_resilience]") {
    fv::testkit::TempDataRoot root;
    write_bytes(root.path() / "update.exe.part", kComplete);
    FakeHttpClient client;
    CHECK(read_bytes(fetch(client, root.path())) == kComplete);
    CHECK(client.requests().empty());
}

// from tests/test_update_resilience.py::test_wrong_complete_partial_restarts
TEST_CASE("wrong complete partial restarts", "[test_update_resilience]") {
    fv::testkit::TempDataRoot root;
    write_bytes(root.path() / "update.exe.part", std::string(kComplete.size(), 'x'));
    FakeHttpClient client;
    client.set_handler([](const Request&) { return ok(kComplete, {}); });
    CHECK(read_bytes(fetch(client, root.path())) == kComplete);
    REQUIRE(client.requests().size() == 1);
    CHECK_FALSE(client.requests()[0].range_from);
}

// from tests/test_update_resilience.py::test_range_416_restarts_once
TEST_CASE("range 416 restarts once", "[test_update_resilience]") {
    fv::testkit::TempDataRoot root;
    write_bytes(root.path() / "update.exe.part", "complete");
    FakeHttpClient client;
    client.push(status(416, "range"));
    client.push(ok(kComplete, {}));
    CHECK(read_bytes(fetch(client, root.path())) == kComplete);
    CHECK(client.requests().size() == 2);
}

// from tests/test_update_resilience.py::test_short_response_keeps_prefix_and_resumes
TEST_CASE("short response keeps prefix and resumes", "[test_update_resilience]") {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    // No Content-Length, as the Python fake sends none: the shortfall shows only at the end.
    fv::http::ScriptedResponse short_answer;
    short_answer.body = "complete";
    client.push(short_answer);
    try {
        fetch(client, root.path());
        FAIL("the short download was accepted");
    } catch (const update::AppUpdateError& error) {
        CHECK(std::string(error.what()).find("did not complete") != std::string::npos);
    }
    CHECK(read_bytes(root.path() / "update.exe.part") == "complete");

    fv::http::ScriptedResponse rest;
    rest.status = 206;
    rest.reason = "Partial Content";
    rest.headers = {{"Content-Range", "bytes 8-14/15"}};
    rest.body = "-update";
    client.push(rest);
    CHECK(read_bytes(fetch(client, root.path())) == kComplete);
    CHECK(client.requests().back().range_from == 8u);
}

// from
// tests/test_update_resilience.py::test_integrity_failure_does_not_automatically_install_something_else
TEST_CASE("integrity failure does not automatically install something else",
          "[test_update_resilience]") {
    // The setup path's counterpart of Python's native-mode test: a setup that fails its digest
    // is reported, and nothing else is fetched in its place.
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    client.set_handler([](const Request&) { return ok(std::string(kBody.size(), 'x')); });
    const auto feed = github_feed();
    const auto probe = installed_probe();
    const auto context = update::DownloadContext{client, feed, probe, root.path(), "win32"};
    try {
        update::apply_update(context, available(release_with(setup_asset())));
        FAIL("an unverified setup was handed off");
    } catch (const update::AppUpdateError& error) {
        CHECK(std::string(error.what()).find("integrity") != std::string::npos);
    }
    REQUIRE(client.requests().size() == 1);
    CHECK(client.requests()[0].url == setup_asset().url);
    CHECK(folder_names(root.path()).empty());
}

// from tests/test_update_resilience.py::test_queued_success_after_cancel_cannot_launch
TEST_CASE("queued success after cancel cannot launch", "[test_update_resilience]") {
    Runtime fixture;
    fixture.client.set_handler(
        [](const Request& request) { return FakeHttpClient::serve(request, kBody); });
    fixture.runtime.request_download(available(release_with(setup_asset())));
    fixture.components.run_all();
    REQUIRE(fixture.ui.queued() > 0); // the success is on its way to the UI
    fixture.runtime.cancel_download();
    fixture.ui.run_all();
    REQUIRE(fixture.collected.finished.size() == 1);
    CHECK_FALSE(fixture.collected.finished[0].first.has_value());
    CHECK(fixture.collected.finished[0].second == "The update was canceled.");
    // The hand-off went back to the components strand to be discarded.
    CHECK(fixture.components.queued() == 1);
    fixture.components.run_all();
}

// from tests/test_update_resilience.py::test_stale_attempt_cannot_change_current_dialog
TEST_CASE("stale attempt cannot change current dialog", "[test_update_resilience]") {
    Runtime fixture;
    fixture.client.set_handler(
        [](const Request& request) { return FakeHttpClient::serve(request, kBody); });
    update::UpdateCheckResult nothing = available(release_with(setup_asset()));
    nothing.release.reset();
    fixture.runtime.request_download(nothing);
    fixture.components.run_all(); // the old attempt's error is on its way to the UI
    fixture.runtime.request_download(available(release_with(setup_asset())));
    fixture.ui.run_all();
    CHECK(fixture.collected.finished.empty());
    fixture.components.run_all();
    fixture.ui.run_all();
    REQUIRE(fixture.collected.finished.size() == 1);
    CHECK(fixture.collected.finished[0].second.empty());
    REQUIRE(fixture.collected.finished[0].first.has_value());
    CHECK(fixture.collected.finished[0].first->setup_exe ==
          fixture.root.path() / setup_asset().name);
}
