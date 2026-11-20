#include "update_native/storage_actions.h"
#include <utility>

namespace fv::update_native {
// Windows scanners can briefly hold a just-written image or its directory after a process exits.
void StorageActions::move_tree(const std::filesystem::path& from, const std::filesystem::path& to) {
    download::retry_while_locked(
        "Update directory",
        [&] { storage_.install_parent().rename_child_exclusive(from.filename(), to.filename()); },
        retry_.timeout, retry_.options);
}
StorageActions::StorageActions(const JournalContext& context, std::string_view id)
    : storage_(context, id), failed_name_(storage_.paths().failed_dir.filename()) {}
void StorageActions::move_old_to_rollback() {
    const auto& p = storage_.paths();
    move_tree(p.install_dir, p.rollback_dir);
}
void StorageActions::move_candidate_to_install() {
    const auto& p = storage_.paths();
    move_tree(p.candidate_dir, p.install_dir);
}
void StorageActions::move_new_to_failed() {
    const auto& p = storage_.paths();
    move_tree(p.install_dir, failed_name_);
}
void StorageActions::move_rollback_to_install() {
    const auto& p = storage_.paths();
    move_tree(p.rollback_dir, p.install_dir);
}
} // namespace fv::update_native
