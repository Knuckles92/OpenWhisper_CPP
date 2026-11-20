#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "remote/control.h"
#include "remote_records/gate.h"

namespace fv::remote_records {

class RecordNotFound : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string safe_name(const Json& name);
std::string safe_record_id(const Json& id);
std::string safe_kind(const Json& kind);

using Files = std::map<std::string, std::filesystem::path, std::less<>>;

class RecordKind {
public:
    virtual ~RecordKind() = default;
    virtual Json import_record(std::string_view id, const std::filesystem::path& files,
                               const Json& record, const remote::PairedDevice& device) = 0;
    virtual Json list(std::string_view device, std::string_view query, int limit) = 0;
    virtual Files export_record(std::string_view device, std::string_view id,
                                const std::filesystem::path& dest) = 0;
    virtual Json stored_files(std::string_view device, std::string_view id) = 0;
    virtual bool remove_version(std::string_view, std::string_view, std::string_view) {
        throw std::runtime_error("This host cannot verify the selected record copy.");
    }
    virtual bool remove(std::string_view device, std::string_view id) = 0;
    virtual int delete_all(std::string_view device) = 0;
    virtual int rename_owner(std::string_view device, std::string_view name) = 0;
    virtual Json summary(std::string_view device) = 0;
    virtual std::vector<remote::RecordOwnerGroup> owners() = 0;
    virtual bool owns(std::string_view device, std::string_view id) = 0;
};

// A staging and export area for records accepted over authenticated remote speech connections.
// Device IDs and owner grants come from the host's registry. Only RecordKind adapters publish
// records, keeping the transfer independent of the host database.
class HostRecordStore {
public:
    using Kinds = std::map<std::string, std::shared_ptr<RecordKind>, std::less<>>;
    using SpaceProbe = std::function<std::uint64_t(const std::filesystem::path&)>;

    HostRecordStore(std::filesystem::path root, Kinds kinds, Clock& clock, SpaceProbe space = {});

    Json handle(std::string_view op, const Json& header, std::span<const std::byte> payload,
                const remote::PairedDevice& device);
    int delete_device(std::string_view device);
    int rename_device(std::string_view device, std::string_view name);
    Json summary(const remote::PairedDevice& device);
    std::vector<remote::RecordOwnerGroup> owners();
    RecordGate& gate() { return gate_; }

private:
    std::filesystem::path path(std::string_view area, std::string_view device,
                               std::string_view kind = {}, std::string_view id = {}) const;
    Json begin(const std::filesystem::path& dir, const Json& header);
    Json put(const std::filesystem::path& dir, const Json& header, std::span<const std::byte>);
    Json commit(const std::filesystem::path& dir, std::string_view kind, std::string_view id,
                RecordKind& adapter, const remote::PairedDevice& owner);
    Json open(const std::filesystem::path& dir, std::string_view owner, std::string_view id,
              RecordKind& adapter);
    Json fetch(const std::filesystem::path& dir, const Json& header);
    void prune(std::string_view area, std::chrono::seconds ttl,
               const std::filesystem::path& keep = {});

    std::filesystem::path root_;
    Kinds kinds_;
    Clock& clock_;
    SpaceProbe space_;
    RecordGate gate_;
    std::mutex mutex_;
};

} // namespace fv::remote_records
