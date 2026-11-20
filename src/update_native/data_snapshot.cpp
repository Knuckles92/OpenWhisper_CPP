#include "update_native/data_snapshot.h"

#include <array>
#include <fstream>
#include <optional>

#include "base/atomic_file.h"
#include "base/identity.h"
#include "base/ids.h"
#include "base/json.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "sql/db.h"
#include "update/update_contract.h"
#include "update_native/detail/file_ops.h"
#include "update_native/manifest.h"

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;
constexpr auto marker = "data-snapshot.json";

auto names() {
    return std::array{identity().settings_file, identity().database_file};
}

void check_chain(const fs::path& path) {
    for (fs::path p = fs::absolute(path).lexically_normal();; p = p.parent_path()) {
        if (fs::exists(fs::symlink_status(p)) && detail::unsafe_node(p)) {
            throw UpdateError("An update snapshot path is a link: " + path_to_utf8(p));
        }
        if (p == p.root_path()) {
            break;
        }
    }
}

bool regular_or_absent(const fs::path& path) {
    check_chain(path);
    const auto status = fs::symlink_status(path);
    if (!fs::exists(status)) {
        return false;
    }
    if (!fs::is_regular_file(status) || fs::hard_link_count(path) != 1) {
        throw UpdateError("Expected an ordinary update snapshot file: " + path_to_utf8(path));
    }
    return true;
}

fs::path transaction_dir(const fs::path& root, std::string_view id) {
    const auto result = fs::absolute(root) / update::kUpdatesDirname /
                        update::kTransactionsDirname / update::validate_transaction_id(id);
    check_chain(result);
    return result;
}

class Scratch {
public:
    explicit Scratch(const fs::path& parent) : path(parent / (".snapshot-" + uuid4_hex())) {
        if (!fs::create_directory(path)) {
            throw UpdateError("Could not reserve space for an update snapshot.");
        }
    }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};
} // namespace

void snapshot_data(const fs::path& data_root, std::string_view id) {
    const auto tx = transaction_dir(data_root, id);
    fs::create_directories(tx);
    Scratch staging(tx);
    Json manifest = Json::object();
    for (const auto name : names()) {
        const auto source = data_root / name;
        const auto target = staging.path / name;
        if (!regular_or_absent(source)) {
            manifest[name] = nullptr;
            continue;
        }
        if (name == identity().database_file) {
            // SQLite's backup includes committed WAL pages even while a reader holds the WAL.
            for (auto suffix : {"-wal", "-shm", "-journal"}) {
                regular_or_absent(data_root / (std::string(name) + suffix));
            }
            auto live = sql::Connection::open(source, sql::OpenMode::ReadOnly);
            auto copy = sql::Connection::open(target, sql::OpenMode::Create);
            live.backup_to(copy, {});
        } else {
            fs::copy_file(source, target);
        }
        detail::flush_file(target);
        manifest[name] = download::sha256_file(target, true);
    }
    write_file_atomic(staging.path / marker, manifest.dump(2) + "\n");
    detail::flush_directory(staging.path);
    detail::publish_new(staging.path, tx / "data");
}

void restore_data(const fs::path& data_root, std::string_view id) {
    const auto snapshot = transaction_dir(data_root, id) / "data";
    const auto index = snapshot / marker;
    if (!regular_or_absent(index)) {
        return;
    }
    if (fs::file_size(index) > 4096) {
        throw UpdateError("The update data snapshot is invalid.");
    }
    std::ifstream input(index, std::ios::binary);
    const Json manifest = Json::parse(input);
    if (!manifest.is_object() || manifest.size() != names().size()) {
        throw UpdateError("The update data snapshot is invalid.");
    }
    // Copy and verify both files, and check every destination, before changing user data.
    // A crash during a later replace can safely replay from the immutable snapshot.
    Scratch verified(data_root);
    for (const auto name : names()) {
        if (!manifest.contains(name)) {
            throw UpdateError("The update data snapshot is invalid.");
        }
        const auto& expected = manifest.at(name);
        regular_or_absent(data_root / name);
        if (expected.is_null()) {
            continue;
        }
        const auto source = snapshot / name;
        const auto staged = verified.path / name;
        if (!expected.is_string() || !regular_or_absent(source)) {
            throw UpdateError("The update data snapshot failed verification.");
        }
        fs::copy_file(source, staged);
        if (download::sha256_file(staged, true) != expected.get<std::string>()) {
            throw UpdateError("The update data snapshot failed verification.");
        }
        detail::flush_file(staged);
    }
    const std::string db(identity().database_file);
    for (auto suffix : {"-wal", "-shm", "-journal"}) {
        regular_or_absent(data_root / (db + suffix));
    }
    for (auto suffix : {"-wal", "-shm", "-journal"}) {
        fs::remove(data_root / (db + suffix));
    }
    for (const auto name : names()) {
        if (manifest.at(name).is_null()) {
            fs::remove(data_root / name);
        } else {
            detail::replace_file(verified.path / name, data_root / name);
        }
    }
    detail::flush_directory(fs::absolute(data_root));
}

} // namespace fv::update_native
