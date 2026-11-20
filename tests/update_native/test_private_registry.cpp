#ifdef _WIN32
#include <catch2/catch_test_macros.hpp>
#include <windows.h>

#include "base/atomic_file.h"
#include "base/utf8.h"
#include "testkit/testkit.h"
#include "update/update_contract.h"
#include "update_native/manifest.h"
#include "update_native/win_registry_actions.h"

namespace {
struct Key {
    HKEY value = nullptr;
    ~Key() {
        if (value)
            RegCloseKey(value);
    }
};
// This test runs in its own CTest process. The HKCU override maps that process to a temporary
// application hive accessed only by its handle. Nothing is registered with Windows startup.
struct CurrentUserOverride {
    bool installed = false;
    ~CurrentUserOverride() {
        if (installed)
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    }
};
void set_value(HKEY key, const wchar_t* name, const std::wstring& value) {
    REQUIRE(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
            ERROR_SUCCESS);
}
std::wstring get_value(HKEY key, const wchar_t* name) {
    wchar_t value[2048]{};
    DWORD size = sizeof(value);
    REQUIRE(RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value, &size) ==
            ERROR_SUCCESS);
    return value;
}
} // namespace

TEST_CASE("native registry actions apply and roll back inside a private application hive",
          "[update_native]") {
    namespace u = fv::update_native;
    fv::testkit::TempDataRoot root;
    Key hive;
    REQUIRE(RegLoadAppKeyW((root.path() / "fixture.hiv").c_str(), &hive.value, KEY_ALL_ACCESS,
                           REG_PROCESS_APPKEY, 0) == ERROR_SUCCESS);
    CurrentUserOverride redirect;
    REQUIRE(RegOverridePredefKey(HKEY_CURRENT_USER, hive.value) == ERROR_SUCCESS);
    redirect.installed = true;

    const auto& release = fv::update::release_identity();
    const auto install = root.path() / "Programs" / "FioriVoce";
    std::filesystem::create_directories(install);
    fv::write_file_atomic(install / "fiorivoce.exe", std::string(2048, 'x'));
    fv::update::InstallRegistration registration;
    registration.key_path = release.uninstall_key_path();
    registration.install_location = install;
    registration.uninstall_string = fv::path_to_utf8(install / "unins000.exe");
    registration.display_version = "0.1.0";
    constexpr auto runonce = R"(Software\Microsoft\Windows\CurrentVersion\RunOnce)";
    const u::JournalContext context{root.path() / "data", install, release.app_id, {},
                                    registration,         runonce};
    const std::string id = "0123456789abcdef0123456789abcdef";
    auto journal = u::make_journal(context, id);
    journal.old_version = journal.old_display_version = "0.1.0";
    journal.new_version = "0.1.1";
    journal.old_estimated_size_kb = 74;
    journal.old_install_date = "20261101";
    Key registered;
    REQUIRE(RegCreateKeyExW(hive.value, fv::path_from_utf8(registration.key_path).c_str(), 0,
                            nullptr, 0, KEY_ALL_ACCESS, nullptr, &registered.value,
                            nullptr) == ERROR_SUCCESS);
    set_value(registered.value, L"InstallLocation", install.native());
    set_value(registered.value, L"UninstallString",
              fv::path_from_utf8(registration.uninstall_string).native());
    set_value(registered.value, L"DisplayVersion", L"0.1.0");
    u::WinRegistryActions actions(context, id);
    actions.verify(journal);
    actions.install_recovery(journal);
    actions.install_recovery(journal);
    const auto recovery_name = fv::path_from_utf8(fv::update::runonce_value_name()).native();
    const auto paths = u::journal_paths(context, id);
    Key recovery;
    REQUIRE(RegOpenKeyExW(hive.value, fv::path_from_utf8(runonce).c_str(), 0, KEY_ALL_ACCESS,
                          &recovery.value) == ERROR_SUCCESS);
    const auto recovery_command = get_value(recovery.value, recovery_name.c_str());
    CHECK(recovery_command == L"\"" + (paths.transaction_dir / "fiorivoce-updater.exe").native() +
                                  L"\" --recover --transaction-id " +
                                  fv::path_from_utf8(id).native());
    actions.update(journal);
    CHECK(get_value(registered.value, L"DisplayVersion") == L"0.1.1");
    CHECK(get_value(registered.value, L"InstallDate").size() == 8);
    DWORD size_kb = 0;
    DWORD bytes = sizeof(size_kb);
    REQUIRE(RegGetValueW(registered.value, nullptr, L"EstimatedSize", RRF_RT_REG_DWORD, nullptr,
                         &size_kb, &bytes) == ERROR_SUCCESS);
    CHECK(size_kb == 2);
    actions.restore(journal);
    CHECK(get_value(registered.value, L"DisplayVersion") == L"0.1.0");
    CHECK(get_value(registered.value, L"InstallDate") == L"20261101");
    actions.clear_recovery(journal);
    CHECK(RegQueryValueExW(recovery.value, recovery_name.c_str(), nullptr, nullptr, nullptr,
                           nullptr) == ERROR_FILE_NOT_FOUND);
    set_value(recovery.value, recovery_name.c_str(), L"owned by another transaction");
    CHECK_THROWS_AS(actions.install_recovery(journal), u::UpdateError);
    CHECK_THROWS_AS(actions.clear_recovery(journal), u::UpdateError);
    CHECK(get_value(recovery.value, recovery_name.c_str()) == L"owned by another transaction");
    set_value(registered.value, L"UninstallString", L"other");
    CHECK_THROWS_AS(actions.verify(journal), u::UpdateError);
}
#endif
