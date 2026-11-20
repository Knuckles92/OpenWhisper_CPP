// Rules of the component catalog and store that the ported OpenWhisper tests leave unpinned
// (map a14 A14-02, A14-04, A14-06, A14-07, A14-11, A14-12, A14-13, A14-65, A14-67): the platform
// tag table, every clause of the published gate, the macOS minimum, the install's own refusals
// and free-space sum, the disk error texts, the locked-swap retry and the cancel before
// finalizing.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <system_error>
#include <vector>

#include "base/cancel.h"
#include "base/hash.h"
#include "base/utf8.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/component_store.h"
#include "models/speaker_model.h"
#include "models_test_support.h"

using namespace fv::models;
using namespace fv::models::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::http::FakeHttpClient;

namespace {

struct Store {
    TempDir temp;
    fs::path root = temp.path() / "components";
    FakeHttpClient client;
    ComponentLeases leases;
    ComponentStoreOptions options;

    Store() {
        fs::create_directories(root);
        options.host = host_for(std::string(platform::kWinAmd64));
        options.leases = &leases;
        options.free_bytes = [](const fs::path&) {
            return std::uint64_t{1} << 40;
        };
    }
    ComponentStore make() { return ComponentStore(root, client, options); }
};

Json published_entry() {
    return Json{{"version", "1"},
                {"install_bytes", 10},
                {"archives", Json::array({Json{{"name", "a.zip"},
                                               {"url", "https://example.invalid/a.zip"},
                                               {"sha256", std::string(64, 'a')},
                                               {"size_bytes", 5}}})}};
}

Json runtime_entry(const std::string& archive) {
    return Json{{"version", "0.2.0"},
                {"component_api", kComponentApi},
                {"platform", "win_amd64"},
                {"install_bytes", 1000},
                {"archives", Json::array({Json{{"name", "nemo-speech-cpu.zip"},
                                               {"url", "https://example.invalid/cpu.zip"},
                                               {"sha256", fv::sha256_hex(archive)},
                                               {"size_bytes", archive.size()},
                                               {"extract", "zip"}}})}};
}

} // namespace

TEST_CASE("current_platform_tag maps the OS and architecture as OpenWhisper does", "[models]") {
    using O = std::optional<std::string>;
    CHECK(platform_tag("win32", "AMD64") == O("win_amd64"));
    CHECK(platform_tag("win32", " x86_64 ") == O("win_amd64"));
    CHECK(platform_tag("win32", "x64") == O("win_amd64"));
    CHECK(platform_tag("win32", "ARM64") == std::nullopt);
    CHECK(platform_tag("linux", "x86_64") == O("linux_x86_64"));
    CHECK(platform_tag("linux2", "amd64") == O("linux_x86_64"));
    CHECK(platform_tag("linux", "aarch64") == O("linux_aarch64"));
    CHECK(platform_tag("linux", "ARM64") == O("linux_aarch64"));
    CHECK(platform_tag("linux", "armv7l") == std::nullopt);
    CHECK(platform_tag("darwin", "arm64") == O("darwin_arm64"));
    CHECK(platform_tag("darwin", "aarch64") == O("darwin_arm64"));
    CHECK(platform_tag("darwin", "x86_64") == O("darwin_x86_64"));
    CHECK(platform_tag("darwin", "amd64") == O("darwin_x86_64"));
    CHECK(platform_tag("darwin", "ppc") == std::nullopt);
    CHECK(platform_tag("freebsd13", "amd64") == std::nullopt);
}

TEST_CASE("component_is_published refuses each placeholder an entry can carry", "[models]") {
    CHECK(component_is_published(published_entry()));
    // An explicit "published": true changes nothing; false withdraws the entry.
    auto entry = published_entry();
    entry["published"] = true;
    CHECK(component_is_published(entry));
    entry["published"] = false;
    CHECK_FALSE(component_is_published(entry));

    entry = published_entry();
    entry["archives"] = Json::array();
    CHECK_FALSE(component_is_published(entry));
    entry = published_entry();
    entry.erase("archives");
    CHECK_FALSE(component_is_published(entry));
    for (const Json& bytes : {Json(0), Json(-1), Json(nullptr)}) {
        entry = published_entry();
        entry["install_bytes"] = bytes;
        CHECK_FALSE(component_is_published(entry));
    }
    for (const Json& digest :
         {Json(std::string(63, 'a')), Json(std::string(64, '0')), Json(""), Json(nullptr)}) {
        entry = published_entry();
        entry["archives"][0]["sha256"] = digest;
        CHECK_FALSE(component_is_published(entry));
    }
    // The digest is compared in lower case: an upper-case one is still 64 hex characters.
    entry = published_entry();
    entry["archives"][0]["sha256"] = std::string(64, 'A');
    CHECK(component_is_published(entry));
    for (const Json& url : {Json("http://example.invalid/a.zip"), Json(""), Json(nullptr)}) {
        entry = published_entry();
        entry["archives"][0]["url"] = url;
        CHECK_FALSE(component_is_published(entry));
    }
    for (const Json& size : {Json(0), Json(-5), Json(nullptr)}) {
        entry = published_entry();
        entry["archives"][0]["size_bytes"] = size;
        CHECK_FALSE(component_is_published(entry));
    }
    // One bad archive among good ones withdraws the entry.
    entry = published_entry();
    Json second = entry["archives"][0];
    second["url"] = "ftp://example.invalid/b.zip";
    entry["archives"].push_back(second);
    CHECK_FALSE(component_is_published(entry));
}

TEST_CASE("check_compatibility words a macOS minimum this Mac does not meet", "[models]") {
    Host mac = host_for("darwin_arm64");
    const Json manifest{{"macos_min", "14.0"}, {"platform", "darwin_arm64"}};
    mac.macos_version = std::vector<int>{13, 6};
    CHECK(check_compatibility(manifest, mac) == std::optional<std::string>("Requires macOS 14.0 or "
                                                                           "later"));
    mac.macos_version = std::vector<int>{14, 0};
    CHECK_FALSE(check_compatibility(manifest, mac));
    mac.macos_version = std::vector<int>{15};
    CHECK_FALSE(check_compatibility(manifest, mac));
    // Off macOS there is no version to compare.
    mac.macos_version.reset();
    CHECK_FALSE(check_compatibility(manifest, mac));
    // The component API is checked first, then the platform.
    mac.macos_version = std::vector<int>{13, 6};
    CHECK(check_compatibility(Json{{"macos_min", "14.0"}, {"platform", "win_amd64"}}, mac) ==
          std::optional<std::string>("Built for win_amd64, which this app cannot use"));
}

TEST_CASE("an install refuses an entry without archives before anything else", "[models]") {
    Store s;
    auto store = s.make();
    for (const Json& archives : {Json::array(), Json(nullptr)}) {
        Json entry = runtime_entry("x");
        entry["archives"] = archives;
        entry["platform"] = "linux_x86_64"; // would be refused too, but later
        CHECK_THROWS_WITH(store.install("asr-nvidia-cpu", entry, {}, {}),
                          "The catalog entry lists no files to download.");
    }
    CHECK(s.client.requests().empty());
    CHECK_FALSE(fs::exists(store.staging_dir()));
}

TEST_CASE("the free-space check counts the download and the install with 15 percent spare",
          "[models]") {
    Store s;
    // 10 MB of archives and 30 MB installed need 46 MB; 45 MB is short, though either part
    // alone would fit.
    s.options.free_bytes = [](const fs::path&) {
        return std::uint64_t{45'000'000};
    };
    auto store = s.make();
    Json entry = runtime_entry("x");
    entry["install_bytes"] = 30'000'000;
    entry["archives"][0]["size_bytes"] = 10'000'000;
    std::vector<std::string> phases;
    CHECK_THROWS_WITH(store.install("asr-nvidia-cpu", entry,
                                    [&](std::string_view phase, std::int64_t, std::int64_t) {
                                        phases.emplace_back(phase);
                                    },
                                    {}),
                      "Not enough disk space: 46 MB needed, 45 MB free on this drive.");
    CHECK(phases == std::vector<std::string>{"resolving"});
    CHECK(s.client.requests().empty());
}

TEST_CASE("disk errors are worded for the user as _describe_disk_error words them", "[models]") {
    Store s;
    auto store = s.make();
    CHECK(store.describe_disk_error(std::system_error(std::make_error_code(
              std::errc::no_space_on_device))) == "The drive ran out of space while installing.");
    CHECK(store.describe_disk_error(
              std::system_error(std::make_error_code(std::errc::permission_denied))) ==
          "A file could not be written, which usually means antivirus software blocked it. Try "
          "adding an exclusion for " +
              fv::base_utf8::path_to_utf8(s.root) + ".");
    const std::string other = store.describe_disk_error(
        std::system_error(std::make_error_code(std::errc::no_such_file_or_directory)));
    CHECK(other.starts_with("Installation failed: [Errno "));
#ifdef _WIN32
    CHECK(store.describe_disk_error(std::system_error(std::error_code(
              112, std::system_category()))) == "The drive ran out of space while installing.");
    CHECK(store.describe_disk_error(std::system_error(std::error_code(5, std::system_category())))
              .starts_with("A file could not be written"));
#endif
}

TEST_CASE("a locked runtime swap retries for 15 s every 0.5 s and a cancel ends the wait",
          "[models]") {
    const ComponentStoreOptions defaults;
    CHECK(defaults.runtime_swap_timeout == std::chrono::seconds(15));
    CHECK(defaults.runtime_swap_poll == std::chrono::milliseconds(500));

    Store s;
    const fs::path from = s.root / "staged";
    const fs::path to = s.root / "asr-nvidia-cpu";
    write_file(from / "bin" / "nemo_speech_asr_c.dll", "x");
    fv::CancelSource cancel;
    int attempts = 0;
    s.options.file_ops.replace = [&](const fs::path& a, const fs::path& b) {
        ++attempts;
        // A scanner holds the files; the user cancels during the wait.
        cancel.cancel();
        throw fs::filesystem_error("locked", a, b,
                                   std::make_error_code(std::errc::permission_denied));
    };
    auto store = s.make();
    const auto started = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(store.replace_speech_runtime(from, to, cancel.token()), ComponentCanceled);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
    CHECK(attempts == 1);
    CHECK(fs::exists(from / "bin" / "nemo_speech_asr_c.dll"));
    CHECK_FALSE(fs::exists(to));
}

TEST_CASE("an install checks cancel after the payload is validated and always drops staging",
          "[models]") {
    const std::string archive = stored_zip({{"bin/nemo_speech_asr_c.dll", "runtime"}});
    Store s;
    s.client.set_handler([&archive](const fv::http::Request& request) {
        return FakeHttpClient::serve(request, archive);
    });
    auto store = s.make();
    fv::CancelSource cancel;
    std::vector<std::string> phases;
    const auto progress = [&](std::string_view phase, std::int64_t done, std::int64_t total) {
        if (phases.empty() || phases.back() != phase) {
            phases.emplace_back(phase);
        }
        // The last member is written: nothing is left to extract, so only the check before
        // FINALIZING can see this cancel.
        if (phase == install_phase::kExtracting && done == total) {
            cancel.cancel();
        }
    };
    CHECK_THROWS_AS(
        store.install("asr-nvidia-cpu", runtime_entry(archive), progress, cancel.token()),
        ComponentCanceled);
    CHECK(std::ranges::find(phases, "finalizing") == phases.end());
    CHECK_FALSE(store.is_installed("asr-nvidia-cpu"));
    CHECK_FALSE(fs::exists(s.root / "asr-nvidia-cpu"));
    CHECK(fs::is_empty(store.staging_dir()));
    // The verified archive stays cached for the next attempt.
    CHECK(fs::exists(store.cache_dir() / "nemo-speech-cpu.zip"));

    // A refused payload leaves no staging tree behind either.
    const std::string empty = stored_zip({{"bin/other.dll", "x"}});
    Store t;
    t.client.set_handler([&empty](const fv::http::Request& request) {
        return FakeHttpClient::serve(request, empty);
    });
    auto refusing = t.make();
    CHECK_THROWS_AS(refusing.install("asr-nvidia-cpu", runtime_entry(empty), {}, {}),
                    ComponentError);
    CHECK(fs::is_empty(refusing.staging_dir()));
}

TEST_CASE("the speaker model's verification texts are OpenWhisper's", "[models]") {
    CHECK(kSpeakerModelNoFile == "The speaker model download produced no file.");
    CHECK(kSpeakerModelIntegrity ==
          "The speaker model failed its integrity check and was discarded.");
}
