// Where the running copy is installed: the Inno registration read from the registry, under a
// test key that stands in for HKCU and HKLM (RegOverridePredefKey), never the real ones.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include "download_support.h"
#include "update/apply.h"
#include "update/check.h"
#include "update/install_probe.h"
#include "update/release_identity.h"

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

TEST_CASE("a registration makes a probe", "[update]") {
    update::InstallRegistration registration;
    registration.hive = update::RegistryHive::LocalMachine;
    registration.install_location = "C:/Program Files/OpenWhisper_CPP";
    const auto probe = update::make_install_probe(registration, "C:/Program Files/OpenWhisper_CPP");
    CHECK(probe.registered);
    CHECK(probe.all_users);
    CHECK(probe.install_dir == "C:/Program Files/OpenWhisper_CPP");
    CHECK(update::detect_channel(probe) == update::InstallChannel::Installer);

    const auto none = update::make_install_probe(std::nullopt, "D:/build/bin");
    CHECK_FALSE(none.registered);
    CHECK_FALSE(none.all_users);
    CHECK(none.running_dir == "D:/build/bin");
    CHECK(update::detect_channel(none) == update::InstallChannel::Source);
}

TEST_CASE("the running folder is the executable's", "[update]") {
    const auto dir = update::running_app_dir();
    CHECK(dir.is_absolute());
    CHECK(std::filesystem::is_directory(dir));
}

#ifdef _WIN32
namespace {

std::wstring wide(const std::string& text) {
    return std::filesystem::path(text).wstring();
}

// HKCU and HKLM redirected to two empty keys under HKCU\Software\OpenWhisper_CPPTest\<pid>, for
// this process only, and the whole tree removed afterwards.
class RegistrySandbox {
public:
    RegistrySandbox() {
        base_ = L"Software\\OpenWhisper_CPPTest\\" + std::to_wstring(GetCurrentProcessId());
        REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, (base_ + L"\\hkcu").c_str(), 0, nullptr, 0,
                                KEY_ALL_ACCESS, nullptr, &user_, nullptr) == ERROR_SUCCESS);
        REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, (base_ + L"\\hklm").c_str(), 0, nullptr, 0,
                                KEY_ALL_ACCESS, nullptr, &machine_, nullptr) == ERROR_SUCCESS);
        REQUIRE(RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine_) == ERROR_SUCCESS);
        REQUIRE(RegOverridePredefKey(HKEY_CURRENT_USER, user_) == ERROR_SUCCESS);
    }
    ~RegistrySandbox() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        RegCloseKey(user_);
        RegCloseKey(machine_);
        RegDeleteTreeW(HKEY_CURRENT_USER, base_.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\OpenWhisper_CPPTest");
    }
    RegistrySandbox(const RegistrySandbox&) = delete;
    RegistrySandbox& operator=(const RegistrySandbox&) = delete;

    // Writes the uninstall entry of `key_path` under the (redirected) hive.
    void write(HKEY hive, const std::string& key_path, const std::wstring& location,
               const std::wstring& uninstall, const std::wstring& version, DWORD size_kb) {
        HKEY key = nullptr;
        REQUIRE(RegCreateKeyExW(hive, wide(key_path).c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                                nullptr, &key, nullptr) == ERROR_SUCCESS);
        const auto set_text = [&](const wchar_t* name, const std::wstring& value) {
            RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        };
        if (!location.empty()) {
            set_text(L"InstallLocation", location);
        }
        if (!uninstall.empty()) {
            set_text(L"UninstallString", uninstall);
        }
        set_text(L"DisplayVersion", version);
        set_text(L"InstallDate", L"20261115");
        RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&size_kb),
                       sizeof(size_kb));
        RegCloseKey(key);
    }

private:
    std::wstring base_;
    HKEY user_ = nullptr;
    HKEY machine_ = nullptr;
};

} // namespace

TEST_CASE("an HKCU registration is found with its values", "[update][platform]") {
    RegistrySandbox registry;
    const auto& identity = update::release_identity();
    CHECK_FALSE(update::discover_install_registration(identity).has_value());
    registry.write(HKEY_CURRENT_USER, identity.uninstall_key_path(),
                   L"C:\\Users\\u\\AppData\\Local\\Programs\\OpenWhisper_CPP\\",
                   L"\"C:\\Users\\u\\AppData\\Local\\Programs\\OpenWhisper_CPP\\unins000.exe\"",
                   L"0.1.0", 51200);
    const auto found = update::discover_install_registration(identity);
    REQUIRE(found.has_value());
    CHECK(found->hive == update::RegistryHive::CurrentUser);
    CHECK(found->key_path == identity.uninstall_key_path());
    CHECK(found->install_location ==
          std::filesystem::path(L"C:\\Users\\u\\AppData\\Local\\Programs\\OpenWhisper_CPP"));
    CHECK(found->display_version == "0.1.0");
    CHECK(found->estimated_size_kb == 51200);
    CHECK(found->install_date == "20261115");
    CHECK(found->view == 0);

    const auto probe = update::make_install_probe(
        found, std::filesystem::path(L"C:\\Users\\u\\AppData\\Local\\Programs\\OpenWhisper_CPP"));
    CHECK(probe.registered);
    CHECK_FALSE(probe.all_users);
    CHECK(update::detect_channel(probe) == update::InstallChannel::Installer);
}

TEST_CASE("an entry without a location or an uninstaller is not a registration",
          "[update][platform]") {
    RegistrySandbox registry;
    const auto& identity = update::release_identity();
    registry.write(HKEY_CURRENT_USER, identity.uninstall_key_path(), L"", L"unins000.exe", L"0.1.0",
                   1);
    CHECK_FALSE(update::discover_install_registration(identity).has_value());
}

// from tests/test_app_update.py::TestDualAssetsAndApplyMode::test_hklm_registration_uses_setup
TEST_CASE("hklm registration uses setup, read from the registry", "[test_app_update][platform]") {
    RegistrySandbox registry;
    const auto& identity = update::release_identity();
    registry.write(HKEY_LOCAL_MACHINE, identity.uninstall_key_path(),
                   L"C:\\Program Files\\OpenWhisper_CPP",
                   L"C:\\Program Files\\OpenWhisper_CPP\\unins000.exe", L"0.1.0", 1);
    const auto probe =
        update::make_install_probe(update::discover_install_registration(identity),
                                   std::filesystem::path(L"C:\\Program Files\\OpenWhisper_CPP"));
    REQUIRE(probe.registered);
    CHECK(probe.all_users);
    const auto channel = update::detect_channel(probe);
    CHECK(channel == update::InstallChannel::Installer);
    // A machine-wide install updates only through its setup, run for all users.
    CHECK(update::resolve_release_apply_mode(channel, release_with(setup_asset()), "win32") ==
          update::ApplyMode::Setup);
    const auto handoff = update::setup_command("C:/u/updates/OpenWhisper_CPP-Setup-2.8.6.exe",
                                               probe, "C:/u/updates");
    CHECK(std::ranges::find(handoff.args, "/ALLUSERS") != handoff.args.end());
    CHECK(std::ranges::find(handoff.args, "/CURRENTUSER") == handoff.args.end());
}
#endif
