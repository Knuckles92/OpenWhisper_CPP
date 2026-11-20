// Install channel, version rules, apply mode and the release identity: OpenWhisper's
// tests/test_app_update.py (TestDetectChannel, TestCompareVersions, TestCanApplyAndHints, the
// apply-mode half of TestDualAssetsAndApplyMode) and test_app_update_apply.py's TestStrictVersion.

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <string>

#include "base/identity.h"
#include "pyport/errors.h"
#include "update/check.h"
#include "update/data/release_identity.json.h"
#include "update/release_identity.h"
#include "update/update_contract.h"

namespace update = fv::update;
using update::ApplyMode;
using update::InstallChannel;
using update::ReleaseAsset;
using update::ReleaseInfo;
using update::UpdateStatus;

namespace {

ReleaseAsset asset(const std::string& version, bool setup,
                   std::optional<std::string> sha256 = std::string(64, 'a')) {
    const auto name =
        setup ? update::setup_asset_name(version) : update::archive_asset_name(version);
    return ReleaseAsset{"https://github.com/example/app/releases/download/v" + version + "/" + name,
                        name, 100, std::move(sha256), std::nullopt};
}

// tests/test_app_update.py's _release: a setup asset with a digest, optionally a native one.
std::optional<ReleaseInfo> release(const std::string& version = "2.2.0", bool native = false) {
    ReleaseInfo info;
    info.version = version;
    info.tag_name = "v" + version;
    info.html_url = "https://github.com/example/app/releases/tag/v" + version;
    info.notes = "notes";
    info.setup_asset = asset(version, true);
    if (native) {
        info.native_asset = asset(version, false);
    }
    return info;
}

update::InstallProbe installed(bool all_users = false) {
    update::InstallProbe probe;
    probe.registered = true;
    probe.all_users = all_users;
    probe.install_dir = "C:/Users/me/AppData/Local/Programs/FioriVoce";
    probe.running_dir = "C:/Users/me/AppData/Local/Programs/FioriVoce/";
    return probe;
}

} // namespace

// from tests/test_app_update.py::TestDetectChannel::test_frozen_is_installer
TEST_CASE("a registered install is the installer channel", "[test_app_update]") {
    CHECK(update::detect_channel(installed()) == InstallChannel::Installer);
    auto probe = installed();
    probe.running_dir = "c:\\users\\ME\\appdata\\local\\programs\\fiorivoce\\.";
#ifdef _WIN32
    CHECK(update::detect_channel(probe) == InstallChannel::Installer);
#endif
}

// from tests/test_app_update.py::TestDetectChannel::test_loose_tree_is_source
TEST_CASE("loose tree is source", "[test_app_update]") {
    CHECK(update::detect_channel({}) == InstallChannel::Source);
    update::InstallProbe probe;
    probe.running_dir = "D:/coding/fiorivoce/out/build/bin";
    CHECK(update::detect_channel(probe) == InstallChannel::Source);
}

TEST_CASE("a build tree beside a registered install is the source channel", "[update]") {
    auto probe = installed();
    probe.running_dir = "D:/coding/fiorivoce/out/build/bin";
    CHECK(update::detect_channel(probe) == InstallChannel::Source);
    probe.install_dir.clear();
    probe.running_dir.clear();
    CHECK(update::detect_channel(probe) == InstallChannel::Source);
}

// from tests/test_app_update.py::TestDetectChannel::test_frozen_channel_label_is_platform_aware
TEST_CASE("frozen channel label is platform aware", "[test_app_update]") {
    CHECK(update::channel_label(InstallChannel::Installer, "win32") == "Windows installer");
    CHECK(update::channel_label(InstallChannel::Installer, "linux") == "Linux package");
    CHECK(update::channel_label(InstallChannel::Installer, "linux2") == "Linux package");
    CHECK(update::channel_label(InstallChannel::Installer, "darwin") == "macOS application");
    CHECK(update::channel_label(InstallChannel::Source, "win32") == "Source copy");
}

// from
// tests/test_app_update.py::TestCompareVersions::test_padded_patch_release_preserves_update_asset_names
TEST_CASE("padded patch release preserves update asset names", "[test_app_update]") {
    CHECK(update::compare_versions("2.6.0", "v2.6.01") == UpdateStatus::UpdateAvailable);
    CHECK(update::compare_versions("2.6.01", "v2.6.1") == UpdateStatus::UpToDate);
    CHECK(update::is_newer_version("2.6.09", "2.6.10"));
    CHECK(update::parse_strict_version("v2.6.01") == std::array<long long, 3>{2, 6, 1});
    const std::string product(fv::identity().product);
    CHECK(update::setup_asset_name("v2.6.01") == product + "-Setup-2.6.01.exe");
    CHECK(update::archive_asset_name("v2.6.01") == product + "-2.6.01-win64.zip");
}

// from tests/test_app_update.py::TestCompareVersions::test_equal_behind_and_ahead
TEST_CASE("equal behind and ahead", "[test_app_update]") {
    CHECK(update::compare_versions("2.1.1", "v2.1.1") == UpdateStatus::UpToDate);
    CHECK(update::compare_versions("2.1.1", "v2.2.0") == UpdateStatus::UpdateAvailable);
    CHECK(update::compare_versions("2.2.0", "v2.1.1") == UpdateStatus::Development);
}

// from
// tests/test_app_update.py::TestCompareVersions::test_prerelease_suffix_is_ignored_for_ordering
TEST_CASE("prerelease suffix is ignored for ordering", "[test_app_update]") {
    CHECK(update::parse_version("2.2.0-rc.1") == std::array<long long, 3>{2, 2, 0});
    CHECK(update::compare_versions("2.2.0-rc.1", "v2.2.0") == UpdateStatus::UpToDate);
}

// from tests/test_app_update.py::TestCompareVersions::test_normalize_strips_v
TEST_CASE("normalize strips v", "[test_app_update]") {
    CHECK(update::normalize_version("v2.1.1") == "2.1.1");
    CHECK(update::normalize_version("  V2.1.1  ") == "2.1.1");
}

// from tests/test_app_update_apply.py::TestStrictVersion::test_rejects_prerelease
TEST_CASE("strict version rejects prerelease", "[test_app_update_apply]") {
    CHECK_THROWS_AS(update::parse_strict_version("2.5.2-rc.1"), fv::py::ValueError);
}

// from tests/test_app_update_apply.py::TestStrictVersion::test_accepts_plain
TEST_CASE("strict version accepts plain", "[test_app_update_apply]") {
    CHECK(update::parse_strict_version("v2.5.3") == std::array<long long, 3>{2, 5, 3});
}

// from tests/test_app_update.py::TestCanApplyAndHints::test_installer_windows_with_digest
TEST_CASE("installer windows with digest", "[test_app_update]") {
    CHECK(update::can_apply(InstallChannel::Installer, release(), "win32"));
}

// from tests/test_app_update.py::TestCanApplyAndHints::test_git_never_applies (a development
// build has no Python git checkout's counterpart, and never applies either)
TEST_CASE("a development build never applies", "[test_app_update]") {
    CHECK_FALSE(update::can_apply(InstallChannel::Source, release(), "win32"));
    CHECK(update::resolve_release_apply_mode(InstallChannel::Source, release(), "win32") ==
          ApplyMode::NotifyOnly);
}

// from tests/test_app_update.py::TestCanApplyAndHints::test_macos_installer_does_not_apply
TEST_CASE("macos installer does not apply", "[test_app_update]") {
    CHECK_FALSE(update::can_apply(InstallChannel::Installer, release(), "darwin"));
    CHECK_FALSE(update::can_apply(InstallChannel::Installer, release(), "linux"));
}

// from tests/test_app_update.py::TestDualAssetsAndApplyMode::test_hklm_registration_uses_setup
TEST_CASE("hklm registration uses setup", "[test_app_update]") {
    const auto channel = update::detect_channel(installed(true));
    CHECK(channel == InstallChannel::Installer);
    CHECK(update::resolve_release_apply_mode(channel, release("2.2.0", true), "win32") ==
          ApplyMode::Setup);
}

// from tests/test_app_update.py::TestDualAssetsAndApplyMode::test_setup_only_release_is_setup_mode
TEST_CASE("setup only release is setup mode", "[test_app_update]") {
    CHECK(update::resolve_release_apply_mode(InstallChannel::Installer, release(), "win32") ==
          ApplyMode::Setup);
}

TEST_CASE("a ready native archive does not select native mode before the native updater",
          "[update]") {
    CHECK(update::resolve_release_apply_mode(InstallChannel::Installer, release("2.2.0", true),
                                             "win32") == ApplyMode::Setup);
    auto native_only = release("2.2.0", true);
    native_only->setup_asset.reset();
    CHECK(update::resolve_release_apply_mode(InstallChannel::Installer, native_only, "win32") ==
          ApplyMode::NotifyOnly);
}

// from
// tests/test_app_update.py::TestDualAssetsAndApplyMode::test_missing_both_digests_is_notify_only
TEST_CASE("missing both digests is notify only", "[test_app_update]") {
    ReleaseInfo info;
    info.version = "2.2.0";
    info.tag_name = "v2.2.0";
    info.html_url = "https://example";
    info.setup_asset = asset("2.2.0", true, std::nullopt);
    info.native_asset = asset("2.2.0", false, std::nullopt);
    CHECK(update::resolve_release_apply_mode(InstallChannel::Installer, info, "win32") ==
          ApplyMode::NotifyOnly);
    CHECK_FALSE(update::can_apply(InstallChannel::Installer, info, "win32"));
}

TEST_CASE("an asset needs a URL, a digest and a positive size to be ready", "[update]") {
    auto ready = asset("2.2.0", true);
    CHECK(update::asset_ready(&ready));
    auto no_url = ready;
    no_url.url.clear();
    CHECK_FALSE(update::asset_ready(&no_url));
    auto empty_digest = ready;
    empty_digest.sha256 = "";
    CHECK_FALSE(update::asset_ready(&empty_digest));
    auto no_size = ready;
    no_size.size_bytes = 0;
    CHECK_FALSE(update::asset_ready(&no_size));
    CHECK_FALSE(update::asset_ready(nullptr));
}

TEST_CASE("release_identity.json parses and names its mutexes after the AppId", "[update]") {
    const auto& identity = update::release_identity();
    REQUIRE(identity.app_id.size() == 38);
    CHECK(identity.app_id.front() == '{');
    CHECK(identity.app_id.back() == '}');
    for (std::size_t i = 1; i + 1 < identity.app_id.size(); ++i) {
        const char c = identity.app_id[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            CHECK(c == '-');
        } else {
            CHECK(((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')));
        }
    }
    const auto bare = identity.app_id.substr(1, 36);
    const std::string product(fv::identity().product);
    CHECK(identity.app_id_bare() == bare);
    CHECK(identity.app_mutex() == product + "-App-" + bare);
    CHECK(identity.app_mutex_global() == "Global\\" + product + "-App-" + bare);
    CHECK(identity.setup_mutex() == "Global\\" + product + "-Setup-" + bare);
    CHECK(identity.app_mutexes() ==
          std::vector<std::string>{identity.app_mutex(), identity.app_mutex_global()});
    CHECK(identity.update_mutexes() ==
          std::vector<std::string>{product + "-Update-" + bare,
                                   "Global\\" + product + "-Update-" + bare});
    CHECK(identity.inno_uninstall_key() == identity.app_id + "_is1");
    CHECK(identity.publisher == product);
    CHECK(identity.install_dir_name == product);
    CHECK(identity.setup_prefix == product + "-Setup-");
    CHECK(identity.source.kind == update::UpdateSource::Kind::None);
    CHECK_FALSE(identity.app_exe.empty());
    CHECK_FALSE(identity.cli_exe.empty());
}

TEST_CASE("release_identity.json names nothing of OpenWhisper's release", "[update]") {
    std::string text(fv::embedded::update_release_identity);
    for (auto& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CHECK(text.find("openwhisper") == std::string::npos);
    CHECK(text.find("knuckles92") == std::string::npos);
    CHECK(text.find("fiorilabs") == std::string::npos);
    CHECK(text.find("ca36ad0a") == std::string::npos); // OpenWhisper's AppId
}

TEST_CASE("a malformed release identity is refused with the key it lacks", "[update]") {
    // A made-up AppId: the real one is written only in release_identity.json.
    const std::string good = R"({"app_id": "{0123ABCD-4567-89EF-0123-456789ABCDEF}",
        "publisher": "P", "publisher_url": "", "install_dir_name": "D", "app_exe": "a.exe",
        "cli_exe": "c.exe", "setup_prefix": "P-Setup-", "source": {"kind": "none"},
        "timestamp_url": ""})";
    CHECK_NOTHROW(update::parse_release_identity(good));
    const auto github = update::parse_release_identity(std::string(good).replace(
        good.find(R"({"kind": "none"})"), 16, R"({"kind": "github", "repo": "owner/name"})"));
    CHECK(github.source.kind == update::UpdateSource::Kind::GitHub);
    CHECK(github.source.repo == "owner/name");

    const auto refused = [&](std::string_view from, std::string_view to) {
        auto text = good;
        text.replace(text.find(from), from.size(), to);
        CHECK_THROWS_AS(update::parse_release_identity(text), std::invalid_argument);
    };
    refused("{0123ABCD-4567-89EF-0123-456789ABCDEF}", "{0123abcd-4567-89ef-0123-456789abcdef}");
    refused("{0123ABCD-4567-89EF-0123-456789ABCDEF}", "0123ABCD-4567-89EF-0123-456789ABCDEF");
    refused("{0123ABCD-4567-89EF-0123-456789ABCDEF}", "{0123ABCD-4567-89EF-0123-456789ABCDEG}");
    refused("{0123ABCD-4567-89EF-0123-456789ABCDEF}", "{0123ABCD+4567-89EF-0123-456789ABCDEF}");
    refused(R"("publisher": "P")", R"("publisher": "")");
    refused(R"({"kind": "none"})", R"({"kind": "gitlab"})");
    refused(R"({"kind": "none"})", R"({"kind": "github", "repo": "nameonly"})");
    refused(R"({"kind": "none"})", R"({"kind": "github", "repo": "a/b/c"})");
    refused(R"("cli_exe": "c.exe",)", "");
    CHECK_THROWS_AS(update::parse_release_identity("[]"), std::invalid_argument);
    CHECK_THROWS_AS(update::parse_release_identity("{"), std::invalid_argument);
}

// from tests/test_update_resilience.py::test_bridge_policy_is_enforced_at_the_minimum_version
TEST_CASE("bridge policy is enforced at the minimum version", "[test_update_resilience]") {
    // An install at the floor still has a helper that cannot take this native protocol: it gets
    // the setup first. One patch later it does not.
    CHECK(update::is_setup_bridge_release(update::kMinimumUpdaterVersion));
    const auto floor = update::parse_strict_version(update::kMinimumUpdaterVersion);
    const auto next = std::to_string(floor[0]) + "." + std::to_string(floor[1]) + "." +
                      std::to_string(floor[2] + 1);
    CHECK_FALSE(update::is_setup_bridge_release(next));
    if (floor[2] > 0) {
        CHECK(update::is_setup_bridge_release(std::to_string(floor[0]) + "." +
                                              std::to_string(floor[1]) + "." +
                                              std::to_string(floor[2] - 1)));
    }
    CHECK_THROWS_AS(update::is_setup_bridge_release(next + "-rc.1"), fv::py::ValueError);
}

TEST_CASE("transaction ids and native results keep the contract's shape", "[update]") {
    const std::string id(32, 'a');
    CHECK(update::validate_transaction_id("  " + std::string(32, 'A') + " ") == id);
    CHECK_THROWS_AS(update::validate_transaction_id("../" + id), fv::py::ValueError);
    CHECK(update::encode_native_result(id) == "native:" + id);
    CHECK(update::decode_native_result("native: " + id) == id);
    CHECK_FALSE(update::decode_native_result("native:xyz"));
    CHECK_FALSE(update::decode_native_result(id));
    CHECK(update::is_setup_bridge_release("0.1.0"));
    CHECK_FALSE(update::is_setup_bridge_release("0.1.1"));
    std::string product(fv::identity().product);
    for (auto& c : product) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    CHECK(update::manifest_name() == "." + product + "-update.json");
    CHECK(update::sentinel_name() == "." + product + "-update-complete");
}
