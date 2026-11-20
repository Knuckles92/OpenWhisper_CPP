// Installed components on disk and the coordinator: OpenWhisper's tests/test_components.py (the
// functions this build keeps) and two of tests/test_component_download.py. OpenWhisper's tests
// name gpu-accel and meeting-agent as ordinary components; this build has neither in its
// catalog, so those tests keep the ids as arbitrary names where the catalog does not matter, and
// use the NeMo runtimes where it does.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <type_traits>
#include <vector>

#include "base/hash.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models_test_support.h"

using namespace fv::models;
using namespace fv::models::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::http::FakeHttpClient;

namespace {

// The 22 bytes of an empty zip (only its end-of-central-directory record).
const std::string kEmptyZip = std::string("PK\x05\x06", 4) + std::string(18, '\0');

struct Fixture {
    TempDir temp;
    fs::path root = temp.path() / "components";
    FakeHttpClient client;
    ComponentLeases leases;

    explicit Fixture(std::optional<std::string> platform = std::string(platform::kWinAmd64)) {
        fs::create_directories(root);
        options.host = host_for(std::move(platform));
        options.leases = &leases;
        options.free_bytes = [](const fs::path&) {
            return std::uint64_t{1} << 40;
        };
    }

    ComponentStore store() { return ComponentStore(root, client, options); }

    ComponentStoreOptions options;
};

ComponentCoordinator::Options no_catalog(const Json& null_catalog) {
    ComponentCoordinator::Options options;
    options.catalog = &null_catalog;
    return options;
}

fs::filesystem_error os_error(std::errc code, const fs::path& from, const fs::path& to) {
    return fs::filesystem_error("simulated", from, to, std::make_error_code(code));
}

} // namespace

// Installation state

TEST_CASE("test_not_installed_when_directory_absent", "[test_components]") {
    Fixture f;
    CHECK_FALSE(f.store().is_installed("gpu-accel"));
}

TEST_CASE("test_partial_install_without_sentinel_is_not_installed", "[test_components]") {
    // A tree left behind by an interrupted extract must not count as usable.
    Fixture f;
    fs::create_directories(f.root / "gpu-accel" / "bin");
    CHECK_FALSE(f.store().is_installed("gpu-accel"));
}

TEST_CASE("test_installed_requires_sentinel", "[test_components]") {
    Fixture f;
    make_installed(f.root, "gpu-accel", Json{{"version", "1.0"}});
    CHECK(f.store().is_installed("gpu-accel"));
}

TEST_CASE("test_prune_orphans_removes_staging_and_rollback", "[test_components]") {
    Fixture f;
    fs::create_directories(f.root / ".staging" / "gpu-accel.123");
    fs::create_directories(f.root / "gpu-accel.old");
    const auto keep = f.root / "gpu-accel";
    fs::create_directories(keep);

    f.store().prune_orphans();

    CHECK_FALSE(fs::exists(f.root / ".staging"));
    CHECK_FALSE(fs::exists(f.root / "gpu-accel.old"));
    CHECK(fs::exists(keep));
}

TEST_CASE("test_uninstall_removes_the_tree", "[test_components]") {
    Fixture f;
    make_installed(f.root, "gpu-accel", Json{{"version", "1.0"}});
    auto store = f.store();
    store.uninstall("gpu-accel");
    CHECK_FALSE(store.is_installed("gpu-accel"));
}

// Compatibility gates

TEST_CASE("test_empty_manifest_is_compatible", "[test_components]") {
    CHECK_FALSE(check_compatibility(Json::object(), host_for("win_amd64")));
}

TEST_CASE("test_component_api_mismatch_is_rejected", "[test_components]") {
    const auto reason =
        check_compatibility(Json{{"component_api", kComponentApi + 1}}, host_for("win_amd64"));
    REQUIRE(reason);
    // OpenWhisper names itself here; OpenWhisper_CPP does.
    CHECK_THAT(*reason, ContainsSubstring("OpenWhisper_CPP"));
    CHECK(*reason ==
          "Built for a different version of OpenWhisper_CPP (component API 2, this app uses 1)");
}

TEST_CASE("test_foreign_platform_is_rejected", "[test_components]") {
    const auto reason =
        check_compatibility(Json{{"platform", "linux_x86_64"}}, host_for("win_amd64"));
    REQUIRE(reason);
    CHECK_THAT(*reason, ContainsSubstring("linux_x86_64"));
}

// Coordinator

TEST_CASE("test_begin_install_is_exclusive", "[test_components]") {
    Fixture f;
    auto store = f.store();
    ComponentCoordinator coordinator(store);
    CHECK(coordinator.begin_install("gpu-accel"));
    // A second request while one is in flight must be refused, not queued.
    CHECK_FALSE(coordinator.begin_install("gpu-accel"));

    coordinator.end_install("gpu-accel");
    CHECK(coordinator.begin_install("gpu-accel"));
}

TEST_CASE("test_cancel_all_sets_every_flag", "[test_components]") {
    Fixture f;
    auto store = f.store();
    ComponentCoordinator coordinator(store);
    const auto source = coordinator.begin_install("gpu-accel");
    REQUIRE(source);
    CHECK_FALSE(source->cancelled());

    coordinator.cancel_all();

    CHECK(source->cancelled());
}

TEST_CASE("test_describe_reports_states", "[test_components]") {
    Fixture f;
    auto store = f.store();
    const Json null_catalog;
    ComponentCoordinator coordinator(store, no_catalog(null_catalog));

    auto info = coordinator.describe("gpu-accel");
    CHECK(info.state == ComponentState::NotInstalled);
    CHECK_FALSE(info.is_usable());

    make_installed(f.root, "gpu-accel", Json{{"version", "1.0"}});
    info = coordinator.describe("gpu-accel");
    CHECK(info.state == ComponentState::Installed);
    CHECK(info.is_usable());
    REQUIRE(info.installed_version);
    CHECK(*info.installed_version == "1.0");
}

TEST_CASE("test_catalog_needs_no_network", "[test_components]") {
    // The catalog ships in the app; nothing is fetched to read it.
    Fixture f;
    auto store = f.store();
    const Json& catalog = ComponentCoordinator(store).fetch_catalog();

    const Json& entry =
        catalog.at("components").at(component_id::kAsrNvidiaCpu).at("platforms").at("win_amd64");
    REQUIRE_FALSE(entry.at("archives").empty());
    for (const Json& archive : entry.at("archives")) {
        CHECK(archive.at("url").get<std::string>().starts_with("https://github.com/"));
        CHECK_FALSE(archive.at("sha256").get<std::string>().empty());
    }
    CHECK(f.client.requests().empty());
}

TEST_CASE("test_available_component_ids_by_platform", "[test_components]") {
    // OpenWhisper's GPU Acceleration, meeting agents and other engines' runtimes are not in this
    // build's catalog: what remains is NVIDIA Speech per platform.
    using V = std::vector<std::string>;
    CHECK(available_component_ids(host_for("linux_x86_64")) ==
          V{"asr-nvidia-cpu", "asr-nvidia-cuda"});
    CHECK(available_component_ids(host_for("linux_aarch64")).empty());

    Host mac = host_for("darwin_arm64");
    mac.macos_version = std::vector<int>{15, 0};
    CHECK(available_component_ids(mac) == V{"asr-nvidia-cpu", "asr-nvidia-metal"});
    {
        Fixture f("darwin_arm64");
        auto store = f.store();
        CHECK(ComponentCoordinator(store).list_components().size() == 2);
    }
    mac.macos_version = std::vector<int>{14, 6};
    CHECK(available_component_ids(mac) == V{"asr-nvidia-cpu", "asr-nvidia-metal"});
    CHECK(available_component_ids("darwin_arm64", mac) == V{"asr-nvidia-cpu", "asr-nvidia-metal"});

    CHECK(available_component_ids(host_for("darwin_x86_64")) == V{"asr-nvidia-cpu"});
    {
        Fixture f("darwin_x86_64");
        auto store = f.store();
        CHECK(ComponentCoordinator(store).list_components().size() == 1);
    }

    // NVIDIA publishes the Vulkan speech runtime for Linux only, and Metal for Macs.
    CHECK(available_component_ids(host_for("win_amd64")) == V{"asr-nvidia-cpu", "asr-nvidia-cuda"});
    CHECK(available_component_ids(host_for(std::nullopt)).empty());
}

TEST_CASE("test_linux_offers_the_vulkan_speech_runtime_only_where_it_is_needed",
          "[test_components]") {
    std::set<std::string, std::less<>> installed;
    for (const auto& card : {std::optional<NvidiaGpu>{},
                             std::optional{gpu("NVIDIA GeForce RTX 2060", 6144, {{7, 5}})}}) {
        Host host = host_for("linux_x86_64", card);
        host.is_installed = [&](std::string_view id) {
            return installed.contains(id);
        };
        const auto ids = available_component_ids(host);
        CHECK(std::ranges::find(ids, "asr-nvidia-vulkan") == ids.end());
    }
    Host older = host_for("linux_x86_64", gpu("NVIDIA GeForce GTX 1050 Ti", 4096, {{6, 1}}));
    older.is_installed = [&](std::string_view id) {
        return installed.contains(id);
    };
    CHECK(available_component_ids(older) ==
          std::vector<std::string>{"asr-nvidia-cpu", "asr-nvidia-cuda", "asr-nvidia-vulkan"});
    // Once installed it stays listed, so it can be removed after a GPU upgrade.
    installed.emplace("asr-nvidia-vulkan");
    Host none = host_for("linux_x86_64");
    none.is_installed = [&](std::string_view id) {
        return installed.contains(id);
    };
    const auto ids = available_component_ids(none);
    CHECK(std::ranges::find(ids, "asr-nvidia-vulkan") != ids.end());
}

TEST_CASE("test_check_compatibility_rejects_cross_architecture", "[test_components]") {
    const Json manifest{{"component_api", 1}, {"platform", "linux_x86_64"}};
    const auto reason = check_compatibility(manifest, host_for("linux_aarch64"));
    REQUIRE(reason);
    CHECK_THAT(*reason, ContainsSubstring("linux_x86_64"));
    CHECK_FALSE(check_compatibility(manifest, host_for("linux_x86_64")));
}

TEST_CASE("test_read_manifest_rejects_malformed_shapes", "[test_components]") {
    Fixture f;
    auto store = f.store();
    const auto path = store.component_dir("meeting-agent") / "manifest.json";
    write_file(path, "[1,2,3]");
    CHECK_FALSE(store.read_manifest("meeting-agent"));
    write_file(path, R"({"platform": ["win_amd64"], "component_api": "x"})");
    CHECK_FALSE(store.read_manifest("meeting-agent"));
    // A bool is not an integer here, though Python's bool is an int subclass.
    write_file(path, R"({"component_api": true, "version": false})");
    CHECK_FALSE(store.read_manifest("meeting-agent"));
    write_file(path, R"({"component_api": 1, "version": true})");
    CHECK_FALSE(store.read_manifest("meeting-agent"));
}

TEST_CASE("test_boolean_manifest_cannot_activate_component", "[test_components]") {
    // OpenWhisper's startup activation (component_runtime) is not ported: a bool component_api
    // leaves the manifest unread, and describe reports the install broken.
    Fixture f;
    auto store = f.store();
    const auto target = store.component_dir("meeting-agent");
    write_file(target / ".installed", "x");
    write_file(target / "manifest.json", R"({"component_api": true, "version": "1"})");
    CHECK_FALSE(store.read_manifest("meeting-agent"));
    const auto info = ComponentCoordinator(store).describe("meeting-agent");
    CHECK(info.state == ComponentState::Broken);
    CHECK_THAT(info.reason, ContainsSubstring("manifest"));
}

TEST_CASE("test_prune_orphans_restores_complete_old_tree", "[test_components]") {
    Fixture f;
    const auto dest = f.root / "meeting-agent";
    const auto old = f.root / "meeting-agent.old";
    write_file(old / ".installed", "node22-pi1");
    write_file(old / "bundle.cjs", "// ok");
    REQUIRE_FALSE(fs::exists(dest));
    f.store().prune_orphans();
    CHECK(fs::is_directory(dest));
    CHECK(read_file(dest / ".installed") == "node22-pi1");
    CHECK_FALSE(fs::exists(old));
}

TEST_CASE("test_install_component_restores_previous_on_second_rename_failure",
          "[test_components]") {
    Fixture f("linux_x86_64");
    const auto dest = f.root / "meeting-agent";
    write_file(dest / ".installed", "old");
    write_file(dest / "keep.txt", "previous");

    int calls = 0;
    f.options.file_ops.replace = [&calls](const fs::path& from, const fs::path& to) {
        // First: destination -> rollback; second (staging -> destination) fails.
        if (++calls == 2) {
            throw os_error(std::errc::io_error, from, to);
        }
        fs::rename(from, to);
    };
    f.client.set_handler(
        [](const fv::http::Request& request) { return FakeHttpClient::serve(request, kEmptyZip); });
    const Json entry{{"version", "new"},
                     {"component_api", kComponentApi},
                     {"platform", "linux_x86_64"},
                     {"install_bytes", 1},
                     {"archives", Json::array({Json{{"name", "bundle.zip"},
                                                    {"url", "https://example.invalid/bundle.zip"},
                                                    {"sha256", fv::sha256_hex(kEmptyZip)},
                                                    {"size_bytes", kEmptyZip.size()},
                                                    {"extract", "zip"}}})}};

    auto store = f.store();
    CHECK_THROWS_AS(store.install("meeting-agent", entry, {}, {}), ComponentError);
    CHECK(fs::is_directory(dest));
    CHECK(read_file(dest / "keep.txt") == "previous");
}

TEST_CASE("test_uninstall_component_raises_when_leftovers_remain", "[test_components]") {
    Fixture f;
    write_file(f.root / "meeting-agent" / ".installed", "x");
    f.options.file_ops.replace = [](const fs::path& from, const fs::path& to) {
        throw os_error(std::errc::device_or_resource_busy, from, to);
    };
    f.options.file_ops.remove_tree = [](const fs::path&) {
    };
    auto store = f.store();
    CHECK_THROWS_WITH(store.uninstall("meeting-agent"), ContainsSubstring("fully remove"));
}

TEST_CASE("test_install_component_rejects_foreign_platform_before_download", "[test_components]") {
    Fixture f("linux_x86_64");
    const Json entry{{"version", "node22-pi1"},
                     {"component_api", kComponentApi},
                     {"platform", "win_amd64"},
                     {"install_bytes", 10},
                     {"archives", Json::array({Json{{"name", "node.zip"},
                                                    {"url", "https://example.invalid/node.zip"},
                                                    {"sha256", std::string(64, 'a')},
                                                    {"size_bytes", 5},
                                                    {"extract", "zip"}}})}};
    auto store = f.store();
    CHECK_THROWS_WITH(store.install("meeting-agent", entry, {}, {}),
                      ContainsSubstring("win_amd64"));
    CHECK(f.client.requests().empty());
}

TEST_CASE("test_a_superseded_install_is_offered_the_update", "[test_components]") {
    // A shipped-version difference must reach the user as a choice.
    Fixture f;
    make_installed(f.root, "asr-nvidia-cuda", Json{{"version", "0.0.9-py312"}});
    auto store = f.store();
    const auto info = ComponentCoordinator(store).describe("asr-nvidia-cuda");
    REQUIRE(info.installed_version);
    CHECK(*info.installed_version == "0.0.9-py312");
    REQUIRE(info.available_version);
    CHECK(*info.available_version == "0.1.0-py312");
    CHECK(info.state == ComponentState::UpdateAvailable);
}

TEST_CASE("test_update_available_explains_the_disk_it_frees", "[test_components]") {
    // The user chooses whether to update, so the row must state the payoff.
    Fixture f;
    make_installed(f.root, "gpu-accel", Json{{"version", "old"}});
    const Json entry{{"version", "new"}, {"install_bytes", 0}, {"archives", Json::array()}};
    const Json catalog{{"schema", 2},
                       {"components", {{"gpu-accel", {{"platforms", {{"win_amd64", entry}}}}}}}};
    auto store = f.store();
    ComponentCoordinator::Options options;
    options.catalog = &catalog;
    const auto info = ComponentCoordinator(store, options).describe("gpu-accel");
    CHECK(info.state == ComponentState::UpdateAvailable);
    CHECK_THAT(info.reason, ContainsSubstring("frees"));
}

TEST_CASE("test_describe_flags_incompatible_installs", "[test_components]") {
    Fixture f;
    make_installed(f.root, "gpu-accel",
                   Json{{"version", "1.0"}, {"component_api", kComponentApi + 1}});
    auto store = f.store();
    const Json null_catalog;
    const auto info = ComponentCoordinator(store, no_catalog(null_catalog)).describe("gpu-accel");
    CHECK(info.state == ComponentState::Incompatible);
    CHECK_FALSE(info.is_usable());
    CHECK_FALSE(info.reason.empty());
}

TEST_CASE("test_describe_survives_a_missing_catalog_entry", "[test_components]") {
    // What is on disk decides installed-ness, not what the catalog knows.
    Fixture f;
    make_installed(f.root, "gpu-accel", Json{{"version", "1.0"}});
    auto store = f.store();
    const Json null_catalog;
    const auto info = ComponentCoordinator(store, no_catalog(null_catalog)).describe("gpu-accel");
    CHECK(info.state == ComponentState::Installed);
    REQUIRE(info.installed_version);
    CHECK(*info.installed_version == "1.0");
}

// Disk space

TEST_CASE("test_free_space_check_raises_when_short", "[test_components]") {
    Fixture f;
    f.options.free_bytes = [](const fs::path&) {
        return std::uint64_t{1024};
    };
    auto store = f.store();
    CHECK_THROWS_WITH(store.check_free_space(std::int64_t{10} * 1024 * 1024 * 1024),
                      ContainsSubstring("disk space"));
}

TEST_CASE("test_free_space_check_passes_when_ample", "[test_components]") {
    Fixture f;
    f.options.free_bytes = [](const fs::path&) {
        return std::uint64_t{100} * 1024 * 1024 * 1024;
    };
    auto store = f.store();
    CHECK_NOTHROW(store.check_free_space(1024));
}

TEST_CASE("test_prune_orphans_preserves_old_when_restore_fails", "[test_components]") {
    Fixture f;
    const auto dest = f.root / "meeting-agent";
    const auto old = f.root / "meeting-agent.old";
    write_file(old / ".installed", "node22-pi1");
    write_file(old / "bundle.cjs", "// ok");
    f.options.file_ops.replace = [](const fs::path& from, const fs::path& to) {
        throw os_error(std::errc::permission_denied, from, to);
    };
    f.store().prune_orphans();
    CHECK(fs::is_directory(old));
    CHECK(fs::exists(old / ".installed"));
    CHECK_FALSE(fs::exists(dest));
}

TEST_CASE("test_uninstall_removes_stale_old_without_target", "[test_components]") {
    Fixture f;
    const auto old = f.root / "meeting-agent.old";
    write_file(old / "bundle.cjs", "// leftover");
    f.store().uninstall("meeting-agent");
    CHECK_FALSE(fs::exists(old));
}

TEST_CASE("test_catalog_entry_mutation_does_not_affect_builtin", "[test_components]") {
    auto entry = catalog_entry(component_id::kAsrNvidiaCpu, platform::kWinAmd64);
    REQUIRE(entry);
    (*entry)["install_bytes"] = 1;
    (*entry)["archives"][0]["sha256"] = std::string(64, '0');
    const auto again = catalog_entry(component_id::kAsrNvidiaCpu, platform::kWinAmd64);
    CHECK(again->at("install_bytes") != 1);
    CHECK(again->at("archives")[0].at("sha256") != std::string(64, '0'));
}

TEST_CASE("test_linux_runtime_needs_the_shared_library", "[test_components]") {
    Fixture f("linux_x86_64");
    auto store = f.store();
    CHECK_THROWS_WITH(store.validate_payload("asr-nvidia-cuda", f.temp.path()),
                      ContainsSubstring("missing required files"));
}

TEST_CASE("test_installed_size_does_not_count_dylib_aliases_twice", "[test_components]") {
    Fixture f;
    const auto directory = f.root / "asr-nvidia-cpu";
    write_file(directory / "libreal.dylib", std::string(1000, 'x'));
    std::error_code error;
    fs::create_symlink("libreal.dylib", directory / "libalias.dylib", error);
    if (error) {
        SKIP("Creating symlinks requires Windows Developer Mode or elevation");
    }
#ifdef _WIN32
    constexpr std::int64_t kLinkSize = 0; // a file link's own size, as lstat reports it
#else
    constexpr std::int64_t kLinkSize = 13; // the link holds "libreal.dylib"
#endif
    CHECK(f.store().installed_size_bytes("asr-nvidia-cpu") == 1000 + kLinkSize);
}

TEST_CASE("test_installed_size_is_remembered_until_the_install_changes", "[test_components]") {
    Fixture f;
    const auto target = make_installed(f.root, "gpu-accel", Json{{"version", "1"}});
    std::vector<fs::path> walks;
    bool counting = false;
    f.options.walk_size = [&](const fs::path& directory) {
        if (counting) {
            walks.push_back(directory);
        }
        std::int64_t total = 0;
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                total += static_cast<std::int64_t>(entry.file_size());
            }
        }
        return total;
    };
    auto store = f.store();
    const auto first = store.installed_size_bytes("gpu-accel");
    counting = true;
    CHECK(store.installed_size_bytes("gpu-accel") == first);
    CHECK(walks.empty());

    // An install swaps a freshly staged tree, sentinel included, into place.
    const auto staged = f.root / "staged";
    write_file(staged / "bin" / "fake.dll", std::string(4096, 'x'));
    write_file(staged / ".installed", "2");
    fs::remove_all(target);
    fs::rename(staged, target);
    CHECK(store.installed_size_bytes("gpu-accel") == 4096 + 1);
    CHECK(walks.size() == 1);
}

TEST_CASE("test_fetch_catalog_is_read_only_and_entries_are_mutable_copies", "[test_components]") {
    Fixture f;
    auto store = f.store();
    ComponentCoordinator coordinator(store);
    static_assert(std::is_const_v<std::remove_reference_t<decltype(coordinator.fetch_catalog())>>);
    auto entry = coordinator.catalog_entry(component_id::kAsrNvidiaCpu);
    REQUIRE(entry);
    (*entry)["install_bytes"] = -1;
    CHECK(coordinator.catalog_entry(component_id::kAsrNvidiaCpu)->at("install_bytes") != -1);
}

// tests/test_component_download.py

namespace {
const std::string kBody = "component-payload";
const std::string kUrl = "https://files.pythonhosted.org/packages/example.whl";
} // namespace

TEST_CASE("test_progress_is_reported_across_all_archives", "[test_component_download]") {
    Fixture f;
    f.client.set_handler(
        [](const fv::http::Request& request) { return FakeHttpClient::serve(request, kBody); });
    auto store = f.store();
    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> seen;
    store.download_verified(
        kUrl, fv::sha256_hex(kBody), static_cast<std::int64_t>(kBody.size()),
        f.temp.path() / "example.whl",
        [&](std::string_view phase, std::int64_t done, std::int64_t total) {
            seen.emplace_back(std::string(phase), done, total);
        },
        {}, 100, 500);
    REQUIRE_FALSE(seen.empty());
    const auto size = static_cast<std::int64_t>(kBody.size());
    CHECK(seen.front() ==
          std::tuple<std::string, std::int64_t, std::int64_t>{"downloading", 100 + size, 500});
    CHECK(seen.back() ==
          std::tuple<std::string, std::int64_t, std::int64_t>{"verifying", 100 + size, 500});
}

TEST_CASE("test_cancel_keeps_the_partial_for_resume", "[test_component_download]") {
    Fixture f;
    auto store = f.store();
    const auto destination = f.temp.path() / "example.whl";
    fv::CancelSource cancel;

    fv::http::ScriptedResponse cut = FakeHttpClient::serve(fv::http::Request{}, kBody);
    cut.chunks = {8};
    cut.on_chunk = [&cancel] {
        cancel.cancel();
    };
    f.client.push(cut);
    CHECK_THROWS_AS(store.download_verified(kUrl, fv::sha256_hex(kBody),
                                            static_cast<std::int64_t>(kBody.size()), destination,
                                            {}, cancel.token()),
                    ComponentCanceled);
    CHECK(read_file(fs::path(destination).concat(".part")) == kBody.substr(0, 8));

    f.client.set_handler(
        [](const fv::http::Request& request) { return FakeHttpClient::serve(request, kBody); });
    store.download_verified(kUrl, fv::sha256_hex(kBody), static_cast<std::int64_t>(kBody.size()),
                            destination, {}, {});
    CHECK(read_file(destination) == kBody);
    const auto requests = f.client.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[1].range_from);
    CHECK(*requests[1].range_from == 8);
}
