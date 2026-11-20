// A speech runtime's install from its release archive, and the changes to a runtime this process
// has loaded, which wait for the next start (architecture 5.7: a loaded runtime is never
// unloaded, so its folder is never replaced while the app runs).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <tuple>
#include <vector>

#include "base/hash.h"
#include "base/identity.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/component_leases.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/downloads_model.h"
#include "models_test_support.h"
#include "pyport/json.h"

using namespace fv::models;
using namespace fv::models::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::http::FakeHttpClient;

namespace {

constexpr std::string_view kRuntime = "asr-nvidia-cpu";

struct Fixture {
    TempDir temp;
    fs::path root = temp.path() / "components";
    FakeHttpClient client;
    ComponentLeases leases;
    ComponentStoreOptions options;
    std::string archive;

    Fixture() {
        options.host = host_for(std::string(platform::kWinAmd64));
        options.leases = &leases;
        options.free_bytes = [](const fs::path&) {
            return std::uint64_t{1} << 40;
        };
        serve(stored_zip({{"bin/nemo_speech_asr_c.dll", "runtime v2"}, {"bin/ggml.dll", "ggml"}}));
    }

    void serve(std::string body) {
        archive = std::move(body);
        client.set_handler([this](const fv::http::Request& request) {
            return FakeHttpClient::serve(request, archive);
        });
    }

    // The catalog's shape for the CPU runtime, pinned to `archive`.
    Json entry(std::string version = "0.2.0") const {
        return Json{{"version", std::move(version)},
                    {"component_api", kComponentApi},
                    {"platform", "win_amd64"},
                    {"install_bytes", 1000},
                    {"archives", Json::array({Json{{"name", "nemo-speech-cpu.zip"},
                                                   {"url", "https://example.invalid/cpu.zip"},
                                                   {"sha256", fv::sha256_hex(archive)},
                                                   {"size_bytes", archive.size()},
                                                   {"extract", "zip"}}})}};
    }

    ComponentStore store() { return ComponentStore(root, client, options); }

    fs::path cached_archive() const { return root / ".cache" / "nemo-speech-cpu.zip"; }
};

} // namespace

TEST_CASE("a runtime installs from its archive, markers last and the cache dropped after commit",
          "[models]") {
    Fixture f;
    auto store = f.store();
    std::vector<std::string> phases;
    const auto outcome = store.install(kRuntime, f.entry(),
                                       [&](std::string_view phase, std::int64_t, std::int64_t) {
                                           if (phases.empty() || phases.back() != phase) {
                                               phases.emplace_back(phase);
                                           }
                                       },
                                       {});
    CHECK(outcome == ChangeOutcome::Done);
    CHECK(phases == std::vector<std::string>{"resolving", "downloading", "verifying", "extracting",
                                             "finalizing"});

    const auto tree = f.root / kRuntime;
    CHECK(read_file(tree / "bin" / "nemo_speech_asr_c.dll") == "runtime v2");
    CHECK(read_file(tree / ".installed") == "0.2.0");
    // json.dump(entry, indent=2) through a text-mode file.
    const std::string manifest = read_file(tree / "manifest.json");
    CHECK(fv::py::loads(manifest) == f.entry());
#ifdef _WIN32
    CHECK_THAT(manifest, ContainsSubstring("{\r\n  \"version\": \"0.2.0\",\r\n"));
#else
    CHECK_THAT(manifest, ContainsSubstring("{\n  \"version\": \"0.2.0\",\n"));
#endif
    CHECK(store.is_installed(kRuntime));
    CHECK_FALSE(fs::exists(f.cached_archive()));
    CHECK_FALSE(fs::exists(f.root / "asr-nvidia-cpu.old"));
    CHECK(fs::is_empty(store.staging_dir()));
}

TEST_CASE("a runtime archive without the library is refused and keeps its download", "[models]") {
    Fixture f;
    f.serve(stored_zip({{"bin/ggml.dll", "ggml"}}));
    auto store = f.store();
    CHECK_THROWS_WITH(store.install(kRuntime, f.entry(), {}, {}),
                      "The speech runtime is missing required files.");
    CHECK_FALSE(store.is_installed(kRuntime));
    CHECK_FALSE(fs::exists(f.root / kRuntime));
    // Cached archives are deleted only once an install commits.
    CHECK(fs::exists(f.cached_archive()));
}

TEST_CASE("a cached runtime archive is hashed again and fetched anew when it differs", "[models]") {
    Fixture f;
    write_file(f.cached_archive(), std::string(f.archive.size(), 'x'));
    auto store = f.store();
    store.install(kRuntime, f.entry(), {}, {});
    CHECK(f.client.requests().size() == 1);
    CHECK(read_file(f.root / kRuntime / "bin" / "nemo_speech_asr_c.dll") == "runtime v2");

    // A matching cached archive is used without a download.
    Fixture g;
    write_file(g.cached_archive(), g.archive);
    auto again = g.store();
    again.install(kRuntime, g.entry(), {}, {});
    CHECK(g.client.requests().empty());
    CHECK(again.is_installed(kRuntime));
}

TEST_CASE("an update of the loaded runtime is staged and applied at the next start", "[models]") {
    Fixture f;
    make_installed(f.root, kRuntime, Json{{"version", "0.1.0-py312"}});
    const auto release = f.leases.acquire(kRuntime);
    auto store = f.store();

    CHECK(store.install(kRuntime, f.entry(), {}, {}) == ChangeOutcome::StagedForRestart);
    // The loaded folder is untouched; the finished tree waits under .pending.
    CHECK(read_file(f.root / kRuntime / ".installed") == "0.1.0-py312");
    CHECK(fs::exists(f.root / kRuntime / "bin" / "fake.dll"));
    CHECK(read_file(store.pending_dir() / kRuntime / ".installed") == "0.2.0");
    CHECK_FALSE(fs::exists(f.cached_archive()));
    CHECK(ComponentCoordinator(store).describe(kRuntime).state == ComponentState::Installed);

    // The next start: nothing holds the runtime before it loads.
    release();
    auto next = f.store();
    next.settle_staged_changes();
    CHECK(read_file(f.root / kRuntime / ".installed") == "0.2.0");
    CHECK(read_file(f.root / kRuntime / "bin" / "nemo_speech_asr_c.dll") == "runtime v2");
    CHECK_FALSE(fs::exists(f.root / kRuntime / "bin" / "fake.dll"));
    CHECK_FALSE(fs::exists(next.pending_dir()));
    CHECK_FALSE(fs::exists(f.root / "asr-nvidia-cpu.old"));
}

TEST_CASE("staging an update of the loaded runtime waits out a scanner's lock", "[models]") {
    Fixture f;
    make_installed(f.root, kRuntime, Json{{"version", "0.1.0-py312"}});
    const auto release = f.leases.acquire(kRuntime);
    int locked = 2;
    f.options.runtime_swap_poll = std::chrono::milliseconds(1);
    f.options.file_ops.replace = [&locked](const fs::path& from, const fs::path& to) {
        if (to.parent_path().filename() == ".pending" && locked > 0) {
            --locked;
            // A scanner still reading the files: Windows' sharing violation.
            throw fs::filesystem_error("scanner still reading files", from, to,
                                       std::make_error_code(std::errc::permission_denied));
        }
        fs::rename(from, to);
    };
    auto store = f.store();
    CHECK(store.install(kRuntime, f.entry(), {}, {}) == ChangeOutcome::StagedForRestart);
    CHECK(locked == 0);
    CHECK(read_file(store.pending_dir() / kRuntime / ".installed") == "0.2.0");
    CHECK(read_file(f.root / kRuntime / ".installed") == "0.1.0-py312");
}

TEST_CASE("removing the loaded runtime is staged and done at the next start", "[models]") {
    Fixture f;
    make_installed(f.root, kRuntime, Json{{"version", "0.1.0-py312"}});
    const auto release = f.leases.acquire(kRuntime);
    auto store = f.store();
    CHECK(store.uninstall(kRuntime) == ChangeOutcome::StagedForRestart);
    CHECK(store.is_installed(kRuntime));
    CHECK(fs::exists(store.pending_dir() / "asr-nvidia-cpu.remove"));

    release();
    auto next = f.store();
    next.settle_staged_changes();
    CHECK_FALSE(next.is_installed(kRuntime));
    CHECK_FALSE(fs::exists(f.root / kRuntime));
    CHECK_FALSE(fs::exists(next.pending_dir()));
}

TEST_CASE("a staged tree that never finished is discarded at the next start", "[models]") {
    Fixture f;
    make_installed(f.root, kRuntime, Json{{"version", "0.1.0-py312"}});
    auto store = f.store();
    write_file(store.pending_dir() / kRuntime / "bin" / "nemo_speech_asr_c.dll", "half");
    store.settle_staged_changes();
    CHECK(read_file(f.root / kRuntime / ".installed") == "0.1.0-py312");
    CHECK_FALSE(fs::exists(store.pending_dir()));
}

TEST_CASE("a component being changed cannot be leased, nor a leased one changed", "[models]") {
    ComponentLeases leases;
    {
        const auto mutation = leases.mutation(kRuntime);
        CHECK_THROWS_WITH(leases.acquire(kRuntime),
                          "This component is being installed or removed. Try again when "
                          "Downloads finishes.");
        CHECK_THROWS_AS(leases.mutation(kRuntime), ComponentError);
    }
    const auto release = leases.acquire(kRuntime);
    CHECK(leases.in_use(kRuntime));
    CHECK_THROWS_WITH(leases.mutation(kRuntime),
                      "This component is in use. Finish the meeting or report job before "
                      "changing it.");
    release();
    release(); // releasing twice is harmless
    CHECK_FALSE(leases.in_use(kRuntime));
    CHECK_NOTHROW(leases.mutation(kRuntime));
}

TEST_CASE("an install of a component another install is changing is refused", "[models]") {
    Fixture f;
    const auto mutation = f.leases.mutation(kRuntime);
    auto store = f.store();
    CHECK_THROWS_WITH(store.install(kRuntime, f.entry(), {}, {}),
                      ContainsSubstring("being installed or removed") ||
                          ContainsSubstring("is in use"));
    CHECK(f.client.requests().empty());
}

TEST_CASE("describe() names a change staged for the next start, and the row offers nothing more",
          "[models]") {
    Fixture f;
    make_installed(f.root, kRuntime, Json{{"version", "0.1.0-py312"}});
    const auto release = f.leases.acquire(kRuntime);
    auto store = f.store();
    const ComponentCoordinator coordinator(store);
    CHECK(store.staged_change(kRuntime) == StagedChange::None);
    CHECK(coordinator.describe(kRuntime).staged == StagedChange::None);

    // An update of the loaded runtime.
    REQUIRE(store.install(kRuntime, f.entry(), {}, {}) == ChangeOutcome::StagedForRestart);
    CHECK(store.staged_change(kRuntime) == StagedChange::Install);
    ComponentInfo info = coordinator.describe(kRuntime);
    CHECK(info.staged == StagedChange::Install);
    CHECK(info.reason == std::string("Restart ") + std::string(fv::identity().product) +
                             " to finish installing the component.");
    // This run still uses the loaded tree.
    CHECK(info.installed_version == Json("0.1.0-py312"));
    ComponentRowState row = component_row_state(info, false, false);
    CHECK(row.badge_text == "Restart needed");
    CHECK(row.install_visible == false);
    CHECK_FALSE(row.remove_visible);
    CHECK_THAT(row.summary, ContainsSubstring("to finish installing the component."));

    // Then its removal, which replaces the staged update.
    REQUIRE(store.uninstall(kRuntime) == ChangeOutcome::StagedForRestart);
    CHECK(store.is_installed(kRuntime));
    info = coordinator.describe(kRuntime);
    CHECK(info.staged == StagedChange::Removal);
    CHECK(info.reason == std::string("Restart ") + std::string(fv::identity().product) +
                             " to finish removing the component.");
    row = component_row_state(info, false, false);
    CHECK(row.badge_text == "Restart needed");
    CHECK_FALSE(row.remove_visible);

    // Settled at the next start: nothing is staged any more.
    release();
    auto next = f.store();
    next.settle_staged_changes();
    CHECK(next.staged_change(kRuntime) == StagedChange::None);
    CHECK(ComponentCoordinator(next).describe(kRuntime).staged == StagedChange::None);
}
