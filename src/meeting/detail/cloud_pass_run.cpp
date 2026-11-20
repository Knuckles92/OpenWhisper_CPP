#include "meeting/cloud_pass.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "audio/file_decoder.h"
#include "base/hash.h"
#include "base/ids.h"
#include "base/utf8.h"
#include "meeting/detail/py_value.h"
#include "meeting_capture/session_audio.h"
#include "speech/api_models.h"

namespace fv::meeting {
namespace {

constexpr std::string_view kLoopback = "loopback";
constexpr std::size_t kReassignBatch = 40;

std::string text(const Json& row, std::string_view key) {
    return detail::str_or_empty(detail::get(row, key));
}

double seconds(const Json& row, std::string_view key) {
    const auto& value = detail::get(row, key);
    return value.is_number() ? value.get<double>() : 0;
}

Json failed(std::string_view error, std::size_t windows = 0, bool retired = false) {
    Json result{
        {"ok", false}, {"applied", 0}, {"created", 0}, {"windows", windows}, {"error", error}};
    if (retired) {
        result["retired"] = true;
    }
    return result;
}

SpeakerGroups group_speakers(const NamedSpeakers& by_segment) {
    SpeakerGroups speakers;
    for (const auto& [id, speaker] : by_segment) {
        auto found = std::ranges::find(speakers, speaker, &SpeakerGroups::value_type::first);
        if (found == speakers.end()) {
            speakers.emplace_back(speaker, std::vector<std::string>{id});
        } else {
            found->second.push_back(id);
        }
    }
    return speakers;
}

std::optional<std::string> add_speaker(meeting_state::MeetingStateStore& store,
                                       const std::string& label) {
    const auto result = store.apply("system", std::string("diarizer"),
                                    Json::array({Json{{"op", "upsert_participant"},
                                                      {"display_name", label},
                                                      {"kind", "others_cluster"},
                                                      {"is_provisional", true}}}));
    if (!result.empty() && result.front().ok) {
        const auto id = text(detail::get(result.front().effect, "participant"), "id");
        if (!id.empty()) {
            return id;
        }
    }
    return std::nullopt;
}

struct Reference {
    std::string alias;
    std::string clip;
    std::string identity;
};

std::optional<std::string> clip_for(std::span<const std::int16_t> frames, int rate, double origin,
                                    const Json& row, const CloudSpeakerOptions& options) {
    if (frames.size() < static_cast<std::size_t>(2 * rate)) {
        return std::nullopt;
    }
    return cloud_audio::cut_reference_clip(frames, rate, seconds(row, "start_s"),
                                           seconds(row, "end_s"), origin, options.encode,
                                           options.content_type);
}

void add_reference(std::vector<Reference>& references, const std::string& identity, const Json& row,
                   std::span<const std::int16_t> frames, int rate, double origin,
                   const CloudSpeakerOptions& options) {
    if (references.size() == kMaxKnownSpeakers ||
        std::ranges::any_of(references,
                            [&](const auto& item) { return item.identity == identity; })) {
        return;
    }
    if (auto clip = clip_for(frames, rate, origin, row, options)) {
        references.push_back(
            {std::format("voice_{}", references.size() + 1), std::move(*clip), identity});
    }
}

using SegmentOwners = std::map<std::string, std::string, std::less<>>;

void merge_window(const SpeakerGroups& groups, std::size_t window,
                  const std::vector<Reference>& references, SegmentOwners& assignments) {
    std::set<std::string> claimed;
    for (const auto& [speaker, ids] : groups) {
        const auto ref = std::ranges::find(references, speaker, &Reference::alias);
        if (ref != references.end()) {
            claimed.insert(ref->identity);
            for (const auto& id : ids) {
                assignments[id] = ref->identity;
            }
        }
    }
    for (const auto& [speaker, ids] : groups) {
        if (std::ranges::find(references, speaker, &Reference::alias) != references.end()) {
            continue;
        }
        std::map<std::string, int, std::less<>> votes;
        for (const auto& id : ids) {
            if (const auto prior = assignments.find(id);
                prior != assignments.end() && !claimed.contains(prior->second)) {
                ++votes[prior->second];
            }
        }
        const auto choice =
            std::ranges::max_element(votes, {}, &decltype(votes)::value_type::second);
        const auto identity =
            choice == votes.end() ? std::format("window_{}_{}", window, speaker) : choice->first;
        claimed.insert(identity);
        for (const auto& id : ids) {
            assignments[id] = identity;
        }
    }
}

void add_anchors(std::vector<Reference>& references, const SegmentOwners& assignments,
                 const std::map<std::string, Json, std::less<>>& by_id,
                 std::span<const std::int16_t> frames, int rate, double origin,
                 const CloudSpeakerOptions& options) {
    if (references.size() == kMaxKnownSpeakers) {
        return;
    }
    std::vector<std::pair<std::string, const Json*>> candidates;
    for (const auto& [id, identity] : assignments) {
        candidates.emplace_back(identity, &by_id.at(id));
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return seconds(*a.second, "end_s") - seconds(*a.second, "start_s") >
               seconds(*b.second, "end_s") - seconds(*b.second, "start_s");
    });
    for (const auto& [identity, row] : candidates) {
        add_reference(references, identity, *row, frames, rate, origin, options);
        if (references.size() == kMaxKnownSpeakers) {
            break;
        }
    }
}

bool inside(const std::filesystem::path& child, const std::filesystem::path& root) {
    const auto relative = child.lexically_relative(root);
    return !relative.empty() && relative != "." && !relative.is_absolute() &&
           *relative.begin() != "..";
}

std::vector<Json> local_chunks(meeting_state::MeetingRepository& repo,
                               const std::string& meeting_id, const std::filesystem::path& spool) {
    std::vector<Json> accepted;
    for (auto row : repo.get_audio_chunks(meeting_id)) {
        if (text(row, "channel") != kLoopback) {
            continue;
        }
        const auto encoded = text(row, "file_path");
        if (encoded.empty()) {
            continue;
        }
        std::error_code ec;
        const auto stored = path_from_utf8(encoded);
        const auto path = std::filesystem::weakly_canonical(stored, ec);
        if (!ec && inside(path, spool) && path_to_utf8(path.filename()).starts_with("loopback_") &&
            !std::filesystem::is_symlink(stored) && std::filesystem::is_regular_file(path) &&
            std::filesystem::file_size(path) > 44) {
            row["file_path"] = path_to_utf8(path);
            accepted.push_back(std::move(row));
        }
    }
    return accepted;
}

struct TemporaryWav {
    std::filesystem::path path;
    ~TemporaryWav() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

std::tuple<std::optional<std::vector<std::int16_t>>, int, double>
load_approved_audio(const std::filesystem::path& spool, const std::vector<Json>& chunks,
                    std::optional<bool> from_chunks) {
    if (!from_chunks.has_value()) {
        return load_channel_session(spool, kLoopback, chunks);
    }
    TemporaryWav temporary;
    auto source = session_wav_path(spool, kLoopback);
    double origin = session_origin_s(spool, kLoopback);
    if (*from_chunks) {
        temporary.path = spool / path_from_utf8("loopback_approval_" + random_hex(12) + ".wav");
        source = temporary.path;
        const auto reconstructed = concat_channel_chunks_to_wav(chunks, kLoopback, source);
        if (!reconstructed) {
            return {std::nullopt, 16000, 0};
        }
        origin = *reconstructed;
    } else if (!std::filesystem::is_regular_file(source) || std::filesystem::is_symlink(source)) {
        return {std::nullopt, 16000, 0};
    }
    audio::AudioFileDecoder decoder(source);
    if (decoder.channels() != 1 || decoder.rate() != 16000) {
        return {std::nullopt, 16000, 0};
    }
    std::vector<std::int16_t> samples;
    while (decoder.read_s16(samples)) {
    }
    return {std::move(samples), decoder.rate(), origin};
}

} // namespace

SpeakerUploadOffer describe_speaker_upload(meeting_state::MeetingRepository& repo,
                                           const std::string& meeting_id,
                                           const std::filesystem::path& recordings_root,
                                           std::optional<bool> requested_chunks) {
    const auto meeting = repo.get_meeting(meeting_id);
    if (!meeting || text(*meeting, "status") != "ended") {
        throw std::runtime_error("Choose a saved meeting that has ended.");
    }
    const auto stored_spool = text(*meeting, "spool_dir");
    if (recordings_root.empty() || stored_spool.empty()) {
        throw std::runtime_error("No saved system-audio recording is available.");
    }
    const auto root = std::filesystem::weakly_canonical(recordings_root);
    const auto spool = std::filesystem::weakly_canonical(path_from_utf8(stored_spool));
    const auto expected =
        std::filesystem::weakly_canonical(root / path_from_utf8(meeting_id.substr(0, 12)));
    if (!inside(spool, root) || spool != expected || !std::filesystem::is_directory(spool) ||
        std::filesystem::is_symlink(path_from_utf8(stored_spool))) {
        throw std::runtime_error("The recording is no longer in this meeting's local folder.");
    }
    const auto wav = session_wav_path(spool, kLoopback);
    std::vector<std::filesystem::path> sources;
    if (std::filesystem::is_symlink(wav)) {
        throw std::runtime_error("The saved system-audio recording is no longer local.");
    }
    const bool has_session =
        std::filesystem::is_regular_file(wav) && std::filesystem::file_size(wav) > 44;
    const bool from_chunks = requested_chunks.value_or(!has_session);
    Sha256 stamp;
    if (!from_chunks && has_session) {
        sources.push_back(wav);
        const auto meta = load_session_meta(session_meta_path(spool, kLoopback));
        if (meta) {
            stamp.update(std::format("origin={}\n", meta->origin_s));
        }
    } else if (from_chunks) {
        for (const auto& row : local_chunks(repo, meeting_id, spool)) {
            const auto chunk = path_from_utf8(text(row, "file_path"));
            if (std::filesystem::is_regular_file(chunk) && std::filesystem::file_size(chunk) > 44) {
                sources.push_back(chunk);
                stamp.update(std::format("chunk:{}:{}\n", seconds(row, "start_s"),
                                         seconds(row, "duration_s")));
            }
        }
    }
    if (sources.empty()) {
        throw std::runtime_error("No saved system-audio recording is available.");
    }
    for (const auto& source : sources) {
        const auto size = std::filesystem::file_size(source);
        const auto written = std::filesystem::last_write_time(source).time_since_epoch().count();
        stamp.update(std::format("{}\n{}\n{}\n", path_to_utf8(source), size, written));
    }
    if (!requested_chunks.has_value()) {
        const auto rows = repo.get_segments(meeting_id, -1.0, std::nullopt);
        if (std::ranges::none_of(
                rows, [](const auto& row) { return text(row, "channel") == kLoopback; })) {
            throw std::runtime_error("This meeting has no saved system-audio transcript to label.");
        }
    }
    return {meeting_id, text(*meeting, "title"), text(*meeting, "started_at"),
            spool,      stamp.hexdigest(),       from_chunks};
}

Json run_cloud_speaker_pass(meeting_state::MeetingRepository& repo, const std::string& meeting_id,
                            meeting_state::MeetingStateStore& store,
                            const std::filesystem::path& spool_dir,
                            const CloudSpeakerOptions& options, const SpeakerProgress& progress,
                            const CancelToken& cancel) {
    const auto spool = std::filesystem::weakly_canonical(spool_dir);
    if (spool_dir.empty() || !std::filesystem::is_directory(spool)) {
        return failed("No system-audio recording is available.");
    }
    if (!options.encode || !options.diarize) {
        return failed("Speaker identification is unavailable.");
    }
    const auto wav = session_wav_path(spool, kLoopback);
    if (std::filesystem::is_symlink(wav) ||
        (std::filesystem::exists(wav) && !inside(std::filesystem::weakly_canonical(wav), spool))) {
        return failed("The system-audio recording is outside its meeting folder.");
    }
    if (cancel.cancelled()) {
        return failed("Speaker identification was canceled.");
    }
    const auto approved = [&] {
        return !options.still_approved || options.still_approved();
    };
    if (!approved()) {
        return failed("The recording changed. Review and approve another upload.");
    }
    const auto chunks = local_chunks(repo, meeting_id, spool);
    const auto [audio, rate, origin] = load_approved_audio(spool, chunks, options.from_chunks);
    if (!audio || audio->empty() || rate <= 0) {
        return failed("No system-audio recording is available.");
    }
    auto rows = repo.get_segments(meeting_id, -1.0, std::nullopt);
    std::erase_if(rows, [](const Json& row) { return text(row, "channel") != kLoopback; });
    if (rows.empty()) {
        return failed("No system-audio transcript segments to label.");
    }
    std::map<std::string, Json, std::less<>> by_id;
    for (const auto& row : rows) {
        const auto id = text(row, "id");
        if (!id.empty()) {
            by_id.emplace(id, row);
        }
    }
    const auto windows = cloud_audio::plan_windows(static_cast<std::int64_t>(audio->size()), rate,
                                                   options.byte_budget, 16 * rate);
    const std::span<const std::int16_t> frames(*audio);
    std::vector<Json> participants;
    const auto snapshot = store.snapshot();
    for (const auto& entry : snapshot.at("participants")) {
        participants.push_back(entry);
    }
    std::vector<Reference> references;
    NamedSpeakers named;
    for (const auto& [participant, row] : pick_reference_segments(participants, rows)) {
        const auto id = text(participant, "id");
        const auto identity = "named_" + id;
        add_reference(references, identity, row, frames, rate, origin, options);
        if (!references.empty() && references.back().identity == identity) {
            named.emplace_back(identity, id);
        }
    }
    SegmentOwners assignments;
    for (std::size_t index = 0; index < windows.size(); ++index) {
        if (cancel.cancelled()) {
            return failed("Speaker identification was canceled.", windows.size());
        }
        if (progress) {
            try {
                progress(
                    std::format("Identifying speakers in window {}/{}", index + 1, windows.size()),
                    static_cast<int>(index + 1), static_cast<int>(windows.size()));
            } catch (const std::exception&) {
                // The optional listener cannot interrupt the run.
            }
        }
        const auto [start, end] = windows[index];
        const auto samples =
            frames.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start));
        try {
            DiarizeRequest request;
            request.audio = options.encode(samples, rate);
            request.filename = "system-audio." + options.extension;
            request.content_type = options.content_type;
            if (request.audio.empty() || options.byte_budget <= 0 ||
                request.audio.size() > static_cast<std::uint64_t>(options.byte_budget)) {
                return failed("A system-audio window could not be encoded within the upload limit.",
                              windows.size());
            }
            for (const auto& ref : references) {
                request.known_speaker_names.push_back(ref.alias);
                request.known_speaker_references.push_back(ref.clip);
            }
            if (cancel.cancelled()) {
                return failed("Speaker identification was canceled.", windows.size());
            }
            if (!approved()) {
                return failed("The recording changed. Review and approve another upload.",
                              windows.size());
            }
            const auto turns = options.diarize(request, cancel);
            const auto projected =
                project_window_turns(turns, origin + static_cast<double>(start) / rate,
                                     static_cast<double>(samples.size()) / rate);
            merge_window(group_speakers(map_turns_to_segments(rows, projected)), index, references,
                         assignments);
            if (index + 1 < windows.size()) {
                add_anchors(references, assignments, by_id, frames, rate, origin, options);
            }
        } catch (const std::exception& error) {
            const bool retired = speech::is_model_gone_error(error);
            return failed(retired              ? speech::kSpeakerModelRetiredMessage
                          : cancel.cancelled() ? "Speaker identification was canceled."
                                               : "OpenAI speaker identification could not finish.",
                          windows.size(), retired);
        }
    }
    if (cancel.cancelled()) {
        return failed("Speaker identification was canceled.", windows.size());
    }
    if (!approved()) {
        return failed("The recording changed. Review and approve another upload.", windows.size());
    }
    const NamedSpeakers assigned(assignments.begin(), assignments.end());
    auto labels = resolve_speaker_participants(group_speakers(assigned), by_id, named);
    std::set<std::string> used;
    for (const auto& participant : participants) {
        used.insert(text(participant, "display_name"));
    }
    int created = 0;
    int next = 1;
    for (auto& [speaker, id] : labels) {
        if (id) {
            continue;
        }
        for (;;) {
            const auto label = std::format("Speaker {}", next++);
            if (used.insert(label).second) {
                id = add_speaker(store, label);
                created += id.has_value() ? 1 : 0;
                break;
            }
        }
    }
    int applied = 0;
    Json batch = Json::array();
    const auto flush = [&] {
        for (const auto& result : store.apply("system", std::string("diarizer"), batch)) {
            if (result.ok) {
                ++applied;
            }
        }
        batch = Json::array();
    };
    for (const auto& [id, speaker] : assigned) {
        const auto found = std::ranges::find(labels, speaker, &SpeakerMap::value_type::first);
        if (found != labels.end() && found->second &&
            text(by_id.at(id), "speaker_participant_id") != *found->second) {
            batch.push_back(Json{{"op", "reassign_segment_speaker"},
                                 {"segment_id", id},
                                 {"participant_id", *found->second}});
            if (batch.size() >= kReassignBatch) {
                flush();
            }
        }
    }
    if (!batch.empty()) {
        flush();
    }
    return Json{{"ok", true},
                {"applied", applied},
                {"created", created},
                {"windows", windows.size()},
                {"error", Json()}};
}

} // namespace fv::meeting
