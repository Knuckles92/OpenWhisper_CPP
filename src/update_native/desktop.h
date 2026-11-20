#pragma once

#include <filesystem>
#include <span>

#include "base/paths.h"
#include "update/apply.h"
#include "update_native/journal.h"

namespace fv::update_native {
// What decides whether this process may replace its own installation (native_apply_eligible).
// win/desktop.cpp reads the facts; the rule is the same everywhere.
struct NativeEligibility {
    // The process token could be read and is not elevated: a per-user transaction never runs
    // with administrator rights.
    bool standard_user = false;
    // The registration found for this product and the folder this process runs from.
    update::InstallProbe probe;
    // The data root is the per-user default, the one the helper resolves on its own.
    bool default_data_root = false;
    // This process is the registered folder's application program.
    bool registered_program = false;
    // The installed release inventory (the update manifest) is in that folder.
    bool inventory_present = false;
    // That folder is under a Program Files root (_is_under_program_files): a per-user process
    // cannot replace it, so the setup updates it.
    bool under_program_files = false;
};
// True only for the registered per-user installation running as its own, unelevated user. An
// all-users (HKLM) registration, an elevated process, a copy run from elsewhere or one without
// its inventory updates through the setup instead.
bool native_update_eligible(const NativeEligibility&);
// True when `folder` is one of `roots` (the ProgramFiles, ProgramFiles(x86) and ProgramW6432
// folders) or inside one, compared as canonical paths (case-insensitively on Windows). Empty
// roots are skipped.
bool under_program_files(const std::filesystem::path& folder,
                         std::span<const std::filesystem::path> roots);

#ifdef _WIN32
// Available only to the registered per-user installation using its normal update/data root.
update::NativeUpdateActions native_update_actions(const Paths&, const update::InstallProbe&);
#else
inline update::NativeUpdateActions native_update_actions(const Paths&,
                                                         const update::InstallProbe&) {
    return {};
}
#endif
} // namespace fv::update_native
