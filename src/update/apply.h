#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "download/verified_download.h"
#include "update/check.h"
#include "update/download.h"

// Turning a found release into something to run (services/app_update.py: apply_update,
// _apply_native_update, discard_prepared_result; ui_qt/ui_controller.py:
// _setup_installer_command): the prepared native transaction of an eligible per-user install,
// else the verified Inno setup run silently over the running install.
namespace fv::update {

// The program to start after this process lets go of the install, and its arguments.
struct Handoff {
    std::filesystem::path setup_exe;
    std::vector<std::string> args;
    std::string transaction_id{};
    friend bool operator==(const Handoff&, const Handoff&) = default;
};

struct NativeUpdateActions {
    // Runs on the components worker. The application supplies its trusted install and data roots.
    // Throws UpdateApplyError; UpdateRequiresSetup when only the setup can update this install,
    // UpdateCanceled for a cancel.
    std::function<Handoff(const std::filesystem::path&, std::string_view digest,
                          std::string_view old_version, std::string_view new_version,
                          const CancelToken&)>
        prepare;
    std::function<void(const Handoff&)> discard;
};

// "/FIORIVOCEUPDATE=1": tells the setup it runs as an update (it relaunches the app after).
std::string setup_update_switch();

// The verified setup run silently over the running install: /SP- /SILENT /NORESTART, the update
// switch, /ALLUSERS for an HKLM registration (else /CURRENTUSER), /DIR=<running folder> and
// /LOG=<updates folder>\setup.log.
Handoff setup_command(const std::filesystem::path& setup_exe, const InstallProbe&,
                      const std::filesystem::path& updates_dir);

// Downloads and verifies what `result` offers and returns the hand-off. force_setup takes the
// setup whatever the apply mode. Native mode downloads and prepares the versioned archive; when
// `prepare` throws UpdateRequiresSetup (update/apply_error.h) and the release has a ready setup,
// that verified setup is handed off instead, as _apply_native_update does. Any other
// UpdateApplyError never selects something else to install.
// Throws AppUpdateError: "No installer is available." without a release, cannot_install_message()
// when this copy cannot run the setup, "The update was canceled." for a canceled preparation, a
// preparation's own message, and the download's message otherwise. Blocking: on the components
// strand.
Handoff apply_update(const DownloadContext&, const UpdateCheckResult&,
                     const download::ProgressCallback& progress = {},
                     const CancelToken& cancel = {}, bool force_setup = false,
                     const NativeUpdateActions& native = {});

// Gives up a hand-off that will not run. A setup exe is kept for the next attempt to reuse (the
// next start prunes it); a native transaction (M10) is abandoned here.
void discard_prepared_result(const Handoff&, const NativeUpdateActions& native = {});

} // namespace fv::update
