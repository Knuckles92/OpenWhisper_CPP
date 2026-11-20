#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "update/check.h"
#include "update/release_identity.h"

// Where the running copy is installed, as the Inno uninstall registration says
// (services/app_update_apply.py: discover_install_registration, running_app_dir). Only the
// registration is read here; whether a native update may replace the install is M10's.
namespace fv::update {

enum class RegistryHive { CurrentUser, LocalMachine };

// One Inno uninstall entry ("...\Uninstall\<AppId>_is1"), the values as str() of what the
// registry holds (a DWORD as its decimal digits).
struct InstallRegistration {
    RegistryHive hive = RegistryHive::CurrentUser;
    std::string key_path;
    std::filesystem::path install_location; // os.path.normpath of InstallLocation
    std::string uninstall_string;
    std::string display_version;
    std::optional<std::int64_t> estimated_size_kb; // int() of EstimatedSize; none when unreadable
    std::string install_date;
    int view = 0; // 0 (the default view), KEY_WOW64_64KEY or KEY_WOW64_32KEY
};

// The entry under `key_path`: HKCU before HKLM, each in the default, 64-bit and 32-bit registry
// views, the first that has both an InstallLocation and an UninstallString. nullopt when there is
// none, and always off Windows.
std::optional<InstallRegistration> discover_install_registration(std::string_view key_path);
// ...of this release identity's AppId.
std::optional<InstallRegistration> discover_install_registration(const ReleaseIdentity&);

// The folder of the running executable.
std::filesystem::path running_app_dir();
std::filesystem::path running_app_executable();

// The probe a registration and a running folder make: registered when there is a registration,
// all_users when it is under HKLM (such an install updates through the setup with /ALLUSERS).
InstallProbe make_install_probe(const std::optional<InstallRegistration>&,
                                std::filesystem::path running_dir);

// The probe of this process: the registration of the identity's AppId and running_app_dir().
// Reads the registry: off the GUI thread, or once at startup.
InstallProbe probe_install(const ReleaseIdentity&);

} // namespace fv::update
