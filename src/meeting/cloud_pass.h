#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "meeting/cloud_audio.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"

namespace fv::meeting {

inline constexpr std::string_view kDiarizeModel = "gpt-4o-transcribe-diarize";
inline constexpr std::size_t kMaxKnownSpeakers = 4;

struct DiarizeRequest {
    std::string audio;
    std::string filename;
    std::string content_type;
    std::vector<std::string> known_speaker_names;
    std::vector<std::string> known_speaker_references;
};

using DiarizeFn = std::function<Json(const DiarizeRequest&, const CancelToken&)>;
using SpeakerProgress = std::function<void(const std::string&, int, int)>;
using SpeakerGroups = std::vector<std::pair<std::string, std::vector<std::string>>>;
using NamedSpeakers = std::vector<std::pair<std::string, std::string>>;
using SpeakerMap = std::vector<std::pair<std::string, std::optional<std::string>>>;

double overlap_duration(double start_a, double end_a, double start_b, double end_b);
NamedSpeakers map_turns_to_segments(const std::vector<Json>& segments,
                                    const std::vector<Json>& turns);
SpeakerMap resolve_speaker_participants(const SpeakerGroups& speakers,
                                        const std::map<std::string, Json, std::less<>>& by_id,
                                        const NamedSpeakers& named = {});
std::string speaker_slug(std::string_view name);
std::vector<std::pair<Json, Json>> pick_reference_segments(const std::vector<Json>& participants,
                                                           const std::vector<Json>& segments);
std::vector<Json> parse_diarized_result(const Json& result);
std::vector<Json> project_window_turns(const Json& turns, double origin, double duration);

struct SpeakerUploadOffer {
    std::string meeting_id;
    std::string title;
    std::string started_at;
    std::filesystem::path spool_dir;
    std::string source_stamp;
    bool from_chunks = false;
    bool operator==(const SpeakerUploadOffer&) const = default;
};

SpeakerUploadOffer describe_speaker_upload(meeting_state::MeetingRepository& repo,
                                           const std::string& meeting_id,
                                           const std::filesystem::path& recordings_root,
                                           std::optional<bool> from_chunks = std::nullopt);

struct CloudSpeakerOptions {
    DiarizeFn diarize;
    cloud_audio::Encoder encode;
    std::string extension = "flac";
    std::string content_type = "audio/flac";
    std::int64_t byte_budget = cloud_audio::kDefaultByteBudget;
    std::optional<bool> from_chunks;
    std::function<bool()> still_approved;
};

// Loads only the consented spool's system audio and changes speaker labels through the store.
Json run_cloud_speaker_pass(meeting_state::MeetingRepository& repo, const std::string& meeting_id,
                            meeting_state::MeetingStateStore& store,
                            const std::filesystem::path& spool_dir,
                            const CloudSpeakerOptions& options,
                            const SpeakerProgress& progress = {}, const CancelToken& cancel = {});

} // namespace fv::meeting
