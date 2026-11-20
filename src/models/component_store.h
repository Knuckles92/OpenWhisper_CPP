#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "http/client.h"
#include "models/catalog.h"
#include "models/component_leases.h"

// Installed components on disk (services/components.py:681-707, 1076-2146): the sentinel and
// manifest, sizes, startup recovery, verified downloads, extraction, payload checks, the
// crash-safe swap and removal. Layout under the components root:
//   <id>/                 an install: its files, manifest.json (the catalog entry, indent 2) and
//                         .installed (the version, written last: the only "installed" signal)
//   <id>.old/             the previous tree during a swap or a removal
//   .staging/<id>.<pid>/  a tree being assembled
//   .cache/<archive>      downloaded archives (and <archive>.part while downloading)
//   .pending/<id>/, .pending/<id>.remove
//                         a change to a component the running process has loaded, applied by
//                         settle_staged_changes() at the next start (architecture 5.7)
// Blocking: never on the GUI thread.
namespace fv::models {

// (phase, done bytes, total bytes); install_phase:: names the phases. Called on the installing
// thread.
using InstallProgress =
    std::function<void(std::string_view phase, std::int64_t done, std::int64_t total)>;

// components._download_verified: one file to `destination` through
// download::download_verified, keeping a cancelled transfer in <destination>.part for resume,
// progress reported as (phase, offset_base + done, grand_total). A certificate failure names the
// URL's host. Throws ComponentCanceled or ComponentError (the downloader's message).
void download_component_file(http::Client& client, std::string_view url,
                             std::string_view sha256_hex, std::int64_t size_bytes,
                             const std::filesystem::path& destination,
                             const InstallProgress& progress, const CancelToken& cancel,
                             std::int64_t offset_base = 0, std::int64_t grand_total = 0);

// read_manifest of a manifest.json path: the object, or nullopt when it is missing,
// unreadable, not an object, or has a non-string platform, a bool or non-integer component_api,
// a non-string python_abi, or a bool or non-scalar version.
std::optional<Json> read_manifest_file(const std::filesystem::path& manifest);

// What install() or uninstall() did.
enum class ChangeOutcome {
    Done,             // the change is in place
    StagedForRestart, // the component is loaded in this process; the change applies next start
};

// File operations the store makes, replaceable so tests can make one fail as OpenWhisper's tests
// patch os.replace and shutil.rmtree.
struct StoreFileOps {
    // os.replace: throws std::filesystem::filesystem_error.
    std::function<void(const std::filesystem::path& from, const std::filesystem::path& to)> replace;
    // shutil.rmtree(ignore_errors=True) of a directory: removes what it can, never throws.
    std::function<void(const std::filesystem::path&)> remove_tree;
};

struct ComponentStoreOptions {
    // Platform, GPU and macOS version the store's decisions use. Its is_installed is ignored:
    // the store answers that itself. nullopt: this computer (Host::current).
    std::optional<Host> host;
    StoreFileOps file_ops;             // empty members: the real operations
    ComponentLeases* leases = nullptr; // nullptr: component_leases()
    // Free bytes of a drive (shutil.disk_usage(path).free); empty: the real figure.
    std::function<std::uint64_t(const std::filesystem::path&)> free_bytes;
    // _walk_size_bytes; empty: the real walk.
    std::function<std::int64_t(const std::filesystem::path&)> walk_size;
    // Speech-runtime renames retry this long while Windows reports the tree locked.
    std::chrono::milliseconds runtime_swap_timeout{15'000};
    std::chrono::milliseconds runtime_swap_poll{500};
    const Clock* clock = nullptr; // nullptr: system_clock()
};

// Affinity: thread-safe; one install or removal per component at a time (the coordinator's
// begin_install, and the leases, keep it so).
class ComponentStore {
public:
    // `components_root` is Paths::components_root() in the app. `client` downloads archives.
    ComponentStore(std::filesystem::path components_root, http::Client& client,
                   ComponentStoreOptions options = {});
    ComponentStore(const ComponentStore&) = delete;
    ComponentStore& operator=(const ComponentStore&) = delete;

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path component_dir(std::string_view component_id) const;
    // Extraction scratch; on the install's volume, so the swap is a rename.
    std::filesystem::path staging_dir() const;
    std::filesystem::path cache_dir() const;
    std::filesystem::path pending_dir() const;

    // The host decisions read, with is_installed answered by this store.
    const Host& host() const { return host_; }

    // A complete tree is present: the sentinel decides, so a tree left by an interrupted extract
    // is not installed. A loaded component stays installed for this run whatever change is
    // staged for it: staged_change() tells.
    bool is_installed(std::string_view component_id) const;

    // The change install() or uninstall() staged for the next start: a removal marker, else a
    // complete tree under .pending (an incomplete one is discarded at the next start).
    StagedChange staged_change(std::string_view component_id) const;

    // The manifest, or nullopt when it is missing, unreadable, not an object, or has a
    // non-string platform, a bool or non-integer component_api, a non-string python_abi, or a
    // bool or non-scalar version.
    std::optional<Json> read_manifest(std::string_view component_id) const;

    // Bytes on disk of a component tree (links counted as themselves). A committed install's
    // size is remembered until its folder or sentinel changes.
    std::int64_t installed_size_bytes(std::string_view component_id) const;

    // Startup recovery: removes .staging; for every <id>.old, a complete one whose <id> is
    // missing is moved back (kept when that fails: it may be the only complete copy), a
    // complete one whose <id> exists is removed, an incomplete one is removed.
    void prune_orphans();

    // Applies the changes staged for loaded components (install trees and removals under
    // .pending), before anything loads a runtime. A change that fails is logged and kept.
    void settle_staged_changes();

    // Downloads, verifies, extracts, validates and swaps in `entry` (a catalog entry) for
    // `component_id`, reporting progress. Throws ComponentError with the user's message
    // (checked before any download: an entry without archives, an incompatible entry, too
    // little disk space), ComponentCanceled when `cancel` fires. A component this process has
    // loaded (leased) is not replaced: the finished tree waits under .pending and the result is
    // StagedForRestart.
    ChangeOutcome install(std::string_view component_id, const Json& entry,
                          const InstallProgress& progress, const CancelToken& cancel);

    // Installs a tree `fill` assembles in a staging folder instead of downloaded archives
    // (adoption of another install's files): the payload is validated, `entry` written as its
    // manifest, and the tree swapped in (or staged) as install() does.
    ChangeOutcome install_tree(std::string_view component_id, const Json& entry,
                               const std::function<void(const std::filesystem::path&)>& fill,
                               const CancelToken& cancel);

    // Removes a component: renamed to <id>.old first, so it stops counting as installed even
    // when deleting is slow, then deleted. Throws ComponentError "Could not fully remove
    // '<id>': ..." or "...; leftover files remain.". A loaded component is marked for removal
    // at the next start instead (StagedForRestart).
    ChangeOutcome uninstall(std::string_view component_id);

    // _check_free_space: ComponentError "Not enough disk space: {needed} needed, {free} free on
    // this drive." when the root's drive lacks required_bytes x 1.15.
    void check_free_space(std::int64_t required_bytes) const;

    // _describe_disk_error: "The drive ran out of space while installing.", "A file could not
    // be written, which usually means antivirus software blocked it. Try adding an exclusion
    // for <root>.", or "Installation failed: <error>".
    std::string describe_disk_error(const std::system_error& error) const;

    // download_component_file through this store's client.
    void download_verified(std::string_view url, std::string_view sha256_hex,
                           std::int64_t size_bytes, const std::filesystem::path& destination,
                           const InstallProgress& progress, const CancelToken& cancel,
                           std::int64_t offset_base = 0, std::int64_t grand_total = 0);

    // _replace_speech_runtime: renames a runtime tree, retrying while Windows reports it locked
    // (scanners hold freshly extracted files for seconds); `cancel` stops the wait
    // (ComponentCanceled) and leaves the source where it was.
    void replace_speech_runtime(const std::filesystem::path& from, const std::filesystem::path& to,
                                const CancelToken& cancel);

    // _validate_component_payload for the speech runtimes: the runtime library must be where
    // the platform's release puts it. Other components are not checked.
    void validate_payload(std::string_view component_id, const std::filesystem::path& tree) const;

private:
    void remove_tree(const std::filesystem::path& path) const;
    void replace(const std::filesystem::path& from, const std::filesystem::path& to) const;
    void move_component_tree(std::string_view component_id, const std::filesystem::path& from,
                             const std::filesystem::path& to, const CancelToken& cancel);
    void swap_into_place(std::string_view component_id, const std::filesystem::path& staging,
                         const std::filesystem::path& destination,
                         const std::filesystem::path& rollback, const CancelToken& cancel);
    std::vector<std::filesystem::path> download_archives(std::string_view component_id,
                                                         const Json& archives,
                                                         std::int64_t download_total,
                                                         const InstallProgress& progress,
                                                         const CancelToken& cancel);
    void extract_archive(const Json& archive, const std::filesystem::path& archive_path,
                         const std::filesystem::path& staging, const InstallProgress& progress,
                         const CancelToken& cancel);
    ChangeOutcome remove_now(std::string_view component_id);
    // Takes the mutation lease; true when the component is loaded and the change must be staged.
    bool claim_for_change(std::string_view component_id,
                          std::optional<ComponentLeases::Mutation>& mutation);
    ChangeOutcome build_and_commit(std::string_view component_id, const Json& entry,
                                   const std::function<void(const std::filesystem::path&)>& fill,
                                   const InstallProgress& progress, const CancelToken& cancel,
                                   const std::vector<std::filesystem::path>& archive_paths,
                                   bool stage);

    std::filesystem::path root_;
    http::Client& client_;
    ComponentStoreOptions options_;
    Host host_;
    ComponentLeases& leases_;

    // Tree sizes by install path, with the install's fingerprint (folder and sentinel identity,
    // modification times, sentinel size) they were measured under.
    mutable std::mutex size_mutex_;
    mutable std::map<std::filesystem::path, std::pair<std::string, std::int64_t>> size_cache_;
};

} // namespace fv::models
