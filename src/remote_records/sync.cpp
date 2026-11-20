#include "remote_records/sync.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <set>
#include <utility>

#include "base/atomic_file.h"
#include "base/base64.h"
#include "base/hash.h"
#include "base/ids.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "history/retention_settings.h"
#include "pyport/json.h"
#include "remote/wire.h"
#include "remote_records/dictation_records.h"
#include "remote_records/meeting_records.h"
#include "settings/keys.h"

namespace fv::remote_records {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr std::string_view kKind = "dictation";
constexpr std::uint64_t kMaxBytes = 64ULL << 30;
// sync.py LIST_TIMEOUT_S and UNREACHABLE_QUIET_S.
constexpr auto kListTimeout = 8s;

const Json& member(const Json& data, std::string_view name) {
    static const Json missing;
    const auto at = data.find(std::string(name));
    return at == data.end() ? missing : *at;
}

std::string text(const Json& data, std::string_view name) {
    const auto& item = member(data, name);
    return item.is_string() ? item.get<std::string>() : std::string();
}

std::optional<std::uint64_t> bytes(const Json& value, std::uint64_t maximum = kMaxBytes) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        return std::nullopt;
    }
    const auto size = value.get<std::uint64_t>();
    return size <= maximum ? std::make_optional(size) : std::nullopt;
}

Json fields(std::string_view id) {
    return {{"kind", kKind}, {"record_id", id}};
}

Json read_record(const fs::path& directory, std::string_view kind = kKind) {
    const auto manifest = directory / "record.json";
    if (!fs::is_regular_file(manifest) || fs::file_size(manifest) > (256ULL << 20)) {
        throw remote::RecordsUnavailable("The record manifest is missing or too large.");
    }
    std::ifstream input(manifest, std::ios::binary);
    const auto record = Json::parse(input, nullptr, false);
    if (!record.is_object() || text(record, "kind") != kind) {
        throw remote::RecordsUnavailable("The host sent a record this version can't read.");
    }
    return record;
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const fs::path& root) : path(root / ("request-" + uuid4_hex())) {
        fs::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    fs::path path;
};

std::string digest(const fs::path& manifest, const Files& files) {
    auto combined = download::sha256_file(manifest, true);
    for (const auto& [name, file] : files) {
        combined += name + ":" + download::sha256_file(file, true);
    }
    return sha256_hex(combined);
}

// sync.py _retention: this computer's recording limits travel with the record, so the host
// keeps no more of its recordings than this computer would. Added after the digest is taken:
// changing the limits does not make a queued record look edited.
void with_retention(const fs::path& manifest, const Json& settings) {
    std::ifstream input(manifest, std::ios::binary);
    auto record = Json::parse(input, nullptr, false);
    input.close();
    if (!record.is_object()) {
        return;
    }
    const auto limit = [](std::optional<std::int64_t> value) {
        return value ? Json(*value) : Json();
    };
    record["retention"] = {{"max_recordings", limit(resolve_max_saved_recordings(settings))},
                           {"max_bytes", limit(resolve_max_saved_recordings_bytes(settings))}};
    write_file_atomic(manifest, record.dump());
}

} // namespace

RecordSync::RecordSync(SyncDeps deps)
    : deps_(std::move(deps)), outbox_(deps_.db, deps_.clock), entries_(deps_.db),
      adapter_(dictation_records(deps_.recordings, deps_.db)),
      audio_cache_dir_(deps_.transfer_root / ("playback-" + uuid4_hex())) {}

RecordSync::~RecordSync() {
    std::error_code ignored;
    fs::remove_all(audio_cache_dir_, ignored);
}

void RecordSync::shutdown() {
    stopping_.cancel();
}

std::string RecordSync::location() {
    return remote::records_location(*deps_.settings.snapshot());
}

void RecordSync::set_location(std::string_view selected) {
    if (selected != "local" && selected != "host" && selected != "both") {
        throw std::invalid_argument(std::format("Unknown storage location: '{}'", selected));
    }
    deps_.settings.save_setting(keys::remote_records_location.key(), std::string(selected));
    // Records already on the host stay there; ones still waiting follow the new choice.
    outbox_.retarget(selected);
    changed.emit("records");
}

remote::SyncStatus RecordSync::status() {
    assert_not_gui_thread();
    const auto peer = deps_.pairing();
    const auto own = entries_.get_history_entries({}, HistoryOrigin::own());
    const auto where = location();
    int pending = 0;
    if (peer) {
        for (const auto& row : outbox_.rows(peer->fingerprint)) {
            pending += row.state == "done" ? 0 : 1;
        }
    }
    std::lock_guard lock(state_mutex_);
    remote::SyncStatus result;
    if (peer && observed_host_ == peer->fingerprint) {
        result = state_;
    }
    result.location = where;
    result.paired = peer.has_value();
    result.host_name = peer ? peer->host_name : std::string();
    result.pending = pending;
    result.local_counts["dictation"] = static_cast<std::int64_t>(own.size());
    return result;
}

remote::ClientPairing RecordSync::require_pairing() {
    assert_not_gui_thread();
    const auto peer = deps_.pairing();
    if (!peer || stopping_.cancelled()) {
        throw remote::RecordsUnavailable("Pair with a host before transferring records.");
    }
    return *peer;
}

void RecordSync::require_same_peer(const remote::ClientPairing& expected) {
    if (const auto peer = deps_.pairing(); stopping_.cancelled() || !peer || *peer != expected) {
        throw remote::RecordsUnavailable("The paired host changed or the transfer stopped.");
    }
}

void RecordSync::observe(const remote::ClientPairing& peer, const Json& ready) {
    std::lock_guard lock(state_mutex_);
    if (observed_host_ != peer.fingerprint) {
        observed_host_ = peer.fingerprint;
        state_ = {};
        remote_items_.clear();
        audio_.clear();
    }
    const auto& permission = member(member(ready, "capabilities"), "records");
    state_.host_supports = permission.is_boolean();
    state_.host_keeps =
        permission.is_boolean() ? std::make_optional(permission.get<bool>()) : std::nullopt;
    state_.stored.clear();
    const auto& stored = member(ready, "records");
    if (stored.is_object()) {
        for (const auto& [kind, info] : stored.items()) {
            const auto count = bytes(member(info, "count"));
            const auto size = bytes(member(info, "bytes"));
            if (count && size) {
                state_.stored[kind] = {static_cast<std::int64_t>(*count),
                                       static_cast<std::int64_t>(*size)};
            }
        }
    }
}

std::unique_ptr<remote::RemoteConnection> RecordSync::open(const remote::ClientPairing& expected,
                                                           bool upload, const CancelToken& cancel) {
    require_same_peer(expected);
    auto connection = deps_.connect(expected);
    const auto ready = connection->connect(cancel);
    observe(expected, ready);
    const auto& permission = member(member(ready, "capabilities"), "records");
    if (!permission.is_boolean()) {
        throw remote::RecordsUnavailable("This host does not support storing records.");
    }
    if (upload && !permission.get<bool>()) {
        std::string waiting = expected.host_name +
                              " isn't keeping records for other computers yet. Turn on \"Keep "
                              "records for paired computers\" in its Settings → Remote engine.";
        {
            std::lock_guard lock(state_mutex_);
            state_.waiting = waiting;
        }
        throw remote::RecordsUnavailable(waiting);
    }
    if (upload) {
        std::lock_guard lock(state_mutex_);
        state_.waiting.clear();
    }
    return connection;
}

void RecordSync::refresh_summary() {
    const auto peer = require_pairing();
    (void)open(peer);
    changed.emit("records");
}

int RecordSync::send_existing() {
    const auto where = location();
    if (where == "local") {
        return 0;
    }
    const auto peer = require_pairing();
    const auto work = gate_.request();
    int count = 0;
    {
        std::lock_guard serial(transfers_);
        require_same_peer(peer);
        for (const auto& entry : entries_.get_history_entries({}, HistoryOrigin::own())) {
            if (entry.entry_kind == FAILED_RECORDING_KIND) {
                continue;
            }
            TemporaryDirectory directory(deps_.transfer_root);
            const auto files = adapter_->export_record("", entry.id, directory.path);
            const auto content = digest(directory.path / "record.json", files);
            count += outbox_.enqueue({std::string(kKind), entry.id, peer.fingerprint,
                                      where == "host" ? "move" : "copy", "pending", content})
                         ? 1
                         : 0;
        }
    }
    if (count) {
        wake(true);
    }
    return count;
}

void RecordSync::wake(bool now) {
    if (!now || stopping_.cancelled()) {
        return;
    }
    {
        std::lock_guard lock(state_mutex_);
        if (running_) {
            requested_again_ = true;
            return;
        }
        running_ = true;
    }
    deps_.io.post([self = shared_from_this()] {
        for (;;) {
            try {
                self->transfer();
            } catch (const std::exception& error) {
                self->problem(error.what());
            }
            bool again = false;
            {
                std::lock_guard lock(self->state_mutex_);
                again =
                    std::exchange(self->requested_again_, false) && !self->stopping_.cancelled();
                self->running_ = again;
                self->state_.active.clear();
            }
            self->changed.emit("records");
            if (!again) {
                break;
            }
        }
    });
}

void RecordSync::problem(std::string text) {
    std::lock_guard lock(state_mutex_);
    state_.error = std::move(text);
}

void RecordSync::transfer() {
    if (location() == "local") {
        return;
    }
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    const auto peer = require_pairing();
    auto connection = open(peer, true);
    for (const auto& item : outbox_.rows(peer.fingerprint)) {
        if (item.state == "done" || item.kind != kKind) {
            continue;
        }
        require_same_peer(peer);
        if (location() == "local") {
            break;
        }
        try {
            const auto saved = entries_.get_history_entry_by_id(item.id, HistoryOrigin::own());
            if (!saved) {
                outbox_.forget(item.kind, item.id, item.host);
                continue;
            }
            TemporaryDirectory directory(deps_.transfer_root);
            const auto files = adapter_->export_record("", item.id, directory.path);
            if (digest(directory.path / "record.json", files) != item.digest) {
                throw remote::RecordsUnavailable(
                    "A local record changed. Choose Copy or Move existing records again.");
            }
            with_retention(directory.path / "record.json", *deps_.settings.snapshot());
            {
                std::lock_guard lock(state_mutex_);
                state_.active = "Sending a dictation…";
            }
            changed.emit("records");
            const auto result =
                upload(*connection, peer, item, directory.path, files, stopping_.token());
            if (member(result, "stored") != true) {
                throw remote::RecordsUnavailable("The host did not confirm the record was stored.");
            }
            if (item.action == "move" && location() == "host") {
                require_same_peer(peer);
                deps_.db.write([&](sql::Connection&) {
                    const auto current =
                        entries_.get_history_entry_by_id(item.id, HistoryOrigin::own());
                    if (!current) {
                        return;
                    }
                    TemporaryDirectory check(deps_.transfer_root);
                    const auto current_files = adapter_->export_record("", item.id, check.path);
                    if (digest(check.path / "record.json", current_files) != item.digest) {
                        throw remote::RecordsUnavailable("This dictation changed while it was "
                                                         "being copied. Both copies were kept.");
                    }
                    adapter_->remove("", item.id);
                });
            }
            outbox_.complete(item);
        } catch (const std::exception& error) {
            outbox_.fail(item, error.what());
            throw;
        }
    }
    problem({});
    (void)open(peer); // Read the post-transfer summary without re-sending anything.
}

Json RecordSync::upload(remote::RemoteConnection& connection, const remote::ClientPairing& peer,
                        const OutboxItem& item, const fs::path& directory, const Files& files,
                        const CancelToken& cancel) {
    Files bundle = files;
    bundle["record.json"] = directory / "record.json";
    std::uint64_t total = 0;
    for (const auto& entry : bundle) {
        total += fs::file_size(entry.second);
        if (total > kMaxBytes) {
            throw remote::RecordsUnavailable("The record is too large to send.");
        }
    }
    const Json key{{"kind", item.kind}, {"record_id", item.id}};
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            auto declaration = key;
            declaration["files"] = bundle.size();
            declaration["bytes"] = total;
            const auto response =
                connection.request_timed("records_begin", declaration, 60s, {}, {}, cancel).first;
            for (const auto& [name, path] : bundle) {
                const auto size = fs::file_size(path);
                auto offset = bytes(member(member(response, "received"), name), size).value_or(0);
                const auto hash = download::sha256_file(path, true);
                auto info = key;
                info["name"] = name;
                info["size"] = size;
                info["sha256"] = hash;
                std::ifstream file(path, std::ios::binary);
                if (!file) {
                    throw remote::RecordsUnavailable("A local recording cannot be read.");
                }
                for (;;) {
                    require_same_peer(peer);
                    file.clear();
                    file.seekg(static_cast<std::streamoff>(offset));
                    std::vector<char> buffer(remote::RECORD_CHUNK_BYTES);
                    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                    const auto count = file.gcount();
                    info["offset"] = offset;
                    const auto payload =
                        std::as_bytes(std::span(buffer.data(), static_cast<std::size_t>(count)));
                    const auto reply =
                        connection.request_timed("records_put", info, 120s, {}, payload, cancel)
                            .first;
                    if (member(reply, "done") == true) {
                        break;
                    }
                    const auto next = bytes(member(reply, "received"), size);
                    if (!next || *next == offset) {
                        throw remote::RecordsUnavailable("The host did not accept a record chunk.");
                    }
                    offset = *next;
                }
            }
            require_same_peer(peer);
            return connection.request_timed("records_commit", key, 300s, {}, {}, cancel).first;
        } catch (const remote::RemoteRequestError& error) {
            if (attempt || error.code != "bad_request") {
                throw;
            }
            require_same_peer(peer);
            connection.request_timed("records_abort", key, 30s, {}, {}, cancel);
        }
    }
    throw remote::RecordsUnavailable("The record could not be sent.");
}

std::vector<Json> RecordSync::list(remote::RemoteConnection& connection,
                                   const remote::ClientPairing& peer, std::string_view query,
                                   int limit, std::string_view kind) {
    require_same_peer(peer);
    const auto response =
        connection
            .request_timed(
                "records_list",
                Json{{"kind", kind}, {"query", query}, {"limit", std::clamp(limit, 1, 500)}},
                kListTimeout, {}, {}, stopping_.token())
            .first;
    require_same_peer(peer);
    const auto& records = member(response, "records");
    std::vector<Json> result;
    if (records.is_array()) {
        std::lock_guard lock(state_mutex_);
        for (auto record : records) {
            const auto id = text(record, "id");
            try {
                safe_record_id(Json(id));
            } catch (const std::exception&) {
                continue;
            }
            record["stored_on"] = peer.host_name;
            record["_stored_on_fingerprint"] = peer.fingerprint;
            if (kind == kKind) {
                remote_items_[id] = record;
            }
            result.push_back(std::move(record));
            if (result.size() >= static_cast<std::size_t>(std::clamp(limit, 1, 500))) {
                break;
            }
        }
    }
    return result;
}

std::vector<Json> RecordSync::list_remote(std::string_view kind, std::string_view query,
                                          int limit) {
    if (kind != kKind && kind != "meeting") {
        return {};
    }
    const auto peer = deps_.pairing();
    if (!peer || stopping_.cancelled()) {
        return {};
    }
    const auto unreachable = [&] {
        return remote::RecordsUnavailable(peer->host_name + " isn't reachable right now.");
    };
    {
        // History and Past Meetings ask on every refresh: a host that just failed to answer is
        // not asked again for a while, so each refresh doesn't wait out a connection timeout.
        std::lock_guard lock(state_mutex_);
        if (unreachable_since_ && deps_.clock.steady() - *unreachable_since_ < kUnreachableQuiet) {
            throw unreachable();
        }
    }
    std::unique_ptr<remote::RemoteConnection> connection;
    try {
        connection = open(*peer);
    } catch (const remote::RecordsUnavailable&) {
        // A host that keeps no records, or a pairing that changed meanwhile: nothing to list.
        return {};
    } catch (const std::exception&) {
        std::lock_guard lock(state_mutex_);
        unreachable_since_ = deps_.clock.steady();
        throw unreachable();
    }
    {
        std::lock_guard lock(state_mutex_);
        unreachable_since_.reset();
    }
    try {
        return list(*connection, *peer, query, limit, kind);
    } catch (const remote::RecordsUnavailable&) {
        throw;
    } catch (const std::exception& error) {
        const std::string reason = error.what();
        throw remote::RecordsUnavailable(reason.empty() ? "The host didn't answer." : reason);
    }
}

std::vector<Json> RecordSync::shareable_meetings(int limit) {
    return local_meetings_for_sharing(deps_.db, deps_.clock, limit);
}

void RecordSync::share_meeting(std::string_view expected_host, std::string_view id, bool audio,
                               const CancelToken& user_cancel) {
    const auto peer = require_pairing();
    if (peer.fingerprint != expected_host) {
        throw remote::RecordsUnavailable("The paired host changed. Confirm the new destination.");
    }
    const auto record = safe_record_id(Json(id));
    CancelSource cancel(stopping_.token());
    const auto user = user_cancel.on_cancel([cancel]() mutable { cancel.cancel(); });
    const auto token = cancel.token();
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    require_same_peer(peer);
    if (token.cancelled()) {
        throw remote::RecordsUnavailable("Meeting sharing was canceled.");
    }
    TemporaryDirectory bundle(deps_.transfer_root);
    const auto files = export_meeting_for_sharing(deps_.meetings, deps_.db, deps_.clock, record,
                                                  bundle.path, audio, token);
    if (token.cancelled()) {
        throw remote::RecordsUnavailable("Meeting sharing was canceled.");
    }
    auto connection = open(peer, true, token);
    const OutboxItem selected{"meeting", record, peer.fingerprint, "copy", {}, {}};
    const auto result = upload(*connection, peer, selected, bundle.path, files, token);
    if (member(result, "stored") != true) {
        throw remote::RecordsUnavailable("The paired host did not confirm it stored the meeting.");
    }
    outbox_.mark_done({"meeting", record, peer.fingerprint, "copy", "done", "shared"});
    changed.emit("records");
}

std::vector<Json> RecordSync::list_held_meetings(std::string_view expected_host, int limit) {
    const auto peer = require_pairing();
    if (peer.fingerprint != expected_host) {
        throw remote::RecordsUnavailable("The paired host changed. Reopen its meeting list.");
    }
    auto connection = open(peer);
    return list(*connection, peer, {}, limit, "meeting");
}

Json RecordSync::read_held_meeting(std::string_view expected_host, std::string_view id) {
    const auto peer = require_pairing();
    if (peer.fingerprint != expected_host) {
        throw remote::RecordsUnavailable("The paired host changed. Reopen its meeting list.");
    }
    const auto record_id = safe_record_id(Json(id));
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    auto connection = open(peer);
    Json request{{"kind", "meeting"}, {"record_id", record_id}};
    const auto opened =
        connection->request_timed("records_open", request, 30s, {}, {}, stopping_.token()).first;
    constexpr std::uint64_t max_manifest = 16ULL << 20;
    const auto& files = member(opened, "files");
    std::optional<std::uint64_t> size;
    if (files.is_array() && files.size() <= 50'000) {
        for (const auto& file : files) {
            if (text(file, "name") == "record.json") {
                if (size) {
                    throw remote::RecordsUnavailable(
                        "The host sent overlapping meeting manifests.");
                }
                size = bytes(member(file, "size"), max_manifest);
                if (!size || *size == 0) {
                    throw remote::RecordsUnavailable("The host's meeting preview is too large.");
                }
            }
        }
    }
    if (!size) {
        throw remote::RecordsUnavailable("The host didn't provide a meeting preview.");
    }

    request["name"] = "record.json";
    std::string contents;
    contents.reserve(static_cast<std::size_t>(*size));
    while (contents.size() < *size) {
        require_same_peer(peer);
        request["offset"] = contents.size();
        const auto reply =
            connection->request_timed("records_fetch", request, 30s, {}, {}, stopping_.token())
                .first;
        const auto data = b64decode(text(reply, "data"));
        if (!data || data->empty() || data->size() > remote::RECORD_CHUNK_BYTES ||
            data->size() > *size - contents.size() ||
            bytes(member(reply, "offset")) != contents.size()) {
            throw remote::RecordsUnavailable("The host sent an incomplete meeting preview.");
        }
        contents.append(reinterpret_cast<const char*>(data->data()), data->size());
    }
    require_same_peer(peer);

    const auto manifest = Json::parse(contents, nullptr, false);
    const auto& session = member(manifest, "session");
    if (text(manifest, "kind") != "meeting" || text(session, "id") != record_id) {
        throw remote::RecordsUnavailable("The host returned a different meeting.");
    }
    Json preview{{"id", record_id}, {"title", text(session, "title")}};
    std::string transcript;
    constexpr std::size_t max_preview = 128 * 1024;
    if (const auto& segments = member(manifest, "segments"); segments.is_array()) {
        std::vector<const Json*> ordered;
        ordered.reserve(segments.size());
        for (const auto& segment : segments) {
            ordered.push_back(&segment);
        }
        std::ranges::stable_sort(ordered, std::less{}, [](const Json* segment) {
            const auto& start = member(*segment, "start_s");
            return start.is_number() ? start.get<double>() : 0.0;
        });
        for (const auto* segment : ordered) {
            auto stanza = text(*segment, "text");
            if (stanza.empty()) {
                continue;
            }
            if (stanza.size() + transcript.size() + 1 > max_preview) {
                transcript += "\n…";
                break;
            }
            if (!transcript.empty()) {
                transcript += '\n';
            }
            transcript += stanza;
        }
    }
    preview["transcript"] = std::move(transcript);
    return preview;
}

std::optional<Json> RecordSync::remote_item(std::string_view kind, std::string_view id) {
    std::lock_guard lock(state_mutex_);
    const auto found = remote_items_.find(id);
    return kind == kKind && found != remote_items_.end() ? std::make_optional(found->second)
                                                         : std::nullopt;
}

bool RecordSync::delete_remote(std::string_view kind, std::string_view id) {
    if (kind != kKind) {
        return false;
    }
    const auto validated = safe_record_id(Json(id));
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    const auto peer = require_pairing();
    auto connection = open(peer);
    const auto result = connection->request("records_delete", fields(validated), 60s);
    outbox_.forget(kind, id, peer.fingerprint);
    {
        std::lock_guard lock(state_mutex_);
        remote_items_.erase(validated);
        audio_.erase(validated);
    }
    changed.emit("records");
    return member(result, "deleted") == true;
}

Json RecordSync::fetch(remote::RemoteConnection& connection, std::string_view id,
                       const fs::path& directory,
                       const std::function<void(std::int64_t, std::int64_t)>& progress,
                       std::string_view kind) {
    const Json key{{"kind", kind}, {"record_id", id}};
    const auto opened =
        connection.request_timed("records_open", key, 120s, {}, {}, stopping_.token()).first;
    const auto& files = member(opened, "files");
    const auto total = bytes(member(opened, "bytes"));
    if (!files.is_array() || !total || files.size() > 50'000) {
        throw remote::RecordsUnavailable("The host sent an invalid record file list.");
    }
    std::uint64_t got = 0;
    for (const auto& file : files) {
        const auto name = safe_name(member(file, "name"));
        const auto size = bytes(member(file, "size"));
        if (!size || *size > *total - got) {
            throw remote::RecordsUnavailable("The host sent an invalid record file size.");
        }
        const auto path = directory / path_from_utf8(name);
        if (fs::exists(path)) {
            throw remote::RecordsUnavailable("The host sent conflicting record file names.");
        }
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        auto next = key;
        next["name"] = name;
        std::uint64_t offset = 0;
        while (offset < *size) {
            next["offset"] = offset;
            const auto chunk =
                connection.request_timed("records_fetch", next, 120s, {}, {}, stopping_.token())
                    .first;
            const auto data = b64decode(text(chunk, "data"));
            if (!data || data->size() > remote::RECORD_CHUNK_BYTES ||
                data->size() > *size - offset || bytes(member(chunk, "offset")) != offset) {
                throw remote::RecordsUnavailable("The host returned an incomplete record.");
            }
            if (data->empty()) {
                break;
            }
            output.write(reinterpret_cast<const char*>(data->data()),
                         static_cast<std::streamsize>(data->size()));
            offset += data->size();
            got += data->size();
            if (progress) {
                progress(static_cast<std::int64_t>(got), static_cast<std::int64_t>(*total));
            }
            if (member(chunk, "eof") == true) {
                break;
            }
        }
        output.close();
        if (offset != *size) {
            // sync.py _fetch: the host ended a file short of the size it listed.
            throw remote::RecordsUnavailable(name + " came back incomplete.");
        }
    }
    if (got != *total) {
        throw remote::RecordsUnavailable("The host returned an incomplete record.");
    }
    return read_record(directory, kind);
}

void RecordSync::restore_local(std::string_view id, const fs::path& directory, const Json& record) {
    deps_.db.write([&](sql::Connection&) {
        if (!entries_.get_history_entry_by_id(id, HistoryOrigin::own())) {
            adapter_->import_record(id, directory, record, {});
            return;
        }
        // A copied record may have been edited after it was sent. Preserve both copies
        // unless their content still matches or the local one only lacks its recording.
        TemporaryDirectory local(deps_.transfer_root);
        const auto files = adapter_->export_record("", id, local.path);
        const auto saved = read_record(local.path);
        const auto local_audio = text(saved, "audio");
        const auto remote_audio = text(record, "audio");
        py::DumpOptions sorted;
        sorted.sort_keys = true;
        if (py::dumps(member(saved, "entry"), sorted) !=
                py::dumps(member(record, "entry"), sorted) ||
            py::dumps(member(saved, "entry_ext"), sorted) !=
                py::dumps(member(record, "entry_ext"), sorted)) {
            throw remote::RecordsUnavailable(
                "A different version of this dictation is already here. Both copies were kept.");
        }
        if (!local_audio.empty() && !remote_audio.empty() &&
            download::sha256_file(files.at(local_audio), true) !=
                download::sha256_file(directory / path_from_utf8(safe_name(Json(remote_audio))),
                                      true)) {
            throw remote::RecordsUnavailable("This dictation has different recordings on the two "
                                             "computers. Both copies were kept.");
        }
        if (local_audio.empty() && !remote_audio.empty()) {
            adapter_->import_record(id, directory, record, {});
        }
    });
}

void RecordSync::check_out(std::string_view kind, std::string_view id,
                           std::function<void(std::int64_t, std::int64_t)> progress) {
    safe_kind(Json(kind));
    if (kind != kKind) {
        throw remote::RecordsUnavailable("This record kind is unavailable.");
    }
    const auto validated = safe_record_id(Json(id));
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    const auto peer = require_pairing();
    auto connection = open(peer);
    TemporaryDirectory directory(deps_.transfer_root);
    const auto record = fetch(*connection, validated, directory.path, progress);
    require_same_peer(peer);
    restore_local(validated, directory.path, record);
    // Kept on both computers now: marked done, so History shows it as also on the host and
    // Copy or Move existing records doesn't send it back unchanged. Nothing sends an edit of it
    // or drops it at the next start on its own (a17-retention).
    std::string content = "fetched";
    try {
        TemporaryDirectory local(deps_.transfer_root);
        const auto files = adapter_->export_record("", validated, local.path);
        content = digest(local.path / "record.json", files);
    } catch (const std::exception& error) {
        (void)error; // sync.py: a record whose digest can't be taken is marked "fetched".
    }
    outbox_.mark_done({std::string(kKind), validated, peer.fingerprint,
                       location() == "both" ? "copy" : "move", "done", content});
    changed.emit("records");
}

void RecordSync::check_out_from(std::string_view kind, std::string_view id,
                                std::string_view expected_host,
                                std::function<void(std::int64_t, std::int64_t)> progress) {
    if (kind != "meeting") {
        throw remote::RecordsUnavailable("This record kind is unavailable.");
    }
    const auto peer = require_pairing();
    if (expected_host.empty() || peer.fingerprint != expected_host) {
        throw remote::RecordsUnavailable("The paired host changed. Choose the meeting again.");
    }
    const auto validated = safe_record_id(Json(id));
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    auto connection = open(peer);
    TemporaryDirectory directory(deps_.transfer_root);
    const auto record = fetch(*connection, validated, directory.path, progress, kind);
    require_same_peer(peer);
    import_meeting_for_viewing(deps_.meetings, deps_.db, deps_.clock, validated, directory.path,
                               record);
    changed.emit("records");
}

void RecordSync::delete_remote_version(std::string_view kind, std::string_view id,
                                       std::string_view expected_host, std::string_view revision) {
    if (kind != "meeting" || revision.empty()) {
        throw remote::RecordsUnavailable("Choose a meeting copy whose version can be verified.");
    }
    const auto peer = require_pairing();
    if (expected_host.empty() || peer.fingerprint != expected_host) {
        throw remote::RecordsUnavailable("The paired host changed. Choose the meeting again.");
    }
    const auto validated = safe_record_id(Json(id));
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    auto connection = open(peer);
    require_same_peer(peer);
    const auto result =
        connection
            ->request_timed("records_delete_version",
                            Json{{"kind", kind}, {"record_id", validated}, {"revision", revision}},
                            60s, {}, {}, stopping_.token())
            .first;
    if (member(result, "deleted") != true) {
        throw remote::RecordsUnavailable(
            "The selected host copy is no longer there. Refresh the list.");
    }
    changed.emit("records");
}

bool RecordSync::delete_remote_from(std::string_view kind, std::string_view id,
                                    std::string_view expected_host) {
    if (kind != "meeting") {
        throw remote::RecordsUnavailable("This record kind is unavailable.");
    }
    const auto peer = require_pairing();
    if (expected_host.empty() || peer.fingerprint != expected_host) {
        throw remote::RecordsUnavailable("The paired host changed. Choose the meeting again.");
    }
    const auto validated = safe_record_id(Json(id));
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    auto connection = open(peer);
    require_same_peer(peer);
    const auto result =
        connection
            ->request_timed("records_delete", Json{{"kind", kind}, {"record_id", validated}}, 60s,
                            {}, {}, stopping_.token())
            .first;
    changed.emit("records");
    return member(result, "deleted") == true;
}

std::filesystem::path RecordSync::audio_for(std::string_view id) {
    const auto validated = safe_record_id(Json(id));
    const auto peer = require_pairing();
    const auto cached = [&]() -> std::optional<fs::path> {
        std::lock_guard lock(state_mutex_);
        const auto at = audio_.find(validated);
        std::error_code problem;
        if (observed_host_ == peer.fingerprint && at != audio_.end() &&
            fs::is_regular_file(at->second, problem)) {
            return at->second;
        }
        return std::nullopt;
    };
    if (const auto local = cached()) {
        return *local;
    }
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    require_same_peer(peer);
    // Another caller may have finished the download while this one waited.
    if (const auto local = cached()) {
        return *local;
    }
    auto connection = open(peer);
    TemporaryDirectory directory(deps_.transfer_root);
    const auto record = fetch(*connection, validated, directory.path, {});
    require_same_peer(peer);
    const auto audio = text(record, "audio");
    if (audio.empty()) {
        throw remote::RecordsUnavailable("That entry has no recording on the host.");
    }
    const auto path = directory.path / path_from_utf8(safe_name(Json(audio)));
    if (!fs::is_regular_file(path)) {
        throw remote::RecordsUnavailable("The host sent an incomplete recording.");
    }
    fs::create_directories(audio_cache_dir_);
    const auto local = audio_cache_dir_ / (uuid4_hex() + ".wav");
    fs::rename(path, local);
    {
        std::lock_guard lock(state_mutex_);
        if (observed_host_ == peer.fingerprint) {
            audio_[validated] = local;
        }
    }
    return local;
}

int RecordSync::bring_back(std::function<void(std::string)> progress) {
    // Dictations only: meetings are shared as copies and never leave this computer
    // (a17-retention), so the host holds none that belong back here.
    const auto work = gate_.request();
    std::lock_guard serial(transfers_);
    const auto peer = require_pairing();
    auto connection = open(peer);
    int moved = 0;
    std::set<std::string> skipped;
    std::string last_problem;
    while (!stopping_.cancelled()) {
        std::vector<Json> batch;
        for (auto& item : list(*connection, peer, {}, 100)) {
            if (!skipped.contains(text(item, "id"))) {
                batch.push_back(std::move(item));
            }
        }
        if (batch.empty()) {
            break;
        }
        for (const auto& item : batch) {
            const auto id = safe_record_id(member(item, "id"));
            if (progress) {
                progress(std::format("Bringing back {}…", moved + 1));
            }
            try {
                TemporaryDirectory directory(deps_.transfer_root);
                const auto record = fetch(*connection, id, directory.path, {});
                require_same_peer(peer);
                restore_local(id, directory.path, record);
            } catch (const remote::RecordsUnavailable& error) {
                // sync.py: a record that can't come home stays on the host, and so does anything
                // older than it (the host lists its newest first, without paging).
                skipped.insert(id);
                last_problem = error.what();
                continue;
            }
            if (member(connection->request("records_delete", fields(id), 60s), "deleted") != true) {
                throw remote::RecordsUnavailable(
                    "The record was saved here. Its host copy remains.");
            }
            outbox_.forget(kKind, id, peer.fingerprint);
            {
                std::lock_guard lock(state_mutex_);
                remote_items_.erase(id);
                audio_.erase(id);
            }
            ++moved;
        }
    }
    problem(last_problem);
    (void)open(peer);
    changed.emit("records");
    return moved;
}

std::set<std::string> RecordSync::copies_on_host(std::string_view kind,
                                                 std::span<const std::string> ids) {
    std::set<std::string> result;
    if (const auto peer = deps_.pairing(); peer) {
        for (const auto& item : outbox_.rows(peer->fingerprint)) {
            if (item.kind == kind && item.state == "done" &&
                std::ranges::find(ids, item.id) != ids.end()) {
                result.insert(item.id);
            }
        }
    }
    return result;
}

std::vector<std::string> RecordSync::copies_on_host(std::string_view kind,
                                                    const std::vector<std::string>& ids) {
    const auto found = copies_on_host(kind, std::span(ids));
    return {found.begin(), found.end()};
}

bool RecordSync::listing_wanted(std::string_view kind) {
    if ((kind != kKind && kind != "meeting") || stopping_.cancelled()) {
        return false;
    }
    const auto peer = deps_.pairing();
    if (!peer) {
        return false;
    }
    const bool local = location() == "local";
    std::lock_guard lock(state_mutex_);
    if (observed_host_ != peer->fingerprint || !state_.host_supports) {
        return true;
    }
    const auto count = state_.stored.find(std::string(kind));
    return *state_.host_supports &&
           (!local || (count != state_.stored.end() && count->second.count > 0));
}

std::string RecordSync::host_name() {
    return status().host_name;
}

HistoryEntry RecordSync::remote_history_entry(const Json& item) {
    HistoryEntry entry;
    entry.id = safe_record_id(member(item, "id"));
    entry.text = text(item, "text");
    entry.model = text(item, "model");
    entry.timestamp = text(item, "timestamp");
    const auto optional_text = [&](const char* key) -> std::optional<std::string> {
        const auto& value = member(item, key);
        return value.is_string() ? std::make_optional(value.get<std::string>()) : std::nullopt;
    };
    const auto optional_seconds = [&](const char* key) -> std::optional<double> {
        const auto& value = member(item, key);
        if (value.is_number()) {
            const auto seconds = value.get<double>();
            if (std::isfinite(seconds)) {
                return seconds;
            }
        }
        return std::nullopt;
    };
    entry.raw_text = optional_text("raw_text");
    entry.source_name = optional_text("source_name");
    entry.title = optional_text("title");
    entry.cleanup_provider = optional_text("cleanup_provider");
    entry.cleanup_model = optional_text("cleanup_model");
    entry.language = optional_text("language");
    entry.cleaned_text = optional_text("cleaned_text");
    entry.cleanup_level = optional_text("cleanup_level");
    entry.entry_kind = optional_text("entry_kind");
    entry.transcription_time = optional_seconds("transcription_time");
    entry.audio_duration = optional_seconds("audio_duration");
    if (const auto size = bytes(member(item, "file_size"))) {
        entry.file_size = static_cast<std::int64_t>(*size);
    }
    const auto host = text(item, "stored_on");
    entry.stored_on = host.empty() ? "the host" : host;
    entry.remote_audio = member(item, "has_audio") == true;
    return entry;
}

remote::SyncPause RecordSync::paused_for_backup() {
    return remote::SyncPause([self = shared_from_this(), hold = std::make_shared<RecordGate::Pause>(
                                                             gate_.paused())]() mutable {
        hold.reset();
        self.reset();
    });
}

} // namespace fv::remote_records
