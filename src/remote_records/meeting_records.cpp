#include "remote_records/meeting_records.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <set>
#include <utility>

#include "base/atomic_file.h"
#include "base/base64.h"
#include "base/hash.h"
#include "base/ids.h"
#include "base/utf8.h"
#include "db/database.h"
#include "download/verified_files.h"
#include "meeting_state/data_lifecycle.h"
#include "meeting_state/origin_rows.h"
#include "meeting_state/sql_repository.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "remote/record_sync_control.h"

namespace fv::remote_records {
namespace {

namespace fs = std::filesystem;
constexpr std::string_view kKind = "meeting";
constexpr auto kDigest = ".retained-digest";
constexpr std::array kChildren = {"chunks",      "segments",  "participants",
                                  "state_items", "questions", "events"};
constexpr std::array kLocalSession = {
    "host_token",       "guest_token",         "app_pid",
    "app_heartbeat_at", "origin_device_id",    "origin_device_name",
    "spool_dir",        "agent_endpoint_json", "asr_remote_json"};

const Json& member(const Json& object, const char* key) {
    static const Json missing;
    if (const auto found = object.find(key); found != object.end()) {
        return *found;
    }
    return missing;
}

std::string text(const Json& object, const char* key) {
    const auto& found = member(object, key);
    return found.is_string() ? found.get<std::string>() : std::string();
}

bool finished(const Json& session) {
    const auto state = text(session, "status");
    return state == "ended" || state == "failed";
}

fs::path meeting_file(const fs::path& folder, const std::string& name) {
    const auto path = path_from_utf8(name);
    const auto root = fs::weakly_canonical(fs::absolute(folder));
    const auto file = fs::weakly_canonical(root / path);
    if (file.lexically_relative(root) != path || !fs::is_regular_file(file)) {
        throw py::ValueError("A meeting file is missing or outside its folder.");
    }
    return file;
}

std::string audio_leaf(const Json& name) {
    const auto path = safe_name(name);
    if (!path.starts_with("audio/") || path.find('/', 6) != std::string::npos ||
        (!path.ends_with(".wav") && !path.ends_with("_session.json"))) {
        throw py::ValueError("The meeting has a file outside its audio folder.");
    }
    return path.substr(6);
}

bool wav_header(const fs::path& file) {
    std::array<char, 12> header{};
    std::ifstream input(file, std::ios::binary);
    return input.read(header.data(), static_cast<std::streamsize>(header.size())) &&
           std::string_view(header.data(), 4) == "RIFF" &&
           std::string_view(header.data() + 8, 4) == "WAVE";
}

Files validate_audio(const fs::path& folder, const Json& names) {
    if (!names.is_object()) {
        throw py::ValueError("The meeting's file list is missing.");
    }
    Files files;
    for (const auto& [name, size] : names.items()) {
        const auto leaf = audio_leaf(Json(name));
        const auto file = meeting_file(folder, name);
        if (!size.is_number_integer() ||
            (!size.is_number_unsigned() && size.get<std::int64_t>() < 0) ||
            size.get<std::uint64_t>() != fs::file_size(file)) {
            throw py::ValueError("A meeting file's size doesn't match the manifest.");
        }
        if (name.ends_with(".wav") && !wav_header(file)) {
            throw py::ValueError("A meeting recording isn't a WAV file.");
        }
        files.emplace(leaf, file);
    }
    return files;
}

using ScopedIds = std::map<std::string, std::string, std::less<>>;

struct MeetingIds {
    ScopedIds participants;
    ScopedIds segments;
    ScopedIds cards;
    ScopedIds questions;
};

ScopedIds scoped_ids(const Json& record, const char* table, std::string_view meeting) {
    ScopedIds result;
    const auto& collection = member(record, table);
    if (collection.is_null()) {
        return result;
    }
    if (!collection.is_array()) {
        throw py::ValueError("A meeting row collection isn't an array.");
    }
    for (const auto& entry : collection) {
        const auto source = text(entry, "id");
        if (source.empty() || source.size() > 256 ||
            !result
                 .try_emplace(source,
                              sha256_hex(Json::array({std::string(meeting), table, source}).dump())
                                  .substr(0, 32))
                 .second) {
            throw py::ValueError("A meeting row has an invalid or repeated id.");
        }
    }
    return result;
}

bool map_id(Json& value, const ScopedIds& scope) {
    if (value.is_string()) {
        if (const auto found = scope.find(value.get_ref<const std::string&>());
            found != scope.end()) {
            value = found->second;
            return true;
        }
    }
    return false;
}

void map_fields(Json& node, const MeetingIds& ids, int depth = 0) {
    if (depth > 32) {
        throw py::ValueError("A meeting field is nested too deeply.");
    }
    if (node.is_array()) {
        for (auto& value : node) {
            map_fields(value, ids, depth + 1);
        }
    } else if (node.is_object()) {
        for (auto& [key, value] : node.items()) {
            if (key == "participant_id" || key == "speaker_participant_id" ||
                key == "owner_participant_id") {
                if (!map_id(value, ids.participants)) {
                    value = nullptr;
                }
            } else if (key == "author_id" || key == "actor_id") {
                map_id(value, ids.participants);
            } else if (key == "segment_id") {
                map_id(value, ids.segments);
            } else if (key == "question_id") {
                map_id(value, ids.questions);
            } else if (key == "item_id") {
                map_id(value, ids.cards);
            } else if (key == "target_id") {
                (void)(map_id(value, ids.participants) || map_id(value, ids.segments) ||
                       map_id(value, ids.cards) || map_id(value, ids.questions));
            } else if ((key == "evidence" || key == "segment_ids" ||
                        key == "rolling_summary_evidence") &&
                       value.is_array()) {
                for (auto& ref : value) {
                    map_id(ref, ids.segments);
                }
            } else if (value.is_structured()) {
                map_fields(value, ids, depth + 1);
            }
        }
    }
}

void map_json_field(Json& row, const char* key, const MeetingIds& ids, bool is_evidence = false) {
    const auto saved = row.find(key);
    if (saved == row.end() || !saved->is_string()) {
        return;
    }
    auto value = Json::parse(saved->get_ref<const std::string&>(), nullptr, false);
    if (value.is_discarded()) {
        throw py::ValueError("A meeting contains an unreadable JSON field.");
    }
    if (is_evidence && value.is_array()) {
        for (auto& ref : value) {
            map_id(ref, ids.segments);
        }
    } else {
        map_fields(value, ids);
    }
    *saved = value.dump();
}

void map_snapshot(Json& session, const MeetingIds& ids) {
    const auto saved = session.find("state_json");
    if (saved == session.end() || !saved->is_string() ||
        saved->get_ref<const std::string&>().empty()) {
        return;
    }
    auto state = Json::parse(saved->get_ref<const std::string&>(), nullptr, false);
    if (!state.is_object()) {
        throw py::ValueError("The meeting's snapshot isn't readable.");
    }
    state["meeting_id"] = session.at("id");
    state["status"] = session.at("status");
    state["cloud_enabled"] = false;
    state["intelligence_online"] = false;
    if (auto people = state.find("participants"); people != state.end() && people->is_object()) {
        Json mapped = Json::object();
        for (auto& [source, person] : people->items()) {
            if (const auto found = ids.participants.find(source);
                found != ids.participants.end() && person.is_object()) {
                person["id"] = found->second;
                mapped[found->second] = std::move(person);
            }
        }
        *people = std::move(mapped);
    }
    if (auto cards = state.find("cards"); cards != state.end() && cards->is_object()) {
        for (auto& [key, items] : cards->items()) {
            if (items.is_array()) {
                for (auto& item : items) {
                    if (item.is_object() && item.contains("id")) {
                        map_id(item["id"], ids.cards);
                    }
                }
            }
        }
    }
    if (auto questions = state.find("questions");
        questions != state.end() && questions->is_array()) {
        for (auto& item : *questions) {
            if (item.is_object() && item.contains("id")) {
                map_id(item["id"], ids.questions);
            }
        }
    }
    map_fields(state, ids);
    *saved = state.dump();
}

Json import_rows(const Json& record, std::string_view id, const fs::path& spool,
                 const MeetingIds& ids) {
    Json rows = Json::object();
    std::set<std::int64_t> chunk_refs;
    for (const auto* collection : kChildren) {
        const auto& items = member(record, collection);
        if (items.is_null()) {
            continue;
        }
        if (!items.is_array()) {
            throw py::ValueError("A meeting row collection isn't an array.");
        }
        auto& normalized = rows[collection] = Json::array();
        for (auto row : items) {
            if (!row.is_object()) {
                throw py::ValueError("A meeting row isn't an object.");
            }
            row["meeting_id"] = std::string(id);
            const std::string_view table(collection);
            if (table == "participants") {
                map_id(row["id"], ids.participants);
            } else if (table == "segments") {
                map_id(row["id"], ids.segments);
            } else if (table == "state_items") {
                map_id(row["id"], ids.cards);
            } else if (table == "questions") {
                map_id(row["id"], ids.questions);
            }
            map_fields(row, ids);
            for (const auto* key : {"data_json", "thread_json", "payload_json", "inverse_json"}) {
                map_json_field(row, key, ids);
            }
            map_json_field(row, "evidence_json", ids, true);
            if (table == "chunks") {
                const auto ref = member(row, "id");
                if (!ref.is_number_integer() ||
                    (ref.is_number_unsigned() &&
                     ref.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) ||
                    !chunk_refs.insert(ref.get<std::int64_t>()).second) {
                    throw py::ValueError("A meeting chunk has an invalid or repeated id.");
                }
                row["chunk_ref"] = ref;
                row.erase("id");
                const auto name = audio_leaf(Json("audio/" + text(row, "file_path")));
                row["file_path"] = path_to_utf8(spool / path_from_utf8(name));
                row["asr_status"] = "done";
            } else if (table == "events") {
                row.erase("id");
            } else if (table == "segments" && member(row, "embedding").is_object()) {
                const auto encoded = text(row["embedding"], "$b64");
                constexpr std::size_t max_embedding = 64 * 1024;
                if (encoded.empty() || encoded.size() > 4 * ((max_embedding + 2) / 3)) {
                    throw py::ValueError("A meeting segment has an invalid embedding size.");
                }
                const auto decoded = b64decode(encoded);
                if (!decoded || decoded->empty() || decoded->size() > max_embedding ||
                    decoded->size() % sizeof(float) != 0) {
                    throw py::ValueError("A meeting segment has an invalid embedding.");
                }
                std::vector<std::uint8_t> embedding;
                embedding.reserve(decoded->size());
                for (const auto byte : *decoded) {
                    embedding.push_back(std::to_integer<std::uint8_t>(byte));
                }
                row["embedding"] = Json::binary(std::move(embedding));
            }
            normalized.push_back(std::move(row));
        }
    }
    return rows;
}

std::string digest(const Json& record, const Files& audio) {
    py::DumpOptions sorted;
    sorted.sort_keys = true;
    auto input = sha256_hex(py::dumps(record, sorted));
    for (const auto& [name, path] : audio) {
        input += name + ":" + download::sha256_file(path, true);
    }
    return sha256_hex(input);
}

class MeetingRecords final : public RecordKind {
public:
    MeetingRecords(fs::path root, Database& database, const Clock& clock)
        : root_(fs::weakly_canonical(fs::absolute(std::move(root)))), db_(database),
          repository_(database, clock) {}

    Json import_record(std::string_view raw_id, const fs::path& files, const Json& record,
                       const remote::PairedDevice& owner) override {
        if (owner.id.empty()) {
            throw py::ValueError("A retained meeting needs a paired owner.");
        }
        return import_bundle(raw_id, files, record, &owner);
    }

    Json import_local(std::string_view raw_id, const fs::path& files, const Json& record) {
        return import_bundle(raw_id, files, record, nullptr);
    }

private:
    Json import_bundle(std::string_view raw_id, const fs::path& files, const Json& record,
                       const remote::PairedDevice* owner) {
        const auto id = safe_record_id(Json(raw_id));
        Json session = member(record, "session");
        if (text(record, "kind") != kKind || !finished(session) || text(session, "id") != id ||
            member(record, "format") != 1 || member(record, "update") == true) {
            throw py::ValueError("Send a complete, finished meeting with its matching id.");
        }
        const auto audio = validate_audio(files, member(record, "files"));
        const auto incoming_digest = digest(record, audio);
        return db_.write([&](sql::Connection&) {
            if (const auto existing = repository_.get_meeting(id)) {
                if (owner ? text(*existing, "origin_device_id") != owner->id
                          : !member(*existing, "origin_device_id").is_null()) {
                    throw py::ValueError("A different meeting already has this id.");
                }
                const auto spool = checked_spool(*existing);
                if (!spool.empty()) {
                    std::ifstream marker(spool / kDigest);
                    std::string saved;
                    if (marker >> saved && saved == incoming_digest) {
                        return Json{{"files", audio.size()}, {"unchanged", true}};
                    }
                }
                throw std::runtime_error("The host already keeps a different version of this "
                                         "meeting. Both copies were kept.");
            }

            const auto spool = root_ / ("retained-" + uuid4_hex());
            const MeetingIds ids{
                scoped_ids(record, "participants", id), scoped_ids(record, "segments", id),
                scoped_ids(record, "state_items", id), scoped_ids(record, "questions", id)};
            const auto rows = import_rows(record, id, spool, ids);
            map_snapshot(session, ids);
            for (const auto* key : kLocalSession) {
                session.erase(key);
            }
            session["host_token"] = token_urlsafe();
            session["guest_token"] = token_urlsafe();
            session["cloud_enabled"] = false;
            session["spool_dir"] = path_to_utf8(spool);
            session["origin_device_id"] = owner ? Json(owner->id) : Json();
            session["origin_device_name"] = owner ? Json(owner->name) : Json();
            if (!fs::create_directories(spool)) {
                throw std::runtime_error("Couldn't prepare the host's meeting folder.");
            }
            try {
                for (const auto& [name, source] : audio) {
                    fs::copy_file(source, spool / path_from_utf8(name));
                }
                write_file_atomic(spool / kDigest, incoming_digest);
                meeting_state::replace_meeting_rows(db_, id, session, rows, false);
            } catch (...) {
                std::error_code ignored;
                fs::remove_all(spool, ignored);
                throw;
            }
            return Json{{"files", audio.size()}};
        });
    }

public:
    Json list(std::string_view device, std::string_view query, int limit) override {
        Json out = Json::array();
        if (device.empty()) {
            return out;
        }
        meeting_state::PastMeetingQuery scope;
        scope.limit = limit;
        scope.query = query;
        scope.origin = meeting_state::PastMeetingQuery::Origin::Device;
        scope.origin_device_id = device;
        for (const auto& row : repository_.list_past_meeting_summaries(scope)) {
            if (!finished(row)) {
                continue;
            }
            Json item = Json::object();
            for (const auto* key :
                 {"id", "title", "status", "started_at", "ended_at", "paused_total_s",
                  "cloud_enabled", "asr_model", "content_summary"}) {
                item[key] = member(row, key);
            }
            if (const auto meeting = repository_.get_meeting(text(row, "id"));
                meeting && text(*meeting, "origin_device_id") == device) {
                item["revision"] = revision_of(*meeting);
            }
            item["preview"] = text(member(row, "content_summary"), "preview_text");
            out.push_back(std::move(item));
        }
        return out;
    }

    Files export_record(std::string_view device, std::string_view id,
                        const fs::path& directory) override {
        return export_bundle(owned(device, id), id, directory, true, nullptr);
    }

    Files export_local(std::string_view id, const fs::path& directory, bool include_audio,
                       const CancelToken& cancel) {
        const auto session = repository_.get_meeting(id);
        if (!session || !member(*session, "origin_device_id").is_null() || !finished(*session) ||
            meeting_state::count_unfinished_chunks(db_, id) != 0) {
            throw remote::RecordsUnavailable(
                "Choose a finished meeting from this computer after its recordings are processed.");
        }
        const auto snapshot = Json::parse(text(*session, "state_json"), nullptr, false);
        const auto finalizing = text(member(snapshot, "finalization"), "status");
        if (finalizing == "pending" || finalizing == "running" ||
            text(member(snapshot, "insight_review"), "status") == "running") {
            throw remote::RecordsUnavailable(
                "Wait until the meeting finishes preparing its notes before sharing it.");
        }
        return export_bundle(*session, id, directory, include_audio, &cancel);
    }

private:
    Files export_bundle(const Json& session, std::string_view id, const fs::path& directory,
                        bool include_audio, const CancelToken* sharing) {
        auto manifest = meeting_state::export_meeting_rows(db_, id);
        for (const auto* key : kLocalSession) {
            manifest["session"].erase(key);
        }
        std::set<std::string> referenced_audio;
        for (auto& chunk : manifest["chunks"]) {
            const auto name = path_to_utf8(path_from_utf8(text(chunk, "file_path")).filename());
            chunk["file_path"] = name;
            referenced_audio.insert("audio/" + name);
        }
        for (auto& segment : manifest["segments"]) {
            auto& embedding = segment["embedding"];
            if (sharing) {
                segment.erase("embedding");
            } else if (embedding.is_binary()) {
                const auto& binary = embedding.get_binary();
                embedding = Json{
                    {"$b64", b64encode(std::as_bytes(std::span(binary.data(), binary.size())))}};
            }
        }
        if (sharing) {
            manifest["events"] = Json::array();
            manifest["session"].erase("agent_provider");
            manifest["session"].erase("agent_model");
        }
        if (!include_audio) {
            manifest["chunks"] = Json::array();
            for (auto& segment : manifest["segments"]) {
                segment["chunk_id"] = nullptr;
            }
        }
        manifest["kind"] = std::string(kKind);
        manifest["format"] = 1;
        manifest["update"] = false;
        manifest["files"] = Json::object();
        Files files;
        if (include_audio) {
            files = audio_files(session);
            if (sharing) {
                std::erase_if(files, [&](const auto& file) {
                    const auto& name = file.first;
                    return !referenced_audio.contains(name) && !name.ends_with("_session.wav") &&
                           !name.ends_with("_session.json");
                });
            }
        }
        for (auto& [name, path] : files) {
            if (sharing) {
                if (sharing->cancelled()) {
                    throw remote::RecordsUnavailable("Meeting sharing was canceled.");
                }
                const auto frozen = directory / path_from_utf8(safe_name(Json(name)));
                fs::create_directories(frozen.parent_path());
                fs::copy_file(path, frozen);
                path = frozen;
            }
            manifest["files"][name] = fs::file_size(path);
        }
        write_file_atomic(directory / "record.json", manifest.dump());
        return files;
    }

public:
    Json stored_files(std::string_view, std::string_view) override { return Json::object(); }

    bool remove_version(std::string_view device, std::string_view id,
                        std::string_view revision) override {
        return db_.write([&](sql::Connection&) {
            const auto row = repository_.get_meeting(id);
            if (device.empty() || !row || text(*row, "origin_device_id") != device ||
                !finished(*row)) {
                return false;
            }
            if (revision.empty() || revision_of(*row) != revision) {
                throw py::ValueError(
                    "The host copy changed. Refresh the meeting list before deleting.");
            }
            return remove(device, id);
        });
    }

    bool remove(std::string_view device, std::string_view id) override {
        if (!owns(device, id)) {
            return false;
        }
        return meeting_state::delete_meeting_data(repository_, std::string(id), root_);
    }

    int delete_all(std::string_view device) override {
        int deleted = 0;
        if (!device.empty()) {
            for (const auto& row : meeting_state::origin_spools(db_, device)) {
                deleted += remove(device, row.meeting_id) ? 1 : 0;
            }
        }
        return deleted;
    }

    int rename_owner(std::string_view device, std::string_view name) override {
        return device.empty() ? 0 : meeting_state::rename_origin(db_, device, name);
    }

    Json summary(std::string_view device) override {
        std::size_t count = 0;
        std::uint64_t bytes = 0;
        if (!device.empty()) {
            for (const auto& item : meeting_state::origin_spools(db_, device)) {
                ++count;
                for (const auto& [name, file] : audio_files({{"spool_dir", item.spool_dir}})) {
                    bytes += fs::file_size(file);
                }
            }
        }
        return {{"count", count}, {"bytes", bytes}};
    }

    std::vector<remote::RecordOwnerGroup> owners() override {
        std::vector<remote::RecordOwnerGroup> groups;
        for (auto& owner : meeting_state::record_owners(db_)) {
            groups.push_back(
                {std::move(owner.id), std::move(owner.name), {{"meeting", owner.count}}});
        }
        return groups;
    }

    bool owns(std::string_view device, std::string_view id) override {
        if (!device.empty()) {
            const auto row = repository_.get_meeting(id);
            return row && text(*row, "origin_device_id") == device && finished(*row);
        }
        return false;
    }

private:
    Json owned(std::string_view device, std::string_view id) {
        const auto row = repository_.get_meeting(id);
        if (device.empty() || !row || text(*row, "origin_device_id") != device || !finished(*row)) {
            throw RecordNotFound("That meeting isn't kept here for this computer.");
        }
        return *row;
    }

    std::string revision_of(const Json& session) const {
        const auto spool = checked_spool(session);
        if (spool.empty()) {
            return {};
        }
        const auto name = path_to_utf8(spool.filename());
        return name.starts_with("retained-") ? name.substr(9) : std::string();
    }

    fs::path checked_spool(const Json& session) const {
        const auto raw = text(session, "spool_dir");
        if (raw.empty()) {
            return {};
        }
        std::error_code problem;
        const auto candidate = path_from_utf8(raw);
        if (!candidate.is_absolute() || !fs::is_directory(fs::symlink_status(candidate, problem)) ||
            problem) {
            return {};
        }
        const auto spool = fs::canonical(candidate, problem);
        return !problem && spool.filename() == candidate.filename() &&
                       fs::equivalent(spool.parent_path(), root_, problem) && !problem
                   ? spool
                   : fs::path{};
    }

    Files audio_files(const Json& session) const {
        Files files;
        const auto spool = checked_spool(session);
        if (spool.empty()) {
            return files;
        }
        for (const auto& item : fs::directory_iterator(spool)) {
            const auto name = path_to_utf8(item.path().filename());
            if ((name.ends_with(".wav") && name != "playback.wav") ||
                name.ends_with("_session.json")) {
                if (fs::is_regular_file(item.symlink_status())) {
                    files.emplace("audio/" + name, item.path());
                }
            }
        }
        return files;
    }

    fs::path root_;
    Database& db_;
    meeting_state::SqlMeetingRepository repository_;
};

} // namespace

std::shared_ptr<RecordKind> meeting_records(std::filesystem::path root, Database& database,
                                            const Clock& clock) {
    return std::make_shared<MeetingRecords>(std::move(root), database, clock);
}

std::vector<Json> local_meetings_for_sharing(Database& database, const Clock& clock, int limit) {
    meeting_state::SqlMeetingRepository repository(database, clock);
    meeting_state::PastMeetingQuery scope;
    scope.limit = limit;
    scope.origin = meeting_state::PastMeetingQuery::Origin::Local;
    auto meetings = repository.list_past_meeting_summaries(scope);
    std::erase_if(meetings, [](const Json& session) { return !finished(session); });
    for (auto& session : meetings) {
        session = Json{{"id", text(session, "id")},
                       {"title", text(session, "title")},
                       {"started_at", text(session, "started_at")}};
    }
    return meetings;
}

Files export_meeting_for_sharing(const fs::path& root, Database& database, const Clock& clock,
                                 std::string_view id, const fs::path& directory, bool include_audio,
                                 const CancelToken& cancel) {
    return MeetingRecords(root, database, clock).export_local(id, directory, include_audio, cancel);
}

void import_meeting_for_viewing(const fs::path& root, Database& database, const Clock& clock,
                                std::string_view id, const fs::path& files, const Json& record) {
    (void)MeetingRecords(root, database, clock).import_local(id, files, record);
}

} // namespace fv::remote_records
