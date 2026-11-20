#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string_view>

#include "base/cancel.h"
#include "update/apply.h"
#include "update_native/journal.h"

namespace fv::update_native {
// Called as a preparation passes "extracted" (the verified candidate is on disk), "preserved"
// (the uninstaller files are in it) and "staged" (the helper is in the transaction). Tests cancel
// or fail from it.
using PrepareHook = std::function<void(std::string_view point)>;

// Bytes of the regular files under `root`, symbolic links to folders not followed and files that
// cannot be read skipped (tree_size_bytes).
std::uint64_t tree_size_bytes(const std::filesystem::path& root);

// Free bytes on the drive of a path; disk_free_bytes in production.
using FreeBytes = std::function<std::uint64_t(const std::filesystem::path&)>;

// prepare_candidate's two check_free_space calls, each with a 1.15 margin: beside the install
// (its parent), room for the current tree and the unpacked release, because the rollback tree
// stays on disk beside the new one until the update is confirmed; in the update cache, room for
// the archive. Throws UpdateError "Not enough disk space: <needed> bytes needed, <free> free."
// `free_bytes` empty: the disk's.
void check_preparation_space(const std::filesystem::path& install_dir, std::uint64_t install_bytes,
                             std::uint64_t unpacked_bytes,
                             const std::filesystem::path& updates_root, std::uint64_t archive_bytes,
                             const FreeBytes& free_bytes = {});

// Prepares a verified candidate beside the current-user installation. The installed application
// stays active until the detached helper receives the transaction ID after the UI handoff.
// A cancel is honored during the extraction and at every step after it (UpdateCanceled). Any
// failure after the journal exists leaves the installation untouched, marks the transaction
// superseded (OpenWhisper's rolled_back: nothing was installed) and removes the candidate, the
// archive and the transaction it wrote (_discard_preparation), so a later attempt downloads again.
// A failure before the journal exists keeps the archive, as OpenWhisper's does.
update::Handoff prepare_update(const JournalContext&, const std::filesystem::path& archive,
                               std::string_view archive_sha256, std::string_view current_version,
                               std::string_view release_version, std::uint32_t parent_pid,
                               std::int64_t creation_ticks, const CancelToken& cancel = {},
                               const PrepareHook& hook = {});
// Releases a prepared update that will not be handed off (abandon_prepared_update): the journal
// becomes superseded and the candidate, the archive and the transaction it wrote are collected
// (OpenWhisper's abandon_prepared_update ends rolled_back and does the same). The installed
// tree is never touched. The caller holds the update gates. Throws UpdateError when the hand-off
// is not this installation's.
void abandon_prepared_update(const JournalContext&, const update::Handoff&);
} // namespace fv::update_native
