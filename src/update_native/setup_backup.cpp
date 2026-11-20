#include "update_native/setup_backup.h"

#include "base/utf8.h"
#include "download/verified_files.h"
#include "pyport/text.h"
#include "update/release_identity.h"
#include "update/update_contract.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"
#include <algorithm>
#include <map>
#include <set>

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;
constexpr auto marker = "setup.json";

bool managed(std::string_view name) {
    const auto root = py::lower(name.substr(0, name.find('/')));
    if (root == "share" || (root.starts_with("openwhisper-cpp") && root.ends_with(".exe")))
        return true;
    try {
        check_topology(name, false);
        return true;
    } catch (const RequiresSetup&) {
        return false;
    }
}

fs::path backup_path(const fs::path& install) {
    if (!install.is_absolute() || install != install.lexically_normal() ||
        install.filename().empty() || install == install.root_path())
        throw UpdateError("Setup requires an absolute installation directory.");
    return install.parent_path() / (install.filename().wstring() + L".setup-backup");
}

void ensure_directory(const fs::path& path) {
    if (!fs::exists(fs::symlink_status(path))) {
        ensure_directory(path.parent_path());
        GuardedDirectory parent(path.parent_path());
        fs::create_directory(path);
    }
    GuardedDirectory checked(path);
}

std::string relative_name(const fs::path& path, const fs::path& root) {
    auto name = path_to_utf8(path.lexically_relative(root));
    std::replace(name.begin(), name.end(), '\\', '/');
    return member_name(name);
}

Json read_stage(const fs::path& install, const GuardedDirectory& backup) {
    const auto state = Json::parse(backup.read_file(marker, max_manifest_bytes));
    if (!state.is_object() || state.value("app_id", "") != update::release_identity().app_id ||
        state.value("app_dir", "") != path_to_utf8(install) || !state.contains("files") ||
        !state.at("files").is_object() || state.at("files").size() > max_members ||
        !state.contains("incoming") || !state.at("incoming").is_array() ||
        state.at("incoming").size() > max_members + 1)
        throw UpdateError("The setup backup does not belong to this installation.");
    for (const auto& [name, hash] : state.at("files").items()) {
        if (!managed(member_name(name)) || !hash.is_string())
            throw UpdateError("Invalid setup backup entry.");
        const auto& text = hash.get_ref<const std::string&>();
        if (text.size() != 64 || text.find_first_not_of("0123456789abcdef") != text.npos)
            throw UpdateError("Invalid setup backup digest.");
    }
    for (const auto& entry : state.at("incoming"))
        if (!entry.is_string() || !managed(member_name(entry.get<std::string>())))
            throw UpdateError("Invalid setup recovery path.");
    return state;
}

void discard(const fs::path& install, const Json& state) {
    const auto path = backup_path(install);
    {
        GuardedDirectory backup(path);
        std::set<std::string> expected{marker};
        for (const auto& hash : state.at("files"))
            expected.insert(hash.get<std::string>());
        for (const auto& entry : fs::directory_iterator(path))
            if (!expected.contains(path_to_utf8(entry.path().filename())))
                throw UpdateError("The setup backup contains an unrelated file.");
        for (const auto& leaf : expected)
            if (leaf != marker)
                backup.remove_file(path_from_utf8(leaf));
        backup.remove_file(marker);
    }
    GuardedDirectory parent(install.parent_path());
    parent.remove_empty_directory(path.filename());
}

void restore(const fs::path& install, const fs::path& path, const Json& stage) {
    GuardedDirectory backup(path);
    ensure_directory(install);
    GuardedDirectory target(install);
    std::map<std::string, std::unique_ptr<GuardedDirectory::FilePin>> objects;
    for (const auto& value : stage.at("files")) {
        const auto hash = value.get<std::string>();
        if (objects.contains(hash))
            continue;
        auto pinned = backup.pin_file(path_from_utf8(hash));
        if (!pinned || download::sha256_file(path / path_from_utf8(hash), true) != hash)
            throw UpdateError("The setup rollback copy failed verification.");
        objects.emplace(hash, std::move(pinned));
    }
    for (const auto& value : stage.at("incoming")) {
        const auto name = value.get<std::string>();
        const auto entry = install / path_from_utf8(name);
        if (!stage.at("files").contains(name) && fs::exists(fs::symlink_status(entry))) {
            GuardedDirectory directory(entry.parent_path());
            directory.remove_file(entry.filename());
        }
    }
    for (const auto& [name, hash] : stage.at("files").items()) {
        const auto entry = install / path_from_utf8(name);
        ensure_directory(entry.parent_path());
        GuardedDirectory directory(entry.parent_path());
        directory.copy_file_atomic_from(*objects.at(hash.get<std::string>()), entry.filename());
    }
}

void restore_first_install(const fs::path& install, const Json& stage, const Json& incoming) {
    const auto records = validate_manifest(incoming);
    if (!stage.at("files").empty())
        throw UpdateError("This folder contains a backup of a different installation.");
    for (const auto& entry : stage.at("incoming")) {
        const auto name = entry.get<std::string>();
        if (name != update::manifest_name() && !records.contains(name))
            throw UpdateError(
                "Use the original setup package to finish this interrupted installation.");
    }
    for (const auto& entry : stage.at("incoming")) {
        const auto name = entry.get<std::string>();
        const auto file = install / path_from_utf8(name);
        if (!fs::exists(fs::symlink_status(file)))
            continue;
        GuardedDirectory parent(file.parent_path());
        if (name == update::manifest_name()) {
            std::string hash;
            {
                auto pin = parent.pin_file(file.filename());
                if (!pin ||
                    Json::parse(parent.read_file(file.filename(), max_manifest_bytes)) != incoming)
                    throw UpdateError(
                        "The installed manifest belongs to a different setup package.");
                hash = download::sha256_file(file, true);
            }
            parent.remove_file(file.filename(), hash);
        } else {
            parent.remove_file(file.filename(), records.at(name).sha256);
        }
    }
}

void finish_previous(const fs::path& install, const Json& incoming, bool registered) {
    const auto path = backup_path(install);
    if (!fs::exists(fs::symlink_status(path)))
        return;
    Json stage;
    {
        GuardedDirectory backup(path);
        stage = read_stage(install, backup);
        const auto phase = stage.value("state", "");
        if (!registered && !stage.at("files").empty())
            throw UpdateError(
                "A previous installation must be registered before setup can restore it.");
        if (phase == "prepared") {
            if (registered)
                restore(install, path, stage);
            else
                restore_first_install(install, stage, incoming);
            stage["state"] = "restored";
            backup.write_file_atomic(marker, stage.dump());
        } else if (phase != "copying" && phase != "installed" && phase != "restored") {
            throw UpdateError("The setup backup state is invalid.");
        }
    }
    discard(install, stage);
}
} // namespace

void prepare_setup(const fs::path& install, const Json& incoming, bool registered) {
    const auto records = validate_manifest(incoming);
    const auto path = backup_path(install);
    ensure_directory(install);
    GuardedDirectory target(install);
    finish_previous(install, incoming, registered);
    Json names = Json::array({update::manifest_name()});
    for (const auto& entry : records)
        names.push_back(entry.first);
    Json files = Json::object();
    std::uintmax_t bytes = 0;
    for (auto it = fs::recursive_directory_iterator(install);
         it != fs::recursive_directory_iterator(); ++it) {
        const auto name = relative_name(it->path(), install);
        const auto status = it->symlink_status();
        const bool owned = managed(name) || (fs::is_directory(status) && managed(name + "/child"));
        if (!owned) {
            it.disable_recursion_pending();
            continue;
        }
        if (fs::is_directory(status)) {
            GuardedDirectory checked(it->path());
            continue;
        }
        if (!registered)
            throw UpdateError(
                "The selected folder already contains application files. Select an empty folder.");
        GuardedDirectory directory(it->path().parent_path());
        auto pinned = directory.pin_file(it->path().filename());
        if (!pinned || files.size() >= max_members ||
            fs::file_size(it->path()) > max_unpacked_bytes - bytes)
            throw UpdateError("The previous installation exceeds the setup backup limits.");
        bytes += fs::file_size(it->path());
        files[name] = download::sha256_file(it->path(), true);
    }
    if (fs::space(install.parent_path()).available <
        bytes + incoming.at("unpacked_bytes").get<std::uint64_t>())
        throw UpdateError("There is insufficient free space for setup and its rollback copy.");
    Json stage = {{"app_id", update::release_identity().app_id},
                  {"app_dir", path_to_utf8(install)},
                  {"state", "copying"},
                  {"files", std::move(files)},
                  {"incoming", std::move(names)}};
    if (stage.dump().size() > max_manifest_bytes)
        throw UpdateError("The setup backup inventory is too large.");
    if (!fs::create_directory(path))
        throw UpdateError("The setup backup directory already exists.");
    GuardedDirectory backup(path);
    backup.write_file_atomic(marker, stage.dump());
    for (const auto& [name, value] : stage.at("files").items()) {
        const auto entry = install / path_from_utf8(name);
        GuardedDirectory directory(entry.parent_path());
        auto pinned = directory.pin_file(entry.filename());
        const auto hash = value.get<std::string>();
        if (!pinned || download::sha256_file(entry, true) != hash)
            throw UpdateError("The installed files changed during setup preparation.");
        backup.copy_file_atomic_from(*pinned, path_from_utf8(hash));
    }
    stage["state"] = "prepared";
    backup.write_file_atomic(marker, stage.dump());
}

void finish_setup(const fs::path& install, const Json& incoming) {
    const auto path = backup_path(install);
    Json stage;
    {
        GuardedDirectory backup(path);
        stage = read_stage(install, backup);
        if (stage.value("state", "") != "prepared")
            throw UpdateError("Setup did not finish preparing its rollback copy.");
        GuardedDirectory target(install);
        if (Json::parse(target.read_file(update::manifest_name(), max_manifest_bytes)) != incoming)
            throw UpdateError("The installed manifest does not match this setup.");
        for (const auto& [name, record] : validate_manifest(incoming)) {
            const auto file = install / path_from_utf8(name);
            GuardedDirectory directory(file.parent_path());
            auto pinned = directory.pin_file(file.filename());
            if (!pinned || fs::file_size(file) != record.size ||
                download::sha256_file(file, true) != record.sha256)
                throw UpdateError("An installed file failed verification: " + name);
        }
        stage["state"] = "installed";
        backup.write_file_atomic(marker, stage.dump());
    }
    // Installation is committed. A lock on its backup can be retried at the next setup.
    try {
        discard(install, stage);
    } catch (const std::exception&) {
    }
}

void rollback_setup(const fs::path& install, const Json& incoming, bool registered) {
    finish_previous(install, incoming, registered);
}
} // namespace fv::update_native
