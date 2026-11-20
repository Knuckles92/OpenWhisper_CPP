// From a found release to the setup command (services/app_update.py apply_update,
// ui_qt/ui_controller.py _setup_installer_command), and the error a failed update leaves for the
// next launch (services/app_update_apply.py write_apply_error, consume_apply_error).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "download_support.h"
#include "http/testing/fake_http_client.h"
#include "testkit/testkit.h"
#include "update/apply.h"
#include "update/apply_error.h"

namespace update = fv::update;
using fv::http::FakeHttpClient;
using fv::http::Request;
using namespace fv::update::test;

namespace {

std::string failure_of(const std::function<void()>& action) {
    try {
        action();
    } catch (const update::AppUpdateError& error) {
        return error.what();
    }
    FAIL("no AppUpdateError");
    return {};
}

} // namespace

TEST_CASE("the setup runs silently over the running install", "[update]") {
    const auto probe = installed_probe("C:\\Users\\u\\AppData\\Local\\Programs\\OpenWhisper_CPP");
    const std::filesystem::path updates = "C:\\Users\\u\\AppData\\Local\\OpenWhisper_CPP\\updates";
    const auto setup = updates / "OpenWhisper_CPP-Setup-0.1.1.exe";
    const auto handoff = update::setup_command(setup, probe, updates);
    CHECK(handoff.setup_exe == setup);
    const std::vector<std::string> expected{
        "/SP-",
        "/SILENT",
        "/NORESTART",
        "/OPENWHISPER_CPPUPDATE=1",
        "/CURRENTUSER",
        "/DIR=" + probe.running_dir.string(),
        "/LOG=" + (updates / "setup.log").string(),
    };
    CHECK(handoff.args == expected);
    CHECK(update::setup_update_switch() == "/OPENWHISPER_CPPUPDATE=1");
}

TEST_CASE("apply_update hands off the verified setup or says why not", "[update]") {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    client.set_handler(
        [](const Request& request) { return FakeHttpClient::serve(request, kBody); });
    const auto feed = github_feed();
    const auto probe = installed_probe();
    const auto updates = root.path() / "updates";
    const auto context = update::DownloadContext{client, feed, probe, updates, "win32"};

    auto nothing = available(release_with(setup_asset()));
    nothing.release.reset();
    CHECK(failure_of([&] { update::apply_update(context, nothing); }) ==
          "No installer is available.");

    // Notify only (no digest, or not installable here): nothing to run...
    auto notify = available(release_with(setup_asset()));
    notify.apply_mode = update::ApplyMode::NotifyOnly;
    CHECK(failure_of([&] { update::apply_update(context, notify); }) ==
          update::cannot_install_message());
    CHECK(client.requests().empty());

    // ...unless the setup is asked for and this copy can run it.
    const auto forced = update::apply_update(context, notify, {}, {}, true);
    CHECK(forced.setup_exe == updates / setup_asset().name);
    CHECK(read_bytes(forced.setup_exe) == kBody);
    CHECK(forced == update::setup_command(forced.setup_exe, probe, updates));

    // A caller without a native adapter can still use a verified setup.
    auto native = available(release_with(setup_asset()));
    native.apply_mode = update::ApplyMode::Native;
    CHECK(update::apply_update(context, native).setup_exe == forced.setup_exe);
    update::NativeUpdateActions nativeActions;
    auto package = setup_asset();
    package.name = update::archive_asset_name(native.release->version);
    package.url = download_url(native.release->version, package.name);
    native.release->native_asset = package;
    native.current_version = "0.1.0";
    const update::Handoff prepared{root.path() / "helper.exe", {"--apply"}, "test-transaction"};
    int count = 0;
    nativeActions.prepare = [&](const std::filesystem::path& archive, std::string_view digest,
                                std::string_view current, std::string_view next,
                                const fv::CancelToken&) {
        ++count;
        CHECK(read_bytes(archive) == kBody);
        CHECK(archive.filename() == package.name);
        CHECK(digest == *package.sha256);
        CHECK(current == "0.1.0");
        CHECK(next == native.release->version);
        return prepared;
    };
    CHECK(update::apply_update(context, native, {}, {}, false, nativeActions) == prepared);
    CHECK(count == 1);
    CHECK(update::apply_update(context, native, {}, {}, true, nativeActions) == forced);
    CHECK(count == 1);
    int abandoned = 0;
    nativeActions.discard = [&](const update::Handoff& result) {
        ++abandoned;
        CHECK(result == prepared);
    };
    update::discard_prepared_result(forced, nativeActions);
    update::discard_prepared_result(prepared, nativeActions);
    CHECK(abandoned == 1);

    // A release whose setup has no digest cannot be forced.
    auto unverified_asset = setup_asset();
    unverified_asset.sha256.reset();
    CHECK(failure_of([&] {
              update::apply_update(context, available(release_with(unverified_asset)), {}, {},
                                   true);
          }) == update::cannot_install_message());
}

namespace {

// A release with a ready native archive beside its setup, as a check found it in native mode.
struct NativeRelease {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    update::FeedConfig feed = github_feed();
    update::InstallProbe probe = installed_probe();
    std::filesystem::path updates = root.path() / "updates";
    update::UpdateCheckResult result = available(release_with(setup_asset()));
    update::ReleaseAsset archive = setup_asset();
    NativeRelease() {
        client.set_handler(
            [](const Request& request) { return FakeHttpClient::serve(request, kBody); });
        archive.name = update::archive_asset_name(result.release->version);
        archive.url = download_url(result.release->version, archive.name);
        result.release->native_asset = archive;
        result.apply_mode = update::ApplyMode::Native;
    }
    update::DownloadContext context() {
        return update::DownloadContext{client, feed, probe, updates, "win32"};
    }
    // The assets fetched, by file name, in order.
    std::vector<std::string> fetched() const {
        std::vector<std::string> names;
        for (const auto& request : client.requests()) {
            names.push_back(request.url.substr(request.url.rfind('/') + 1));
        }
        return names;
    }
};

update::NativeUpdateActions failing_with(std::function<void()> failure, int& calls) {
    update::NativeUpdateActions actions;
    actions.prepare = [failure = std::move(failure),
                       &calls](const std::filesystem::path&, std::string_view, std::string_view,
                               std::string_view, const fv::CancelToken&) -> update::Handoff {
        ++calls;
        failure();
        return {};
    };
    return actions;
}

} // namespace

// from
// tests/test_update_resilience.py::test_incompatible_native_payload_automatically_uses_verified_setup
TEST_CASE("incompatible native payload automatically uses verified setup",
          "[test_update_resilience]") {
    NativeRelease f;
    int calls = 0;
    const auto native =
        failing_with([] { throw update::UpdateRequiresSetup("new topology"); }, calls);
    const auto handoff = update::apply_update(f.context(), f.result, {}, {}, false, native);
    CHECK(calls == 1);
    // The archive was tried first; what is handed off is the verified setup, run as any setup.
    CHECK(f.fetched() == std::vector<std::string>{f.archive.name, setup_asset().name});
    CHECK(handoff.setup_exe == f.updates / setup_asset().name);
    CHECK(read_bytes(handoff.setup_exe) == kBody);
    CHECK(handoff == update::setup_command(handoff.setup_exe, f.probe, f.updates));
    CHECK(handoff.transaction_id.empty());

    // Without a ready setup there is nothing to fall back to: the reason is the user's to read.
    f.result.release->setup_asset->sha256.reset();
    CHECK(failure_of([&] { update::apply_update(f.context(), f.result, {}, {}, false, native); }) ==
          "new topology");
}

// The native half of
// tests/test_update_resilience.py::test_integrity_failure_does_not_automatically_install_something_else
TEST_CASE("a native preparation that fails for any other reason installs nothing else",
          "[update]") {
    NativeRelease f;
    int calls = 0;
    CHECK(failure_of([&] {
              update::apply_update(
                  f.context(), f.result, {}, {}, false,
                  failing_with([] { throw update::UpdateApplyError("hash mismatch"); }, calls));
          }) == "hash mismatch");
    CHECK(f.fetched() == std::vector<std::string>{f.archive.name});

    // A cancel during the preparation is worded as every canceled update is.
    CHECK(failure_of([&] {
              update::apply_update(
                  f.context(), f.result, {}, {}, false,
                  failing_with([] { throw update::UpdateCanceled("The update was canceled."); },
                               calls));
          }) == "The update was canceled.");
    fv::CancelSource cancel;
    CHECK(failure_of([&] {
              update::apply_update(f.context(), f.result, {}, cancel.token(), false,
                                   failing_with(
                                       [&] {
                                           cancel.cancel();
                                           throw std::runtime_error("interrupted read");
                                       },
                                       calls));
          }) == "The update was canceled.");
    CHECK(calls == 3);
    // No setup was fetched in any of these.
    for (const auto& name : f.fetched()) {
        CHECK(name == f.archive.name);
    }
}

// from tests/test_app_update_apply.py::TestPreserveAndEligibility::test_hklm_is_not_native_eligible
TEST_CASE("an all-users install is never prepared natively", "[update]") {
    NativeRelease f;
    f.probe.all_users = true;
    int calls = 0;
    const auto native = failing_with([] {}, calls);
    CHECK(failure_of([&] { update::apply_update(f.context(), f.result, {}, {}, false, native); }) ==
          update::cannot_install_message());
    CHECK(calls == 0);
    CHECK(f.client.requests().empty());
    // Its updates go through the setup, with /ALLUSERS.
    f.result.apply_mode = update::ApplyMode::Setup;
    const auto handoff = update::apply_update(f.context(), f.result, {}, {}, false, native);
    CHECK(std::ranges::find(handoff.args, "/ALLUSERS") != handoff.args.end());
    CHECK(calls == 0);
}

TEST_CASE("an apply error is read once, trimmed and cut", "[update]") {
    fv::testkit::TempDataRoot root;
    const auto updates = root.path() / "updates";
    CHECK_FALSE(update::consume_apply_error(updates));

    update::write_apply_error(updates, "  The update could not replace openwhisper-cpp.exe.  \n");
    const auto raw = read_bytes(update::apply_error_path(updates));
#ifdef _WIN32
    CHECK(raw == "The update could not replace openwhisper-cpp.exe.\r\n");
#else
    CHECK(raw == "The update could not replace openwhisper-cpp.exe.\n");
#endif
    CHECK(update::consume_apply_error(updates) ==
          "The update could not replace openwhisper-cpp.exe.");
    CHECK_FALSE(std::filesystem::exists(update::apply_error_path(updates)));
    CHECK_FALSE(update::consume_apply_error(updates));

    update::write_apply_error(updates, "");
    CHECK(update::consume_apply_error(updates) == "The update failed.");

    // Characters, not bytes: 2,500 two-byte characters keep 2,000.
    std::string long_text;
    for (int i = 0; i < 2500; ++i) {
        long_text += "\xC3\xA9"; // é
    }
    update::write_apply_error(updates, long_text);
    const auto cut = update::consume_apply_error(updates);
    REQUIRE(cut.has_value());
    CHECK(cut->size() == 4000);

    // A file written elsewhere: CRLF read as LF, blank means nothing.
    write_bytes(update::apply_error_path(updates), "line one\r\nline two\r\n");
    CHECK(update::consume_apply_error(updates) == "line one\nline two");
    write_bytes(update::apply_error_path(updates), " \r\n\t");
    CHECK_FALSE(update::consume_apply_error(updates));
    CHECK_FALSE(std::filesystem::exists(update::apply_error_path(updates)));

    // Not UTF-8: unreadable, and left where it is.
    write_bytes(update::apply_error_path(updates), "bad \xFF byte");
    CHECK_FALSE(update::consume_apply_error(updates));
    CHECK(std::filesystem::exists(update::apply_error_path(updates)));
}

TEST_CASE("a hand-off is discarded without touching the verified setup", "[update]") {
    fv::testkit::TempDataRoot root;
    const auto setup = root.path() / "OpenWhisper_CPP-Setup-0.1.1.exe";
    write_bytes(setup, "setup");
    update::discard_prepared_result(update::Handoff{setup, {"/SILENT"}});
    CHECK(read_bytes(setup) == "setup");
}
