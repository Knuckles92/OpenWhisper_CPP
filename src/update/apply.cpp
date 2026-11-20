#include "update/apply.h"

#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "pyport/text.h"
#include "update/apply_error.h"

namespace fv::update {

namespace {

constexpr log::Logger kLog("services.app_update");

} // namespace

std::string setup_update_switch() {
    return "/" + py::upper(identity().product) + "UPDATE=1";
}

Handoff setup_command(const std::filesystem::path& setup_exe, const InstallProbe& install,
                      const std::filesystem::path& updates_dir) {
    Handoff handoff;
    handoff.setup_exe = setup_exe;
    handoff.args = {
        "/SP-",
        "/SILENT",
        "/NORESTART",
        setup_update_switch(),
        install.all_users ? "/ALLUSERS" : "/CURRENTUSER",
        "/DIR=" + path_to_utf8(install.running_dir),
        "/LOG=" + path_to_utf8(updates_dir / "setup.log"),
    };
    return handoff;
}

Handoff apply_update(const DownloadContext& context, const UpdateCheckResult& result,
                     const download::ProgressCallback& progress, const CancelToken& cancel,
                     bool force_setup, const NativeUpdateActions& native) {
    if (!result.release) {
        throw AppUpdateError("No installer is available.");
    }
    const auto& release = *result.release;
    const auto mode = force_setup ? ApplyMode::Setup : result.apply_mode;
    if (mode == ApplyMode::Native && native.prepare && asset_ready(release.native_asset)) {
        if (context.platform != "win32" || context.install.all_users ||
            detect_channel(context.install) != InstallChannel::Installer)
            throw AppUpdateError(cannot_install_message());
        const auto& asset = *release.native_asset;
        std::filesystem::create_directories(context.updates_dir);
        const auto archive = download_release_asset(context.client, context.feed, asset,
                                                    context.updates_dir, progress, cancel);
        // _apply_native_update's except clauses.
        try {
            return native.prepare(archive, validated_asset_digest(asset), result.current_version,
                                  release.version, cancel);
        } catch (const UpdateRequiresSetup& error) {
            if (!asset_ready(release.setup_asset)) {
                throw AppUpdateError(error.what());
            }
            // Only a payload this install cannot take natively falls through to the verified
            // setup. An integrity failure never selects something else to install.
            kLog.info("Using setup for an incompatible native payload");
        } catch (const UpdateCanceled&) {
            throw AppUpdateError("The update was canceled.");
        } catch (const UpdateApplyError& error) {
            throw AppUpdateError(error.what());
        } catch (const AppUpdateError&) {
            throw;
        } catch (const std::exception&) {
            if (cancel.cancelled()) {
                throw AppUpdateError("The update was canceled.");
            }
            throw;
        }
    }
    if ((mode == ApplyMode::Setup || mode == ApplyMode::Native) &&
        asset_ready(release.setup_asset)) {
        const auto setup = download_installer(context, release, progress, cancel);
        return setup_command(setup, context.install, context.updates_dir);
    }
    throw AppUpdateError(cannot_install_message());
}

void discard_prepared_result(const Handoff& handoff, const NativeUpdateActions& native) {
    if (!handoff.transaction_id.empty() && native.discard)
        native.discard(handoff);
}

} // namespace fv::update
