#include "remote_records/host_store.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <fstream>
#include <set>
#include <utility>

#include "base/atomic_file.h"
#include "base/base64.h"
#include "base/executor.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "pyport/errors.h"
#include "remote/wire.h"

namespace fv::remote_records {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::uint64_t kMaxRecord = 64ULL << 30;
constexpr std::uint64_t kMaxManifest = 256ULL << 20;
constexpr std::uint64_t kReserve = 1ULL << 30;
constexpr std::uint64_t kMaxFiles = 50'000;
constexpr std::string_view kManifest = "record.json";

const Json& value(const Json& object, std::string_view key) {
    static const Json none;
    const auto at = object.find(std::string(key));
    return at == object.end() ? none : *at;
}

std::string str(const Json& item) {
    return item.is_string() ? item.get<std::string>() : std::string();
}

bool ascii_alnum(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

std::string fold(std::string name) {
    for (auto& ch : name) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return name;
}

bool ordinary_segment(std::string_view part) {
    if (part.empty() || part.size() > 120 || !ascii_alnum(part.front()) || part.back() == '.' ||
        !std::ranges::all_of(
            part, [](char ch) { return ascii_alnum(ch) || ch == '.' || ch == '_' || ch == '-'; })) {
        return false;
    }
    // Names that Windows treats as devices or aliases are never files in a portable record.
    const auto stem = fold(std::string(part.substr(0, part.find('.'))));
    return stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" &&
           !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
             stem.back() >= '0' && stem.back() <= '9');
}

void require_owner(std::string_view id) {
    if (id.empty() || id.size() > 64 || !std::ranges::all_of(id, [](char ch) {
            return ascii_alnum(ch) || ch == '_' || ch == '-';
        })) {
        throw py::ValueError("Invalid record owner.");
    }
}

fs::path beneath(const fs::path& root, const fs::path& relative) {
    const auto base = fs::weakly_canonical(fs::absolute(root));
    const auto expected = (base / relative).lexically_normal();
    const auto target = fs::weakly_canonical(expected);
    const auto tail = target.lexically_relative(base);
    if (target != expected || tail.empty() || tail == "." || tail.is_absolute() ||
        *tail.begin() == "..") {
        throw py::ValueError("Invalid location for a record file.");
    }
    return target;
}

std::uint64_t integer(const Json& item, std::uint64_t min, std::uint64_t max,
                      std::string_view label) {
    if (item.is_number_integer() && (item.is_number_unsigned() || item.get<std::int64_t>() >= 0)) {
        const auto number = item.get<std::uint64_t>();
        if (number >= min && number <= max) {
            return number;
        }
    }
    throw py::ValueError(std::format("Invalid {}.", label));
}

int list_limit(const Json& item) {
    if (!item.is_number_integer()) {
        return 100;
    }
    if (item.is_number_unsigned()) {
        return static_cast<int>(
            std::clamp(item.get<std::uint64_t>(), std::uint64_t{1}, std::uint64_t{500}));
    }
    return static_cast<int>(
        std::clamp(item.get<std::int64_t>(), std::int64_t{1}, std::int64_t{500}));
}

std::uint64_t file_size_or_zero(const fs::path& path) {
    std::error_code error;
    auto result = fs::file_size(path, error);
    return !error ? result : 0;
}

std::optional<Json> read_json(const fs::path& file) {
    if (file_size_or_zero(file) > kMaxManifest) {
        return std::nullopt;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    auto json = Json::parse(input, nullptr, false);
    if (!json.is_object()) {
        return std::nullopt;
    }
    return json;
}

void publish(const fs::path& file, const Json& json) {
    write_file_atomic(file, json.dump());
}

std::uint64_t declared_bytes(const Json& files) {
    std::uint64_t total = 0;
    for (const auto& entry : files) {
        total += integer(value(entry, "size"), 0, kMaxRecord, "file size");
        if (total > kMaxRecord) {
            throw py::ValueError("Files larger than the record declared.");
        }
    }
    return total;
}

std::vector<std::string> owner_ids(const remote::PairedDevice& device) {
    require_owner(device.id);
    std::vector<std::string> out{device.id};
    for (const auto& owner : device.record_owner_ids) {
        require_owner(owner);
        if (std::ranges::find(out, owner) == out.end()) {
            out.push_back(owner);
        }
    }
    return out;
}

std::string timestamp(const Json& item) {
    auto result = str(value(item, "timestamp"));
    return result.empty() ? str(value(item, "started_at")) : result;
}

} // namespace

std::string safe_name(const Json& name) {
    const auto text = str(name);
    if (text.empty() || text.size() > 400) {
        throw py::ValueError("Invalid file name in record.");
    }
    std::string_view remaining(text);
    int count = 0;
    for (;;) {
        const auto end = remaining.find('/');
        if (++count > 4 || !ordinary_segment(remaining.substr(0, end))) {
            throw py::ValueError("Invalid file name in record.");
        }
        if (end == std::string_view::npos) {
            return text;
        }
        remaining.remove_prefix(end + 1);
    }
}

std::string safe_record_id(const Json& id) {
    auto text = str(id);
    if (text.empty() || !ascii_alnum(text.front()) || text.size() > 64 ||
        !std::ranges::all_of(text,
                             [](char ch) { return ascii_alnum(ch) || ch == '_' || ch == '-'; }) ||
        !ordinary_segment(text)) {
        throw py::ValueError("Invalid record id.");
    }
    return text;
}

std::string safe_kind(const Json& kind) {
    auto text = str(kind);
    if (text != "dictation" && text != "meeting") {
        throw py::ValueError("Unknown kind of record.");
    }
    return text;
}

HostRecordStore::HostRecordStore(fs::path root, Kinds kinds, Clock& clock, SpaceProbe space)
    : root_(std::move(root)), kinds_(std::move(kinds)), clock_(clock), space_(std::move(space)) {}

fs::path HostRecordStore::path(std::string_view area, std::string_view device,
                               std::string_view kind, std::string_view id) const {
    require_owner(device);
    auto relative = path_from_utf8(area) / path_from_utf8(device);
    if (!kind.empty()) {
        relative /= safe_kind(Json(kind));
    }
    if (!id.empty()) {
        relative /= safe_record_id(Json(id));
    }
    return beneath(root_, relative);
}

Json HostRecordStore::handle(std::string_view op, const Json& header,
                             std::span<const std::byte> payload,
                             const remote::PairedDevice& device) {
    assert_not_gui_thread();
    const auto request = gate_.request();
    std::lock_guard lock(mutex_);
    const auto owners = owner_ids(device);
    const auto kind = safe_kind(value(header, "kind"));
    const auto found = kinds_.find(kind);
    if (found == kinds_.end()) {
        throw py::ValueError("This record kind isn't supported on the host.");
    }
    auto& adapter = *found->second;
    if (op == "records_list") {
        std::vector<Json> records;
        const auto query = str(value(header, "query")).substr(0, 200);
        const auto limit = list_limit(value(header, "limit"));
        for (const auto& owner : owners) {
            auto rows = adapter.list(owner, query, limit);
            for (auto& item : rows) {
                records.push_back(std::move(item));
            }
        }
        std::stable_sort(records.begin(), records.end(),
                         [](const Json& a, const Json& b) { return timestamp(a) > timestamp(b); });
        records.resize(std::min(records.size(), static_cast<std::size_t>(limit)));
        return {{"records", std::move(records)}};
    }
    if (op == "records_clear") {
        int removed = 0;
        for (const auto& owner : owners) {
            removed += adapter.delete_all(owner);
            fs::remove_all(path("exports", owner, kind));
        }
        return {{"deleted", removed}};
    }
    const auto id = safe_record_id(value(header, "record_id"));
    const auto staging = path("staging", device.id, kind, id);
    if (op == "records_begin") {
        prune("staging", std::chrono::hours(24 * 7), staging);
        return begin(staging, header);
    }
    if (op == "records_put") {
        return put(staging, header, payload);
    }
    if (op == "records_abort") {
        fs::remove_all(staging);
        return Json::object();
    }

    auto owner = device;
    for (const auto& candidate : owners) {
        if (adapter.owns(candidate, id)) {
            owner.id = candidate;
            break;
        }
    }
    if (op == "records_commit") {
        return commit(staging, kind, id, adapter, owner);
    }
    if (op == "records_stat") {
        return {{"stored", adapter.stored_files(owner.id, id)}};
    }
    const auto exported = path("exports", owner.id, kind, id);
    if (op == "records_open") {
        prune("exports", 15min);
        return open(exported, owner.id, id, adapter);
    }
    if (op == "records_fetch") {
        return fetch(exported, header);
    }
    if (op == "records_delete_version") {
        const auto revision = safe_record_id(value(header, "revision"));
        const bool removed = adapter.remove_version(owner.id, id, revision);
        if (removed) {
            fs::remove_all(exported);
        }
        return {{"deleted", removed}};
    }
    if (op == "records_delete") {
        const bool removed = adapter.remove(owner.id, id);
        fs::remove_all(exported);
        return {{"deleted", removed}};
    }
    throw py::ValueError("Unknown record operation.");
}

Json HostRecordStore::begin(const fs::path& dir, const Json& header) {
    const auto count = integer(value(header, "files"), 1, kMaxFiles, "file count");
    const auto bytes = integer(value(header, "bytes"), 0, kMaxRecord, "record size");
    const auto manifest_path = beneath(dir, "upload.json");
    auto manifest = read_json(manifest_path);
    if (!manifest || value(*manifest, "files") != Json(count) ||
        value(*manifest, "bytes") != Json(bytes)) {
        fs::remove_all(dir);
        fs::create_directories(beneath(dir, "files"));
        const auto started = std::chrono::duration<double>(clock_.utc().time_since_epoch()).count();
        manifest = Json{{"files", count},
                        {"bytes", bytes},
                        {"declared", Json::object()},
                        {"done", Json::array()},
                        {"started", started}};
        publish(manifest_path, *manifest);
    }

    Json received = Json::object();
    Json done = Json::array();
    std::uint64_t have = 0;
    for (const auto& [raw, entry] : (*manifest)["declared"].items()) {
        const auto name = safe_name(Json(raw));
        const auto file = beneath(dir, fs::path("files") / name);
        const auto size = integer(value(entry, "size"), 0, kMaxRecord, "file size");
        auto actual = file_size_or_zero(file);
        const auto& finished = value(*manifest, "done");
        const bool was_done = std::ranges::find(finished, Json(name)) != finished.end();
        if (actual > size || (was_done && (actual != size || !fs::is_regular_file(file)))) {
            fs::remove(file);
            actual = 0;
        } else if (was_done) {
            done.push_back(name);
        }
        have += actual;
        received[name] = actual;
    }
    const auto needed = have <= bytes ? bytes - have : bytes;
    const auto free = space_ ? space_(root_) : download::disk_free_bytes(root_);
    if (needed + kReserve > free) {
        throw std::runtime_error(std::format(
            "Not enough free space on the host to keep this record ({} MB needed).", needed >> 20));
    }
    (*manifest)["done"] = done;
    publish(manifest_path, *manifest);
    fs::last_write_time(dir, fs::file_time_type::clock::now());
    return {{"received", std::move(received)}, {"done", std::move(done)}};
}

Json HostRecordStore::put(const fs::path& dir, const Json& header,
                          std::span<const std::byte> payload) {
    const auto name = safe_name(value(header, "name"));
    const auto size = integer(value(header, "size"), 0,
                              name == kManifest ? kMaxManifest : kMaxRecord, "file size");
    const auto offset = integer(value(header, "offset"), 0, kMaxRecord, "offset");
    const auto hash = str(value(header, "sha256"));
    if (hash.size() != 64 || !std::ranges::all_of(hash, [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) {
        throw py::ValueError("Invalid file hash.");
    }
    if (payload.size() > remote::RECORD_CHUNK_BYTES) {
        throw py::ValueError("Record chunk is too large.");
    }
    const auto manifest_path = beneath(dir, "upload.json");
    auto manifest = read_json(manifest_path);
    if (!manifest) {
        throw RecordNotFound("Start the upload again.");
    }
    auto& declared = (*manifest)["declared"];
    auto& done = (*manifest)["done"];
    const Json info{{"size", size}, {"sha256", hash}};
    if (declared.contains(name)) {
        if (declared[name] != info) {
            throw py::ValueError(name + " changed during the upload; start again.");
        }
    } else {
        const auto folded = fold(name);
        for (const auto& [other, ignored] : declared.items()) {
            (void)ignored;
            const auto old = fold(other);
            if (folded == old || folded.starts_with(old + "/") || old.starts_with(folded + "/")) {
                throw py::ValueError("A record contains conflicting file names.");
            }
        }
        if (declared.size() >= integer(value(*manifest, "files"), 1, kMaxFiles, "file count")) {
            throw py::ValueError("More files than the record declared.");
        }
        if (declared_bytes(declared) + size >
            integer(value(*manifest, "bytes"), 0, kMaxRecord, "record size")) {
            throw py::ValueError("Files larger than the record declared.");
        }
        declared[name] = info;
        publish(manifest_path, *manifest);
    }

    const auto file = beneath(dir, fs::path("files") / name);
    const auto have = file_size_or_zero(file);
    if (std::ranges::find(done, Json(name)) != done.end()) {
        return {{"received", size}, {"done", true}};
    }
    if (offset != have) {
        return {{"received", have}, {"done", false}, {"resume", true}};
    }
    const auto received = have + payload.size();
    if (received > size) {
        throw py::ValueError(name + " is longer than declared.");
    }
    fs::create_directories(file.parent_path());
    {
        std::ofstream output(file, std::ios::binary | std::ios::app);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        if (!payload.empty()) {
            output.write(reinterpret_cast<const char*>(payload.data()),
                         static_cast<std::streamsize>(payload.size()));
        }
        output.close();
    }
    fs::last_write_time(dir, fs::file_time_type::clock::now());
    if (received < size) {
        return {{"received", received}, {"done", false}};
    }
    if (download::sha256_file(file, true) != hash) {
        fs::remove(file);
        throw py::ValueError(name + " arrived damaged; it will be sent again.");
    }
    done.push_back(name);
    publish(manifest_path, *manifest);
    return {{"received", received}, {"done", true}};
}

Json HostRecordStore::commit(const fs::path& dir, std::string_view kind, std::string_view id,
                             RecordKind& adapter, const remote::PairedDevice& owner) {
    auto manifest = read_json(beneath(dir, "upload.json"));
    if (!manifest) {
        throw RecordNotFound("Nothing was uploaded for this record.");
    }
    const auto& declared = value(*manifest, "declared");
    auto& done = (*manifest)["done"];
    if (!done.is_array() || done.size() != declared.size() ||
        done.size() != integer(value(*manifest, "files"), 1, kMaxFiles, "file count")) {
        throw py::ValueError("The record's upload isn't complete.");
    }
    if (declared_bytes(declared) !=
        integer(value(*manifest, "bytes"), 0, kMaxRecord, "record size")) {
        throw py::ValueError("The record's size doesn't match what was declared.");
    }
    const auto files = beneath(dir, "files");
    for (const auto& [raw, entry] : declared.items()) {
        const auto name = safe_name(Json(raw));
        const auto now = std::ranges::find(done, Json(name));
        if (now == done.end()) {
            throw py::ValueError("The record's upload isn't complete.");
        }
        const auto file = beneath(files, name);
        if (!fs::is_regular_file(file) ||
            download::sha256_file(file, true) != str(value(entry, "sha256"))) {
            fs::remove(file);
            done.erase(now);
            publish(beneath(dir, "upload.json"), *manifest);
            throw py::ValueError(name + " arrived damaged; it will be sent again.");
        }
    }
    if (!declared.contains(std::string(kManifest))) {
        throw py::ValueError("The record has no record.json.");
    }
    const auto record = read_json(beneath(files, path_from_utf8(kManifest)));
    if (!record || str(value(*record, "kind")) != kind) {
        throw py::ValueError(
            "The record's record.json is unreadable or for another kind of record.");
    }
    Json result = adapter.import_record(id, files, *record, owner);
    fs::remove_all(dir);
    if (!result.is_object()) {
        result = Json::object();
    }
    result["stored"] = true;
    return result;
}

Json HostRecordStore::open(const fs::path& dir, std::string_view owner, std::string_view id,
                           RecordKind& adapter) {
    fs::remove_all(dir);
    fs::create_directories(dir);
    try {
        auto files = adapter.export_record(owner, id, dir);
        files[std::string(kManifest)] = beneath(dir, path_from_utf8(kManifest));
        Json index = Json::object();
        Json entries = Json::array();
        std::uint64_t size = 0;
        for (const auto& [raw, file] : files) {
            const auto name = safe_name(Json(raw));
            const auto bytes = fs::file_size(file);
            size += bytes;
            index[name] = path_to_utf8(file);
            const Json entry{{"name", name}, {"size", bytes}};
            if (name == kManifest) {
                entries.insert(entries.begin(), entry);
            } else {
                entries.push_back(entry);
            }
        }
        publish(beneath(dir, "index.json"), index);
        return {{"files", std::move(entries)}, {"bytes", size}};
    } catch (...) {
        std::error_code error;
        fs::remove_all(dir, error);
        throw;
    }
}

Json HostRecordStore::fetch(const fs::path& dir, const Json& header) {
    const auto name = safe_name(value(header, "name"));
    const auto offset = integer(value(header, "offset"), 0, kMaxRecord, "offset");
    const auto index = read_json(beneath(dir, "index.json"));
    if (!index) {
        throw RecordNotFound("Open the record again; its export has expired.");
    }
    const auto file = path_from_utf8(str(value(*index, name)));
    if (file.empty() || !fs::is_regular_file(file)) {
        throw RecordNotFound(name + " isn't part of this record anymore.");
    }
    fs::last_write_time(dir, fs::file_time_type::clock::now());
    const auto bytes = fs::file_size(file);
    std::ifstream data(file, std::ios::binary);
    data.seekg(static_cast<std::streamoff>(offset));
    std::vector<char> buffer(remote::RECORD_CHUNK_BYTES);
    data.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto got = static_cast<std::size_t>(data.gcount());
    return {{"data", b64encode(std::string_view(buffer.data(), got))},
            {"offset", offset},
            {"size", bytes},
            {"eof", offset + got >= bytes}};
}

void HostRecordStore::prune(std::string_view area, std::chrono::seconds ttl, const fs::path& keep) {
    const auto root = beneath(root_, path_from_utf8(area));
    std::error_code error;
    if (!fs::is_directory(root, error)) {
        return;
    }
    const auto cutoff = fs::file_time_type::clock::now() - ttl;
    for (const auto& owner : fs::directory_iterator(root)) {
        if (owner.is_symlink() || !owner.is_directory()) {
            continue;
        }
        for (const auto& kind : fs::directory_iterator(owner.path())) {
            if (kind.is_symlink() || !kind.is_directory()) {
                continue;
            }
            for (const auto& record : fs::directory_iterator(kind.path())) {
                if (record.is_directory() && !record.is_symlink() && record.path() != keep &&
                    record.last_write_time() < cutoff) {
                    const auto relative = record.path().lexically_relative(root);
                    fs::remove_all(beneath(root, relative), error);
                }
            }
        }
    }
}

int HostRecordStore::delete_device(std::string_view device) {
    const auto request = gate_.request();
    std::lock_guard lock(mutex_);
    const auto staging = path("staging", device);
    const auto exported = path("exports", device);
    int count = 0;
    for (const auto& entry : kinds_) {
        count += entry.second->delete_all(device);
    }
    fs::remove_all(staging);
    fs::remove_all(exported);
    return count;
}

int HostRecordStore::rename_device(std::string_view device, std::string_view name) {
    const auto request = gate_.request();
    std::lock_guard lock(mutex_);
    require_owner(device);
    int count = 0;
    for (const auto& entry : kinds_) {
        count += entry.second->rename_owner(device, name);
    }
    return count;
}

Json HostRecordStore::summary(const remote::PairedDevice& device) {
    std::lock_guard lock(mutex_);
    const auto ids = owner_ids(device);
    Json result = Json::object();
    for (const auto& [kind, adapter] : kinds_) {
        std::int64_t count = 0;
        std::int64_t bytes = 0;
        for (const auto& owner : ids) {
            const auto summary = adapter->summary(owner);
            count += summary.value("count", std::int64_t{0});
            bytes += summary.value("bytes", std::int64_t{0});
        }
        result[kind] = {{"count", count}, {"bytes", bytes}};
    }
    return result;
}

std::vector<remote::RecordOwnerGroup> HostRecordStore::owners() {
    std::lock_guard lock(mutex_);
    std::map<std::string, remote::RecordOwnerGroup, std::less<>> grouped;
    for (const auto& [kind, adapter] : kinds_) {
        for (auto owner : adapter->owners()) {
            auto& item = grouped[owner.id];
            item.id = owner.id;
            item.name = std::move(owner.name);
            item.counts.insert(owner.counts.begin(), owner.counts.end());
        }
    }
    std::vector<remote::RecordOwnerGroup> out;
    for (auto& entry : grouped) {
        out.push_back(std::move(entry.second));
    }
    return out;
}

} // namespace fv::remote_records
