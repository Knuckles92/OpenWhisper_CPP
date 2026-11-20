#include "update_native/transaction_storage.h"
#include <array>
#include <stdexcept>
#include <vector>

#include "base/identity.h"
#include "base/ids.h"
#include "base/json.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "sql/db.h"
#include "update_native/data_snapshot.h"

namespace fv::update_native {
TransactionStorage::TransactionStorage(const JournalContext& context, std::string_view id)
    : paths_(journal_paths(context, id)), install_parent_(paths_.install_dir.parent_path()),
      transaction_directory_(paths_.transaction_dir) {}

void TransactionStorage::acquire_data_lease() {
    if (lease_.held())
        throw std::logic_error("The update data lease is already held.");
    lease_ = DataLease::acquire(paths_.data_root, identity(), LeaseMode::Exclusive);
}
void TransactionStorage::release_data_lease() {
    lease_.release();
}
bool TransactionStorage::has_data_lease() const {
    return lease_.held();
}
void TransactionStorage::snapshot_with_pinned_sources() {
    if (!has_data_lease())
        throw std::logic_error("Snapshot requires the exclusive data lease.");
    GuardedDirectory data(paths_.data_root);
    const auto& app = identity();
    const std::array names{std::string(app.settings_file), std::string(app.database_file)};
    std::vector<std::unique_ptr<GuardedDirectory::FilePin>> pins;
    for (const auto& name :
         {names[0], names[1], names[1] + "-wal", names[1] + "-shm", names[1] + "-journal"}) {
        if (auto pin = data.pin_file(name))
            pins.push_back(std::move(pin));
    }
    const auto scratch_path = paths_.transaction_dir / (".snapshot-" + uuid4_hex());
    if (!std::filesystem::create_directory(scratch_path))
        throw std::runtime_error("Could not reserve snapshot staging.");
    struct Scratch {
        std::filesystem::path path;
        ~Scratch() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } scratch_cleanup{scratch_path};
    GuardedDirectory scratch(scratch_path);
    const auto destination = paths_.transaction_dir / "data";
    if (!std::filesystem::create_directory(destination))
        throw std::runtime_error("An update data snapshot already exists.");
    GuardedDirectory snapshot(destination);
    Json manifest = Json::object();
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto source = data.pin_file(names[i]);
        if (!source) {
            manifest[names[i]] = nullptr;
            continue;
        }
        if (i == 1) {
            // SQLite needs write sharing on its output. A read handle without delete sharing
            // pins the pre-created ordinary file until SQLite has closed it.
            scratch.write_file_atomic(names[i], "");
            auto output = scratch.pin_file(names[i], true);
            {
                auto live =
                    sql::Connection::open(paths_.data_root / names[i], sql::OpenMode::ReadOnly);
                auto copy = sql::Connection::open(scratch_path / names[i], sql::OpenMode::Create);
                live.backup_to(copy, {});
            }
            auto completed = scratch.pin_file(names[i]);
            if (!completed)
                throw std::runtime_error("The snapshot database disappeared.");
            output.reset();
            snapshot.copy_file_atomic_from(*completed, names[i]);
        } else {
            snapshot.copy_file_atomic_from(*source, names[i]);
        }
        auto copied = snapshot.pin_file(names[i]);
        if (!copied)
            throw std::runtime_error("The update snapshot file disappeared.");
        manifest[names[i]] = download::sha256_file(destination / names[i], true);
    }
    snapshot.write_file_atomic("data-snapshot.json", manifest.dump(2) + "\n");
}
void TransactionStorage::restore_with_pinned_sources() {
    if (!has_data_lease())
        throw std::logic_error("Restore requires the exclusive data lease.");
    GuardedDirectory snapshot(paths_.transaction_dir / "data");
    GuardedDirectory data(paths_.data_root);
    const auto& app = identity();
    const std::array names{std::string(app.settings_file), std::string(app.database_file)};
    const auto manifest = Json::parse(snapshot.read_file("data-snapshot.json", 4096));
    if (!manifest.is_object() || manifest.size() != names.size())
        throw std::runtime_error("The update data snapshot is invalid.");
    std::array<std::unique_ptr<GuardedDirectory::FilePin>, 2> pins;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!manifest.contains(names[i]))
            throw std::runtime_error("The update data snapshot is invalid.");
        const auto& hash = manifest.at(names[i]);
        if (hash.is_null())
            continue;
        if (!hash.is_string() || !(pins[i] = snapshot.pin_file(names[i])) ||
            download::sha256_file(paths_.transaction_dir / "data" / names[i], true) !=
                hash.get<std::string>())
            throw std::runtime_error("The update data snapshot failed verification.");
    }
    // The exclusive data lease closes the application database. These are deleted by handle,
    // and the verified snapshot files are copied directly from their pinned handles.
    for (const auto suffix : {"-wal", "-shm", "-journal"})
        data.remove_file(std::string(app.database_file) + suffix);
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (pins[i])
            data.copy_file_atomic_from(*pins[i], names[i]);
        else
            data.remove_file(names[i]);
    }
}
} // namespace fv::update_native
