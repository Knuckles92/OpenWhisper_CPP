#include "update_native/terminal_cleanup.h"
#include "base/hash.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "pyport/text.h"
#include "update/update_contract.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"
#include "update_native/pinned_tree.h"
#include <algorithm>
#include <set>
#include <vector>

namespace fv::update_native {
namespace fs = std::filesystem;
namespace {
// The completion marker is checked when it is there and never required: a preparation that was
// interrupted after its extraction has none, and its files are this release's all the same.
bool remove_verified_tree(const fs::path& root, std::string_view version,
                          std::string_view image_hash, const Json& compatibility,
                          std::string_view manifest_hash = {}) {
    const bool old = !manifest_hash.empty();
    GuardedDirectory parent(root.parent_path());
    std::error_code error;
    if (!fs::exists(fs::symlink_status(root, error))) {
        if (error && error != std::errc::no_such_file_or_directory)
            throw fs::filesystem_error("Inspecting update candidate", root, error);
        return false;
    }
    FileTable records;
    {
        PinnedTree tree(root);
        tree.verify_release(version, image_hash, compatibility, false);
        GuardedDirectory candidate(root);
        const auto bytes =
            candidate.read_file(path_from_utf8(update::manifest_name()), max_manifest_bytes);
        const auto hash = sha256_hex(bytes);
        if (old && hash != manifest_hash)
            throw UpdateError("The previous installation inventory has changed.");
        records = validate_manifest(Json::parse(bytes), version);
        records.emplace(update::manifest_name(), FileRecord{bytes.size(), hash});
        if (fs::exists(fs::symlink_status(root / update::sentinel_name())))
            records.emplace(update::sentinel_name(),
                            FileRecord{version.size(), sha256_hex(version)});
    }
    std::set<fs::path> directories;
    auto remove_leaf = [&](const fs::path& relative, std::string_view hash) {
        const auto directory = relative.parent_path();
        GuardedDirectory guard(directory.empty() ? root : root / directory);
        guard.remove_file(relative.filename(), hash);
        for (auto current = directory; !current.empty(); current = current.parent_path())
            directories.insert(current);
    };
    for (const auto& [name, record] : records)
        remove_leaf(path_from_utf8(name), record.sha256);
    for (auto it = compatibility.begin(); it != compatibility.end(); ++it)
        remove_leaf(path_from_utf8(it.key()), it.value().at("sha256").get<std::string>());
    std::vector<fs::path> ordered(directories.begin(), directories.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
    });
    for (const auto& directory : ordered) {
        GuardedDirectory guard(directory.parent_path().empty() ? root
                                                               : root / directory.parent_path());
        guard.remove_empty_directory(directory.filename());
    }
    parent.remove_empty_directory(root.filename());
    return true;
}
} // namespace
bool cleanup_terminal_candidate(const JournalContext& context, const UpdateJournal& journal) {
    if (journal.state != TransactionState::Superseded &&
        journal.state != TransactionState::RolledBack)
        throw UpdateError("Only a terminal update candidate can be cleaned.");
    const auto paths = validate_journal_binding(context, journal, journal.transaction_id);
    bool removed = remove_verified_tree(paths.candidate_dir, journal.new_version,
                                        journal.new_exe_sha256, journal.compatibility_files);
    if (journal.state == TransactionState::RolledBack) {
        // A new tree that did not become healthy was moved aside before the old one returned.
        // An intact one holds only this release's files; a damaged one is left for inspection
        // and does not keep the rest of the transaction from being collected.
        try {
            removed = remove_verified_tree(paths.failed_dir, journal.new_version,
                                           journal.new_exe_sha256, journal.compatibility_files) ||
                      removed;
        } catch (const std::exception&) {
        }
    }
    return removed;
}
bool cleanup_healthy_rollback(const JournalContext& context, const UpdateJournal& journal) {
    if (journal.state != TransactionState::Healthy || !journal.cleanup_rollback ||
        journal.old_manifest_sha256.empty())
        return false;
    const auto paths = validate_journal_binding(context, journal, journal.transaction_id);
    if (!fs::exists(fs::symlink_status(paths.rollback_dir)))
        return false;
    PinnedTree active(paths.install_dir);
    active.verify_release(journal.new_version, journal.new_exe_sha256, journal.compatibility_files);
    return remove_verified_tree(paths.rollback_dir, journal.old_version, journal.old_exe_sha256,
                                journal.compatibility_files, journal.old_manifest_sha256);
}
void remove_transaction_archive(const JournalContext& context, const UpdateJournal& journal,
                                std::string_view archive_sha256) {
    const auto paths = validate_journal_binding(context, journal, journal.transaction_id);
    if (!paths.archive_file.empty())
        GuardedDirectory(paths.updates_root)
            .remove_file(paths.archive_file.filename(), archive_sha256);
}
bool cleanup_finished_transaction(const JournalContext& context, const UpdateJournal& journal) {
    if (journal.state != TransactionState::Healthy &&
        journal.state != TransactionState::Superseded &&
        journal.state != TransactionState::RolledBack)
        return false;
    const auto paths = validate_journal_binding(context, journal, journal.transaction_id);
    if (journal.old_manifest_sha256.empty() ||
        fs::exists(fs::symlink_status(paths.candidate_dir)) ||
        fs::exists(fs::symlink_status(paths.rollback_dir)))
        return false;
    const auto directory = paths.transaction_dir;
    {
        GuardedDirectory tx(directory);
        const auto manifest = tx.read_file(path_from_utf8(previous_inventory), max_manifest_bytes);
        if (sha256_hex(manifest) != journal.old_manifest_sha256)
            throw UpdateError("The transaction inventory has changed.");
        const auto previous = validate_manifest(Json::parse(manifest), journal.old_version);
        const auto journal_bytes = tx.read_file(paths.journal_file.filename(), max_manifest_bytes);
        if (to_json(journal_from_json(Json::parse(journal_bytes))) != to_json(journal))
            throw UpdateError("The transaction changed during cleanup.");
        std::map<std::string, std::string> files;
        for (const auto& [name, record] : previous)
            if (name == updater_name ||
                (name.find('/') == std::string::npos && py::lower(name).ends_with(".dll")))
                files.emplace(name, record.sha256);
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto leaf = path_to_utf8(entry.path().filename());
            if (!files.contains(leaf) && leaf != "data" && leaf != previous_inventory &&
                leaf != update::kJournalName && leaf != update::kHealthName)
                return false;
        }
        // The updater can still be executing. Its image is the first file we attempt to remove.
        tx.remove_file(path_from_utf8(updater_name), files.at(std::string(updater_name)));
        for (const auto& [name, hash] : files)
            if (name != updater_name)
                tx.remove_file(path_from_utf8(name), hash);
        tx.remove_file(path_from_utf8(update::kHealthName), sha256_hex(journal.health_token));
        if (fs::exists(fs::symlink_status(directory / "data"))) {
            {
                GuardedDirectory snapshot(directory / "data");
                if (!fs::is_empty(directory / "data")) {
                    const auto bytes = snapshot.read_file("data-snapshot.json", 4096);
                    const auto entries = Json::parse(bytes);
                    const std::array names{std::string(identity().settings_file),
                                           std::string(identity().database_file)};
                    if (!entries.is_object() || entries.size() != names.size())
                        return false;
                    for (const auto& entry : fs::directory_iterator(directory / "data")) {
                        const auto name = path_to_utf8(entry.path().filename());
                        if (name != "data-snapshot.json" && name != names[0] && name != names[1])
                            return false;
                    }
                    for (const auto& name : names) {
                        const auto& hash = entries.at(name);
                        if (hash.is_null()) {
                            if (fs::exists(
                                    fs::symlink_status(directory / "data" / path_from_utf8(name))))
                                return false;
                        } else {
                            const auto digest = hash.get<std::string>();
                            if (digest.size() != 64)
                                return false;
                            snapshot.remove_file(path_from_utf8(name), digest);
                        }
                    }
                    snapshot.remove_file("data-snapshot.json", sha256_hex(bytes));
                }
            }
            tx.remove_empty_directory("data");
        }
        tx.remove_file(path_from_utf8(previous_inventory), journal.old_manifest_sha256);
        tx.remove_file(paths.journal_file.filename(), sha256_hex(journal_bytes));
    }
    GuardedDirectory parent(directory.parent_path());
    parent.remove_empty_directory(directory.filename());
    return true;
}

} // namespace fv::update_native
