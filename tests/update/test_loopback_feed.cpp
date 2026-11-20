// The update path end to end on this machine: the release tools' loopback feed
// (tools/release/serve_update_feed.py) serves a setup on 127.0.0.1, a check through the real HTTP
// client finds it, and the runtime downloads and verifies it into a temporary updates folder and
// hands off the setup command. Nothing leaves the loopback interface and nothing is installed.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "base/hash.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "download_support.h"
#include "http/client.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "update/release_feed.h"
#include "update/runtime.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace update = fv::update;
using namespace fv::update::test;

#if defined(_WIN32) && defined(FV_PYTHON) && defined(FV_FEED_SERVER)
namespace {

// The feed server as a child of this test, in a job that ends it when the test does (also when
// the test fails or crashes), with its output on a pipe.
class FeedServer {
public:
    FeedServer(const std::filesystem::path& setup, const std::string& version) {
        job_ = CreateJobObjectW(nullptr, nullptr);
        REQUIRE(job_ != nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        REQUIRE(SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits,
                                        sizeof(limits)));

        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE write = nullptr;
        REQUIRE(CreatePipe(&read_, &write, &inherit, 1 << 16));
        SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);

        const std::filesystem::path python = FV_PYTHON;
        const std::filesystem::path script = FV_FEED_SERVER;
        std::wstring command = L"\"" + python.wstring() + L"\" -I -B \"" + script.wstring() +
                               L"\" --setup \"" + setup.wstring() + L"\" --version " +
                               std::filesystem::path(version).wstring() + L" --port 0";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullptr;
        startup.hStdOutput = write;
        startup.hStdError = write;
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                                            &startup, &process);
        CloseHandle(write);
        REQUIRE(started);
        process_ = process.hProcess;
        AssignProcessToJobObject(job_, process_);
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
    }
    ~FeedServer() {
        TerminateJobObject(job_, 0);
        if (process_ != nullptr) {
            WaitForSingleObject(process_, 10'000);
            CloseHandle(process_);
        }
        CloseHandle(read_);
        CloseHandle(job_);
    }
    FeedServer(const FeedServer&) = delete;
    FeedServer& operator=(const FeedServer&) = delete;

    // The feed URL the server prints, or "" with what it printed when it stopped first.
    std::string feed_url(std::string& output) {
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read_, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
            output.append(buffer, got);
            const auto marker = output.find("UPDATE_FEED_URL = \"");
            if (marker != std::string::npos) {
                const auto start = marker + std::string_view("UPDATE_FEED_URL = \"").size();
                const auto end = output.find('"', start);
                if (end != std::string::npos) {
                    return output.substr(start, end - start);
                }
            }
        }
        return {};
    }

private:
    HANDLE job_ = nullptr;
    HANDLE read_ = nullptr;
    HANDLE process_ = nullptr;
};

} // namespace

TEST_CASE("a setup served on loopback is found, downloaded, verified and handed off",
          "[update][platform]") {
    fv::testkit::TempDataRoot root;
    const auto served = root.path() / "feed";
    std::filesystem::create_directories(served);
    const auto setup_name = update::setup_asset_name("0.1.1");
    std::string setup_bytes = "MZ stand-in for a setup, never run. ";
    for (int i = 0; i < 2000; ++i) {
        setup_bytes += std::to_string(i);
    }
    write_bytes(served / setup_name, setup_bytes);

    FeedServer server(served / setup_name, "0.1.1");
    std::string output;
    const auto url = server.feed_url(output);
    INFO(output);
    REQUIRE(url.starts_with("http://127.0.0.1:"));

    const fv::MapEnvReader env{{update::update_feed_env(), url}};
    const auto feed = update::feed_config(env);
    REQUIRE(feed.override_url == url);
    const auto install = root.path() / "install";
    std::filesystem::create_directories(install);
    const auto probe = installed_probe(install);
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    fv::ManualExecutor ui(clock);
    fv::ManualExecutor jobs;
    fv::ManualExecutor components;
    const auto client = fv::http::make_curl_client();
    const auto updates = root.path() / "updates";
    update::UpdaterRuntime runtime(
        update::CheckContext{*client, feed, settings, probe, clock, {}, "0.1.0"}, updates, ui, jobs,
        components, clock);

    std::optional<update::UpdateCheckResult> found;
    std::string check_error = "no answer";
    auto checks = runtime.check_finished.connect(
        ui, [&](std::optional<update::UpdateCheckResult> result, std::string error, bool) {
            found = std::move(result);
            check_error = std::move(error);
        });
    runtime.request_check(true);
    jobs.run_all();
    ui.run_all();
    REQUIRE(check_error.empty());
    REQUIRE(found.has_value());
    CHECK(found->status == update::UpdateStatus::UpdateAvailable);
    CHECK(found->apply_mode == update::ApplyMode::Setup);
    REQUIRE(found->release.has_value());
    CHECK(found->release->version == "0.1.1");

    std::optional<update::Handoff> handoff;
    std::string download_error = "no answer";
    auto downloads = runtime.download_finished.connect(
        ui, [&](std::optional<update::Handoff> result, std::string error) {
            handoff = std::move(result);
            download_error = std::move(error);
        });
    runtime.request_download(*found);
    components.run_all();
    ui.run_all();
    REQUIRE(download_error.empty());
    REQUIRE(handoff.has_value());
    CHECK(handoff->setup_exe == updates / setup_name);
    CHECK(read_bytes(handoff->setup_exe) == setup_bytes);
    CHECK(fv::sha256_hex(read_bytes(handoff->setup_exe)) == fv::sha256_hex(setup_bytes));
    const auto& args = handoff->args;
    CHECK(std::ranges::find(args, "/OPENWHISPER_CPPUPDATE=1") != args.end());
    CHECK(std::ranges::find(args, "/DIR=" + install.string()) != args.end());
}
#else
TEST_CASE("a setup served on loopback is found, downloaded, verified and handed off",
          "[update][platform]") {
    SKIP("needs Windows and the Python interpreter of the release tools");
}
#endif
