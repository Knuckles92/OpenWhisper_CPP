#pragma once
#include "update/gates.h"
#include "update_native/journal.h"
#include "update_native/parent_process.h"
#include <functional>

namespace fv::update_native {
using RecoveryLauncher =
    std::function<bool(const std::filesystem::path&, const std::vector<std::string>&)>;
// Caller has checked the application and updater gates. Only interrupted commits require a
// detached helper. Unstarted preparations are abandoned after their identified parent exits.
bool recover_before_start(const JournalContext&, update::MutexApi&, std::uint32_t launcher_pid,
                          std::int64_t launcher_creation, const RecoveryLauncher&,
                          const std::function<bool(const UpdateJournal&)>& retire = {});
} // namespace fv::update_native
