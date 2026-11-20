#include "models/component_store.h"

#include <algorithm>
#include <cerrno>
#include <format>
#include <vector>

#include "archive/errors.h"
#include "archive/safe_extract.h"
#include "base/log.h"
#include "base/utf8.h"
#include "download/verified_download.h"
#include "download/verified_files.h"
#include "models/detail/files.h"
#include "models/detail/fs_native.h"
#include "models/runtime_resolver.h"
#include "pyport/json.h"

namespace fv::models {

namespace fs = std::filesystem;

namespace {

constexpr log::Logger kLog("services.components");

// Written last, so its presence means "this tree is complete".
constexpr std::string_view kSentinelName = ".installed";
constexpr std::string_view kManifestName = "manifest.json";
constexpr std::string_view kRemoveSuffix = ".remove";

constexpr std::string_view kSpeechRuntimeIncomplete =
    "The speech runtime is missing required files.";

std::string utf8(const fs::path& path) {
    return fv::base_utf8::path_to_utf8(path);
}

fs::path from_utf8(std::string_view text) {
    return fv::base_utf8::path_from_utf8(text);
}

bool is_dir(const fs::path& path) {
    std::error_code error;
    return fs::is_directory(path, error);
}

bool is_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

bool path_exists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

bool is_speech_runtime(std::string_view component_id) {
    const auto ids = runtime_ids();
    return std::ranges::find(ids, component_id) != ids.end();
}

// PermissionError: the errno Python gives a refused file operation (a sharing or lock violation
// on Windows included).
bool is_permission_error(const std::system_error& error) {
    const int code = detail::python_errno(error.code());
    return code == EACCES || code == EPERM;
}

void require_files(const fs::path& root, std::initializer_list<std::string_view> names) {
    for (const std::string_view name : names) {
        if (!is_file(root / from_utf8(name))) {
            throw ComponentError(std::string(kSpeechRuntimeIncomplete));
        }
    }
}

std::int64_t archive_size(const Json& archive) {
    const Json* size = detail::member(archive, "size_bytes");
    return size == nullptr ? 0 : detail::to_int(*size);
}

std::string archive_text(const Json& archive, std::string_view key) {
    const Json* value = detail::member(archive, key);
    return value == nullptr ? std::string() : detail::py_str(*value);
}

void discard_cached_archives(const std::vector<fs::path>& paths) {
    for (const auto& path : paths) {
        std::error_code error;
        fs::remove(path, error);
    }
}

// _write_install_markers: the entry as manifest.json (json.dump, indent 2), then the sentinel.
void write_install_markers(const fs::path& staging, const Json& entry) {
    py::DumpOptions options;
    options.indent = 2;
    detail::write_text(staging / kManifestName, py::dumps(entry, options));
    const Json* version = detail::member(entry, "version");
    detail::write_text(staging / kSentinelName,
                       version == nullptr ? std::string() : detail::py_str(*version));
}

std::string fingerprint(const fs::path& directory) {
    const auto tree = detail::file_identity(directory);
    const auto sentinel = detail::file_identity(directory / kSentinelName);
    if (!tree || !sentinel) {
        return {};
    }
    return std::format("{}:{}:{}:{}:{}:{}:{}", tree->volume, tree->index, tree->mtime_ns,
                       sentinel->volume, sentinel->index, sentinel->mtime_ns, sentinel->size);
}

} // namespace

ComponentStore::ComponentStore(fs::path components_root, http::Client& client,
                               ComponentStoreOptions options)
    : root_(std::move(components_root)), client_(client), options_(std::move(options)),
      leases_(options_.leases != nullptr ? *options_.leases : component_leases()) {
    host_ = options_.host ? *options_.host : Host::current({});
    host_.is_installed = [this](std::string_view component_id) {
        return is_installed(component_id);
    };
}

fs::path ComponentStore::component_dir(std::string_view component_id) const {
    return root_ / from_utf8(component_id);
}

fs::path ComponentStore::staging_dir() const {
    return root_ / ".staging";
}

fs::path ComponentStore::cache_dir() const {
    return root_ / ".cache";
}

fs::path ComponentStore::pending_dir() const {
    return root_ / ".pending";
}

StagedChange ComponentStore::staged_change(std::string_view component_id) const {
    const fs::path pending = pending_dir() / from_utf8(component_id);
    if (is_file(fs::path(pending).concat(kRemoveSuffix))) {
        return StagedChange::Removal;
    }
    if (is_file(pending / kSentinelName)) {
        return StagedChange::Install;
    }
    return StagedChange::None;
}

void ComponentStore::remove_tree(const fs::path& path) const {
    if (options_.file_ops.remove_tree) {
        options_.file_ops.remove_tree(path);
    } else {
        detail::remove_tree(path);
    }
}

void ComponentStore::replace(const fs::path& from, const fs::path& to) const {
    if (options_.file_ops.replace) {
        options_.file_ops.replace(from, to);
    } else {
        fs::rename(from, to);
    }
}

bool ComponentStore::is_installed(std::string_view component_id) const {
    return is_file(component_dir(component_id) / kSentinelName);
}

std::optional<Json> ComponentStore::read_manifest(std::string_view component_id) const {
    return read_manifest_file(component_dir(component_id) / kManifestName);
}

std::optional<Json> read_manifest_file(const fs::path& manifest) {
    const auto bytes = detail::read_bytes(manifest);
    if (!bytes) {
        return std::nullopt;
    }
    Json data;
    try {
        data = py::loads(*bytes);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!data.is_object()) {
        return std::nullopt;
    }
    // Compatibility fields must be plain JSON scalars when present.
    const auto present = [&data](std::string_view key) -> const Json* {
        const Json* value = detail::member(data, key);
        return value != nullptr && !value->is_null() ? value : nullptr;
    };
    if (const Json* platform = present("platform"); platform && !platform->is_string()) {
        return std::nullopt;
    }
    if (const Json* api = present("component_api"); api && !api->is_number_integer()) {
        return std::nullopt;
    }
    if (const Json* abi = present("python_abi"); abi && !abi->is_string()) {
        return std::nullopt;
    }
    if (const Json* version = present("version");
        version && !(version->is_string() || version->is_number())) {
        return std::nullopt;
    }
    return std::optional<Json>(std::in_place, std::move(data));
}

std::int64_t ComponentStore::installed_size_bytes(std::string_view component_id) const {
    const fs::path directory = component_dir(component_id);
    const std::string print = fingerprint(directory);
    if (!print.empty()) {
        std::lock_guard lock(size_mutex_);
        const auto it = size_cache_.find(directory);
        if (it != size_cache_.end() && it->second.first == print) {
            return it->second.second;
        }
    }
    const std::int64_t total =
        options_.walk_size ? options_.walk_size(directory) : detail::walk_size_bytes(directory);
    if (!print.empty()) {
        std::lock_guard lock(size_mutex_);
        size_cache_[directory] = {print, total};
    }
    return total;
}

void ComponentStore::prune_orphans() {
    remove_tree(staging_dir());
    if (!is_dir(root_)) {
        return;
    }
    std::vector<fs::path> names;
    std::error_code error;
    for (fs::directory_iterator it(root_, error), end; !error && it != end; it.increment(error)) {
        names.push_back(it->path());
    }
    for (const auto& old_path : names) {
        const std::string name = utf8(old_path.filename());
        if (!name.ends_with(".old")) {
            continue;
        }
        const fs::path destination = root_ / from_utf8(name.substr(0, name.size() - 4));
        const bool complete = is_file(old_path / kSentinelName);
        if (complete && !is_dir(destination)) {
            try {
                replace(old_path, destination);
                kLog.info("Restored component tree from interrupted swap: {}",
                          utf8(destination.filename()));
            } catch (const std::system_error& failure) {
                // Never delete the only complete rollback after a failed restore.
                kLog.error_with(failure,
                                std::format("Could not restore component tree from {}; preserving "
                                            ".old rollback for a later retry",
                                            utf8(old_path)));
            }
            continue;
        }
        // Superseded by a committed install, or incomplete: either way not needed.
        remove_tree(old_path);
    }
}

void ComponentStore::settle_staged_changes() {
    const fs::path pending = pending_dir();
    if (!is_dir(pending)) {
        return;
    }
    std::vector<fs::path> entries;
    std::error_code error;
    for (fs::directory_iterator it(pending, error), end; !error && it != end; it.increment(error)) {
        entries.push_back(it->path());
    }
    std::ranges::sort(entries);
    for (const auto& entry : entries) {
        const std::string name = utf8(entry.filename());
        if (name.ends_with(kRemoveSuffix)) {
            const std::string component_id = name.substr(0, name.size() - kRemoveSuffix.size());
            try {
                remove_now(component_id);
                std::error_code removed;
                fs::remove(entry, removed);
            } catch (const std::exception& failure) {
                kLog.error_with(failure, std::format("Could not remove component '{}' staged for "
                                                     "removal; it is retried at the next start",
                                                     component_id));
            }
            continue;
        }
        if (!is_dir(entry)) {
            continue;
        }
        if (!is_file(entry / kSentinelName)) {
            remove_tree(entry);
            continue;
        }
        const fs::path destination = component_dir(name);
        const fs::path rollback = fs::path(destination).concat(".old");
        try {
            remove_tree(rollback);
            swap_into_place(name, entry, destination, rollback, CancelToken());
            remove_tree(rollback);
            kLog.info("Installed component '{}' staged at the last run", name);
        } catch (const std::exception& failure) {
            kLog.error_with(failure, std::format("Could not install component '{}' staged at the "
                                                 "last run; it is retried at the next start",
                                                 name));
        }
    }
    std::error_code removed;
    fs::remove(pending, removed); // only when empty
}

void ComponentStore::check_free_space(std::int64_t required_bytes) const {
    const auto shortfall = download::free_space_shortfall(
        root_, required_bytes, download::kSpaceMargin, options_.free_bytes);
    if (shortfall) {
        throw ComponentError(
            download::describe_free_space_shortfall(shortfall->first, shortfall->second));
    }
}

std::string ComponentStore::describe_disk_error(const std::system_error& error) const {
    const int code = detail::python_errno(error.code());
    const int winerror = detail::python_winerror(error.code());
    if (code == ENOSPC || winerror == 112) {
        return "The drive ran out of space while installing.";
    }
    if (code == EACCES || winerror == 5) {
        return "A file could not be written, which usually means antivirus software blocked it. "
               "Try adding an exclusion for " +
               utf8(root_) + ".";
    }
    return "Installation failed: " + download::os_error_text(error);
}

void ComponentStore::download_verified(std::string_view url, std::string_view sha256_hex,
                                       std::int64_t size_bytes, const fs::path& destination,
                                       const InstallProgress& progress, const CancelToken& cancel,
                                       std::int64_t offset_base, std::int64_t grand_total) {
    download_component_file(client_, url, sha256_hex, size_bytes, destination, progress, cancel,
                            offset_base, grand_total);
}

void download_component_file(http::Client& client, std::string_view url,
                             std::string_view sha256_hex, std::int64_t size_bytes,
                             const fs::path& destination, const InstallProgress& progress,
                             const CancelToken& cancel, std::int64_t offset_base,
                             std::int64_t grand_total) {
    // Name the host being contacted, so a blocked user allowlists the one this download needs.
    const auto host = detail::url_hostname(url);
    download::DownloadOptions options;
    options.describe_error = [host](const download::NetworkFailure& failure) {
        download::NetworkErrorText text;
        if (host) {
            text.hosts = {*host};
        }
        return download::describe_network_error(failure, text);
    };
    const download::ProgressCallback overall = [&](std::string_view phase, std::int64_t done,
                                                   std::int64_t) {
        if (progress) {
            progress(phase, offset_base + done, grand_total);
        }
    };
    try {
        download::download_verified(client, url, destination, size_bytes, sha256_hex, overall,
                                    cancel, true, options);
    } catch (const download::DownloadCanceled&) {
        throw ComponentCanceled();
    } catch (const download::DownloadError& error) {
        throw ComponentError(error.what());
    }
}

void ComponentStore::replace_speech_runtime(const fs::path& from, const fs::path& to,
                                            const CancelToken& cancel) {
    download::LockRetryOptions retry;
    retry.poll = options_.runtime_swap_poll;
    retry.locked = is_permission_error;
    retry.wait = [&cancel](std::chrono::milliseconds delay) {
        cancel.wait_for(delay);
    };
    retry.clock = options_.clock;
    download::retry_while_locked(
        utf8(to),
        [&] {
            if (cancel.cancelled()) {
                throw ComponentCanceled();
            }
            replace(from, to);
        },
        options_.runtime_swap_timeout, retry);
}

void ComponentStore::validate_payload(std::string_view component_id, const fs::path& tree) const {
    if (!is_speech_runtime(component_id)) {
        return;
    }
    const std::string tag = host_.platform.value_or(std::string());
    if (tag == platform::kDarwinArm64 || tag == platform::kDarwinX86_64) {
        const fs::path lib = tree / "nemo-speech" / "lib";
        if (component_id == component_id::kAsrNvidiaMetal) {
            require_files(lib, {"libnemo_speech_asr_c.dylib", "libggml-metal.dylib"});
        } else if (component_id == component_id::kAsrNvidiaCpu) {
            require_files(lib, {"libnemo_speech_asr_c.dylib"});
        } else {
            throw ComponentError(std::string(kSpeechRuntimeIncomplete));
        }
        return;
    }
    if (tag == platform::kLinuxX86_64) {
        if (!component_id.starts_with("asr-nvidia") ||
            !is_file(tree / "nemo-speech" / "lib" / "libnemo_speech_asr_c.so")) {
            throw ComponentError(std::string(kSpeechRuntimeIncomplete));
        }
        return;
    }
    // Windows: OpenWhisper's runtime also carried an embedded Python (python.exe, python312.dll,
    // python312.zip, and a python312._pth it wrote); FioriVoce loads the library in process.
    if (!component_id.starts_with("asr-nvidia")) {
        throw ComponentError(std::string(kSpeechRuntimeIncomplete));
    }
    require_files(tree, {"bin/nemo_speech_asr_c.dll"});
}

void ComponentStore::move_component_tree(std::string_view component_id, const fs::path& from,
                                         const fs::path& to, const CancelToken& cancel) {
    if (is_speech_runtime(component_id)) {
        replace_speech_runtime(from, to, cancel);
    } else {
        replace(from, to);
    }
}

void ComponentStore::swap_into_place(std::string_view component_id, const fs::path& staging,
                                     const fs::path& destination, const fs::path& rollback,
                                     const CancelToken& cancel) {
    bool moved_aside = false;
    const auto restore = [&] {
        // The second rename failed after the live tree was moved aside: put it back first.
        if (moved_aside && is_dir(rollback) && !is_dir(destination)) {
            try {
                move_component_tree(component_id, rollback, destination, CancelToken());
            } catch (const std::system_error& failure) {
                kLog.error_with(failure,
                                std::format("Failed to restore previous {} install after swap "
                                            "error",
                                            component_id));
            }
        }
    };
    try {
        if (is_dir(destination)) {
            move_component_tree(component_id, destination, rollback, cancel);
            moved_aside = true;
        }
        move_component_tree(component_id, staging, destination, cancel);
    } catch (const ComponentCanceled&) {
        restore();
        throw;
    } catch (const std::system_error& failure) {
        restore();
        throw ComponentError(describe_disk_error(failure));
    }
}

std::vector<fs::path> ComponentStore::download_archives(std::string_view component_id,
                                                        const Json& archives,
                                                        std::int64_t download_total,
                                                        const InstallProgress& progress,
                                                        const CancelToken& cancel) {
    // Speech runtimes re-check a cached archive before reusing it.
    const bool recheck_cached = is_speech_runtime(component_id);
    std::vector<fs::path> paths;
    std::int64_t consumed = 0;
    for (const Json& archive : archives) {
        const fs::path target = cache_dir() / from_utf8(archive_text(archive, "name"));
        const std::int64_t size = archive_size(archive);
        const std::string sha = archive_text(archive, "sha256");
        if (recheck_cached && path_exists(target)) {
            // _discard_mismatched_archive
            const std::string digest = download::sha256_file(target);
            if (digest != sha || static_cast<std::int64_t>(fs::file_size(target)) != size) {
                fs::remove(target);
            }
        }
        if (!path_exists(target)) {
            download_verified(archive_text(archive, "url"), sha, size, target, progress, cancel,
                              consumed, download_total);
        }
        consumed += size;
        paths.push_back(target);
    }
    return paths;
}

void ComponentStore::extract_archive(const Json& archive, const fs::path& archive_path,
                                     const fs::path& staging, const InstallProgress& progress,
                                     const CancelToken& cancel) {
    const archive::ExtractProgress extracted = [&progress](std::int64_t done, std::int64_t total) {
        if (progress) {
            progress(install_phase::kExtracting, done, total);
        }
    };
    // FioriVoce's catalog holds zip and nemo-tar archives only; OpenWhisper's wheel, Node and
    // transcribe.cpp extractors belong to components it does not ship.
    if (archive_text(archive, "extract") == "nemo-tar") {
        const Json* root = detail::member(archive, "root");
        const std::string folder = "nemo-speech";
        const std::string top =
            root != nullptr && detail::truthy(*root) ? detail::py_str(*root) : folder;
        archive::extract_native_tar(archive_path, staging, top, folder, extracted, cancel);
    } else {
        archive::extract_zip(archive_path, staging, extracted, cancel);
    }
}

bool ComponentStore::claim_for_change(std::string_view component_id,
                                      std::optional<ComponentLeases::Mutation>& mutation) {
    // A loaded runtime is leased for the life of the process (it can never be unloaded): its
    // folder is not replaced now, the change is staged for the next start.
    try {
        mutation.emplace(leases_.mutation(component_id));
        return false;
    } catch (const ComponentError&) {
        if (!leases_.in_use(component_id)) {
            throw;
        }
        return true;
    }
}

ChangeOutcome ComponentStore::install(std::string_view component_id, const Json& entry,
                                      const InstallProgress& progress, const CancelToken& cancel) {
    const Json* archives = detail::member(entry, "archives");
    if (archives == nullptr || !detail::truthy(*archives) || !archives->is_array()) {
        throw ComponentError("The catalog entry lists no files to download.");
    }
    if (const auto incompatible = check_compatibility(entry, host_)) {
        throw ComponentError(*incompatible);
    }
    std::optional<ComponentLeases::Mutation> mutation;
    const bool stage = claim_for_change(component_id, mutation);

    fs::create_directories(cache_dir());
    fs::create_directories(staging_dir());
    if (progress) {
        progress(install_phase::kResolving, 0, 0);
    }

    std::int64_t download_total = 0;
    for (const Json& archive : *archives) {
        download_total += archive_size(archive);
    }
    const Json* install_bytes = detail::member(entry, "install_bytes");
    check_free_space(download_total + (install_bytes ? detail::to_int(*install_bytes) : 0));

    const auto archive_paths =
        download_archives(component_id, *archives, download_total, progress, cancel);
    const auto fill = [&](const fs::path& staging) {
        std::size_t index = 0;
        for (const Json& archive : *archives) {
            extract_archive(archive, archive_paths[index++], staging, progress, cancel);
        }
    };
    return build_and_commit(component_id, entry, fill, progress, cancel, archive_paths, stage);
}

ChangeOutcome ComponentStore::install_tree(std::string_view component_id, const Json& entry,
                                           const std::function<void(const fs::path&)>& fill,
                                           const CancelToken& cancel) {
    if (const auto incompatible = check_compatibility(entry, host_)) {
        throw ComponentError(*incompatible);
    }
    std::optional<ComponentLeases::Mutation> mutation;
    const bool stage = claim_for_change(component_id, mutation);
    fs::create_directories(staging_dir());
    return build_and_commit(component_id, entry, fill, {}, cancel, {}, stage);
}

ChangeOutcome ComponentStore::build_and_commit(std::string_view component_id, const Json& entry,
                                               const std::function<void(const fs::path&)>& fill,
                                               const InstallProgress& progress,
                                               const CancelToken& cancel,
                                               const std::vector<fs::path>& archive_paths,
                                               bool stage) {
    const fs::path staging =
        staging_dir() / from_utf8(std::format("{}.{}", component_id, detail::current_pid()));
    remove_tree(staging);
    fs::create_directories(staging);

    const fs::path destination = component_dir(component_id);
    const fs::path rollback = fs::path(destination).concat(".old");
    ChangeOutcome outcome = ChangeOutcome::Done;
    try {
        fill(staging);
        validate_payload(component_id, staging);
        if (cancel.cancelled()) {
            throw ComponentCanceled();
        }
        if (progress) {
            progress(install_phase::kFinalizing, 0, 0);
        }
        write_install_markers(staging, entry);

        const Json* version = detail::member(entry, "version");
        const std::string version_text = version ? detail::py_str(*version) : "None";
        if (stage) {
            const fs::path pending = pending_dir() / from_utf8(component_id);
            remove_tree(pending);
            std::error_code removed;
            fs::remove(fs::path(pending).concat(kRemoveSuffix), removed);
            fs::create_directories(pending_dir());
            // A freshly extracted runtime is held by scanners just as on the way into place.
            move_component_tree(component_id, staging, pending, cancel);
            discard_cached_archives(archive_paths);
            kLog.info("Staged component '{}' version {} for the next start", component_id,
                      version_text);
            outcome = ChangeOutcome::StagedForRestart;
        } else {
            remove_tree(rollback);
            swap_into_place(component_id, staging, destination, rollback, cancel);
            // Only drop the previous tree and the cached archives once the new install is
            // committed in place.
            remove_tree(rollback);
            discard_cached_archives(archive_paths);
            kLog.info("Installed component '{}' version {}", component_id, version_text);
        }
    } catch (const ComponentError&) {
        remove_tree(staging);
        throw;
    } catch (const ComponentCanceled&) {
        remove_tree(staging);
        throw;
    } catch (const archive::ArchiveError& error) {
        remove_tree(staging);
        throw ComponentError(error.what());
    } catch (const archive::BadArchive& error) {
        remove_tree(staging);
        throw ComponentError(error.what());
    } catch (const archive::ExtractCanceled&) {
        remove_tree(staging);
        throw ComponentCanceled();
    } catch (const std::system_error& error) {
        remove_tree(staging);
        throw ComponentError(describe_disk_error(error));
    } catch (...) {
        remove_tree(staging);
        throw;
    }
    remove_tree(staging);
    return outcome;
}

ChangeOutcome ComponentStore::uninstall(std::string_view component_id) {
    std::optional<ComponentLeases::Mutation> mutation;
    try {
        mutation.emplace(leases_.mutation(component_id));
    } catch (const ComponentError&) {
        if (!leases_.in_use(component_id)) {
            throw;
        }
        // Loaded in this process: removed at the next start, before anything loads it.
        const fs::path pending = pending_dir() / from_utf8(component_id);
        remove_tree(pending);
        fs::create_directories(pending_dir());
        detail::write_text(fs::path(pending).concat(kRemoveSuffix), "");
        kLog.info("Component '{}' is in use; it is removed at the next start", component_id);
        return ChangeOutcome::StagedForRestart;
    }
    return remove_now(component_id);
}

ChangeOutcome ComponentStore::remove_now(std::string_view component_id) {
    const fs::path target = component_dir(component_id);
    const fs::path rollback = fs::path(target).concat(".old");
    bool target_present = is_dir(target);
    bool rollback_present = is_dir(rollback);
    if (!target_present && !rollback_present) {
        return ChangeOutcome::Done;
    }
    if (target_present) {
        // Rename first so the tree stops counting as installed even if deleting the (very
        // large) contents is slow or partly blocked by antivirus.
        if (rollback_present) {
            remove_tree(rollback);
        }
        try {
            replace(target, rollback);
            rollback_present = true;
        } catch (const std::system_error& failure) {
            remove_tree(target);
            if (is_dir(target) || is_dir(rollback)) {
                throw ComponentError(std::format("Could not fully remove '{}': {}", component_id,
                                                 describe_disk_error(failure)));
            }
            kLog.info("Removed component '{}'", component_id);
            return ChangeOutcome::Done;
        }
    }
    if (rollback_present || is_dir(rollback)) {
        remove_tree(rollback);
    }
    if (is_dir(target) || is_dir(rollback)) {
        throw ComponentError(
            std::format("Could not fully remove '{}'; leftover files remain.", component_id));
    }
    kLog.info("Removed component '{}'", component_id);
    return ChangeOutcome::Done;
}

} // namespace fv::models
