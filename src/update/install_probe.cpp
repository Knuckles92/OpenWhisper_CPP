#include "update/install_probe.h"

#include <utility>

#include "base/utf8.h"
#include "pyport/number.h"
#include "update/detail/platform.h"

namespace fv::update {

namespace {

// os.path.normpath for a folder: separators made native, "." and ".." steps resolved, no
// trailing separator.
std::filesystem::path normpath(std::string_view text) {
    auto path = path_from_utf8(text).lexically_normal();
    path.make_preferred();
    if (!path.has_filename() && path.has_relative_path()) {
        path = path.parent_path();
    }
    return path;
}

} // namespace

std::optional<InstallRegistration> discover_install_registration(std::string_view key_path) {
    for (const auto hive : {RegistryHive::CurrentUser, RegistryHive::LocalMachine}) {
        for (const int view : detail::registry_views()) {
            const auto values = detail::read_uninstall_key(hive, key_path, view);
            if (!values || values->install_location.empty() || values->uninstall_string.empty()) {
                continue;
            }
            InstallRegistration registration;
            registration.hive = hive;
            registration.key_path = std::string(key_path);
            registration.install_location = normpath(values->install_location);
            registration.uninstall_string = values->uninstall_string;
            registration.display_version = values->display_version;
            if (!values->estimated_size.empty()) {
                registration.estimated_size_kb = py::py_int(values->estimated_size);
            }
            registration.install_date = values->install_date;
            registration.view = view;
            return registration;
        }
    }
    return std::nullopt;
}

std::optional<InstallRegistration> discover_install_registration(const ReleaseIdentity& identity) {
    return discover_install_registration(identity.uninstall_key_path());
}

std::filesystem::path running_app_executable() {
    return detail::executable_path();
}

std::filesystem::path running_app_dir() {
    return running_app_executable().parent_path();
}

InstallProbe make_install_probe(const std::optional<InstallRegistration>& registration,
                                std::filesystem::path running_dir) {
    InstallProbe probe;
    probe.running_dir = std::move(running_dir);
    if (registration) {
        probe.registered = true;
        probe.all_users = registration->hive == RegistryHive::LocalMachine;
        probe.install_dir = registration->install_location;
    }
    return probe;
}

InstallProbe probe_install(const ReleaseIdentity& identity) {
    return make_install_probe(discover_install_registration(identity), running_app_dir());
}

} // namespace fv::update
