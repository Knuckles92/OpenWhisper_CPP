#include "update_native/prepare.h"

#include <filesystem>
#include <string>

#include "base/ids.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "pyport/text.h"
#include "update/update_contract.h"
#include "update_native/archive.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"
#include "update_native/terminal_cleanup.h"

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;

void check_cancel(const CancelToken& token) {
    if (token.cancelled())
        throw Canceled("The update was canceled.");
}

FileTable installed_manifest(const GuardedDirectory& install, std::string_view version) {
    try {
        const auto content = install.read_file(update::manifest_name(), max_manifest_bytes);
        return validate_manifest(Json::parse(content), version, version);
    } catch (const std::exception&) {
        throw RequiresSetup("This installation needs the setup package to update.");
    }
}

fs::path uninstall_command(std::string value, const JournalPaths& paths) {
    // Inno registers the absolute path, quoted when necessary. Do not interpret a shell command.
    value = py::strip(value);
    if (value.size() > 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    const auto file = path_from_utf8(value);
    const auto stem = py::lower(path_to_utf8(file.stem()));
    if (!file.is_absolute() || !fs::equivalent(file.parent_path(), paths.install_dir) ||
        py::lower(path_to_utf8(file.extension())) != ".exe" || !stem.starts_with("unins") ||
        stem.size() <= 5 || stem.find_first_not_of("0123456789", 5) != std::string::npos)
        throw RequiresSetup("The registered uninstaller needs the setup package.");
    return file.filename();
}

void stage_helper(const GuardedDirectory& install, const GuardedDirectory& transaction,
                  const JournalPaths& paths, const FileTable& managed, const CancelToken& token) {
    // The detached executable cannot keep DLLs in the install directory mapped during a swap.
    // Stage the installed version and its app-local libraries from its verified release manifest.
    for (const auto& [name, record] : managed) {
        if (name != updater_name &&
            (name.find('/') != std::string::npos || !py::lower(name).ends_with(".dll")))
            continue;
        check_cancel(token);
        const auto leaf = path_from_utf8(name);
        auto pin = install.pin_file(leaf);
        if (!pin || fs::file_size(paths.install_dir / leaf) != record.size ||
            download::sha256_file(paths.install_dir / leaf, true) != record.sha256)
            throw RequiresSetup("An installed updater file changed. Use the setup package.");
        transaction.copy_file_atomic_from(*pin, leaf);
    }
}
// Removes what a preparation that will never run wrote and the archive it was prepared from
// (_discard_preparation, _remove_transaction_payloads): its verified candidate, the archive,
// then its staged helper and journal. Each step is tried on its own, as the Python does, so a
// candidate holding an unknown file does not keep the archive. Whatever cannot be removed now is
// collected by a later start (the archive by prune_stale_downloads). `archive_sha256` empty: the
// archive is removed without its hash being checked again, as the Python's unlink is.
void collect_unstarted(const JournalContext& context, const UpdateJournal& journal,
                       std::string_view archive_sha256 = {}) {
    try {
        (void)cleanup_terminal_candidate(context, journal);
    } catch (const std::exception&) {
    }
    try {
        remove_transaction_archive(context, journal, archive_sha256);
    } catch (const std::exception&) {
    }
    try {
        (void)cleanup_finished_transaction(context, journal);
    } catch (const std::exception&) {
    }
}
} // namespace

std::uint64_t tree_size_bytes(const fs::path& root) {
    std::uint64_t total = 0;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        std::error_code ignored;
        if (!it->is_regular_file(ignored))
            continue;
        const auto size = it->file_size(ignored);
        if (!ignored)
            total += size;
    }
    return total;
}

void check_preparation_space(const fs::path& install_dir, std::uint64_t install_bytes,
                             std::uint64_t unpacked_bytes, const fs::path& updates_root,
                             std::uint64_t archive_bytes, const FreeBytes& free_bytes) {
    // app_update_apply.py _SPACE_MARGIN.
    constexpr double margin = 1.15;
    const auto check = [&free_bytes](const fs::path& path, std::uint64_t required) {
        const auto shortfall = download::free_space_shortfall(
            path, static_cast<std::int64_t>(required), margin, free_bytes);
        if (shortfall)
            throw UpdateError("Not enough disk space: " + std::to_string(shortfall->first) +
                              " bytes needed, " + std::to_string(shortfall->second) + " free.");
    };
    check(install_dir.parent_path(), install_bytes + unpacked_bytes);
    check(updates_root, archive_bytes);
}

void abandon_prepared_update(const JournalContext& context, const update::Handoff& handoff) {
    const auto bound = journal_paths(context, handoff.transaction_id);
    if (handoff.setup_exe != bound.transaction_dir / updater_name)
        throw UpdateError("The prepared update no longer matches this installation.");
    auto journal = load_journal(context, handoff.transaction_id);
    if (journal.state != TransactionState::Preparing && journal.state != TransactionState::Prepared)
        return;
    journal.state = TransactionState::Superseded;
    save_journal(context, journal);
    collect_unstarted(context, journal);
}

update::Handoff prepare_update(const JournalContext& context, const fs::path& archive,
                               std::string_view digest, std::string_view current_version,
                               std::string_view release_version, std::uint32_t parent_pid,
                               std::int64_t creation_ticks, const CancelToken& cancel,
                               const PrepareHook& hook) {
    const auto reached = [&hook](std::string_view point) {
        if (hook)
            hook(point);
    };
    check_cancel(cancel);
    const auto id = uuid4_hex();
    const auto paths = journal_paths(context, id);
    if (!context.expected_registration ||
        context.expected_registration->hive != update::RegistryHive::CurrentUser ||
        !update::is_newer_version(current_version, release_version))
        throw RequiresSetup("This installation needs the setup package to update.");
    GuardedDirectory parent(paths.install_dir.parent_path());
    GuardedDirectory install(paths.install_dir);
    GuardedDirectory updates(paths.updates_root);
    if (!archive.is_absolute() || !fs::equivalent(archive.parent_path(), paths.updates_root))
        throw UpdateError("The update archive is outside the update cache.");
    auto input = updates.pin_file(archive.filename());
    if (!input || download::sha256_file(archive, true) != digest)
        throw UpdateError("The update archive failed its release integrity check.");
    if (fs::exists(fs::symlink_status(paths.candidate_dir)) ||
        fs::exists(fs::symlink_status(paths.rollback_dir)))
        throw UpdateError("A previous update tree is still on disk.");

    auto old_inventory = install.pin_file(update::manifest_name());
    if (!old_inventory)
        throw RequiresSetup(
            "The previous release inventory is unavailable. Use the setup package.");
    const auto installed = installed_manifest(install, current_version);
    const auto& app = update::release_identity().app_exe;
    const auto image = install.pin_file(app);
    if (!image || download::sha256_file(paths.install_dir / app, true) != installed.at(app).sha256)
        throw RequiresSetup("The installed application changed. Use the setup package.");
    const auto uninstaller =
        uninstall_command(context.expected_registration->uninstall_string, paths);
    const auto manifest = verify_archive(archive, release_version, cancel);
    const auto release = validate_manifest(manifest, release_version, current_version);
    check_preparation_space(paths.install_dir, tree_size_bytes(paths.install_dir),
                            manifest.at("unpacked_bytes").get<std::uint64_t>(), paths.updates_root,
                            fs::file_size(archive));
    auto journal = make_journal(context, id);
    journal.old_manifest_sha256 =
        download::sha256_file(paths.install_dir / update::manifest_name(), true);
    journal.old_version = current_version;
    journal.new_version = release_version;
    journal.old_display_version = journal.registration.display_version;
    journal.old_estimated_size_kb = journal.registration.estimated_size_kb;
    journal.old_install_date = journal.registration.install_date;
    journal.old_exe_sha256 = installed.at(app).sha256;
    journal.new_exe_sha256 = release.at(app).sha256;
    journal.archive_path = archive;
    journal.parent_pid = parent_pid;
    journal.parent_creation_time = creation_ticks;
    if (parent_pid)
        journal.parent_exe = context.parent_exe;
    (void)validate_journal_binding(context, journal, id);
    fs::create_directory(paths.transaction_dir.parent_path());
    bool journaled = false;
    try {
        // These guards are released before the handler below runs, so it can remove what an
        // unfinished preparation wrote.
        GuardedDirectory transactions(paths.transaction_dir.parent_path());
        if (!fs::create_directory(paths.transaction_dir))
            throw UpdateError("Could not create an update transaction.");
        GuardedDirectory transaction(paths.transaction_dir);
        save_journal(context, journal);
        journaled = true;
        transaction.copy_file_atomic_from(*old_inventory, path_from_utf8(previous_inventory));
        if (extract_archive(archive, paths.candidate_dir, release_version, cancel) != manifest)
            throw UpdateError("The extracted update manifest changed.");
        reached("extracted");
        // Hashing and copying follow the extraction, so a cancel arriving here has to be
        // honored: otherwise the app hands off and restarts into a version the user declined.
        check_cancel(cancel);
        GuardedDirectory candidate(paths.candidate_dir);
        for (const auto* suffix : {".exe", ".dat", ".msg"}) {
            check_cancel(cancel);
            const auto leaf = fs::path(uninstaller).replace_extension(suffix);
            auto pin = install.pin_file(leaf);
            if (!pin) {
                if (std::string_view(suffix) == ".exe")
                    throw RequiresSetup("The registered uninstaller could not be preserved.");
                continue;
            }
            const auto name = path_to_utf8(leaf);
            const auto source = paths.install_dir / leaf;
            Json record = {{"size", fs::file_size(source)},
                           {"sha256", download::sha256_file(source, true)}};
            candidate.copy_file_atomic_from(*pin, leaf);
            // Recorded only once it is in the candidate, so the journal of an interrupted
            // preparation lists exactly what the candidate holds.
            journal.uninstaller_files.push_back(name);
            journal.compatibility_files[name] = std::move(record);
        }
        reached("preserved");
        stage_helper(install, transaction, paths, installed, cancel);
        reached("staged");
        check_cancel(cancel);
        candidate.write_file_atomic(update::sentinel_name(), release_version);
        journal.state = TransactionState::Prepared;
        check_cancel(cancel);
        save_journal(context, journal);
    } catch (...) {
        if (!journaled)
            throw;
        // Interrupted preparation has never displaced the running installation. OpenWhisper ends
        // it rolled_back; superseded is the native design's name for a transaction that never
        // reached the commit (recovery of an unstarted one ends there too), and it makes no
        // difference to what is reported or collected.
        journal.state = TransactionState::Superseded;
        save_journal(context, journal);
        // The pin keeps the archive from being deleted on Windows.
        input.reset();
        collect_unstarted(context, journal, digest);
        throw;
    }
    return {paths.transaction_dir / updater_name,
            {"--apply", std::string(update::kTransactionArg), id},
            id};
}
} // namespace fv::update_native
