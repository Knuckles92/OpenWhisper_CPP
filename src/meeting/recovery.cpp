#include "meeting/recovery.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>

#include "base/log.h"
#include "base/utf8.h"
#include "meeting/detail/py_value.h"
#include "meeting_capture/session_audio.h"
#include "meeting_capture/spool.h"
#include "meeting_state/schema.h"
#include "meeting_state/stored.h"
#include "meeting_state/time_utils.h"
#include "meeting_state/types.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.recovery");

constexpr std::string_view kSidecarSuffix = ".recovery.json";
constexpr std::string_view kOrphanSuffix = ".orphan";

using meeting_state::MeetingRepository;

const Clock& clock_of(const FinalizeOptions& options) {
    return options.clock != nullptr ? *options.clock : system_clock();
}

// float(value or 0.0), raising what Python's float() raises.
double float_or_zero(const Json& value) {
    if (!detail::truthy(value)) {
        return 0.0;
    }
    if (const auto converted = py::py_float(value)) {
        return *converted;
    }
    if (value.is_string()) {
        throw py::ValueError("could not convert string to float");
    }
    throw py::TypeError("float() argument must be a string or a real number");
}

std::string channel_of(const Json& row) {
    return detail::str_or_empty(detail::get(row, "channel"));
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool is_directory(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_directory(path_from_utf8(path), error);
}

std::uint32_t le32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t le16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

// The fields of a WAV header Python's wave.open reads: nullopt where it raises wave.Error,
// EOFError or OSError.
struct WavHeader {
    int channels = 0;
    int sample_width = 0;
    std::uint32_t rate = 0;
    std::int64_t frames = 0;
};

std::optional<WavHeader> read_wav_header(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::array<unsigned char, 12> riff{};
    if (!in.read(reinterpret_cast<char*>(riff.data()), riff.size())) {
        return std::nullopt;
    }
    if (std::memcmp(riff.data(), "RIFF", 4) != 0 || std::memcmp(riff.data() + 8, "WAVE", 4) != 0) {
        return std::nullopt;
    }
    // Sub-chunks are read inside the RIFF chunk: its declared size bounds them.
    const std::int64_t riff_end = 8 + static_cast<std::int64_t>(le32(riff.data() + 4));
    std::int64_t at = 12;
    std::optional<WavHeader> header;
    bool fmt_read = false;
    while (at + 8 <= riff_end) {
        std::array<unsigned char, 8> chunk{};
        in.seekg(at);
        if (!in.read(reinterpret_cast<char*>(chunk.data()), chunk.size())) {
            break;
        }
        const std::uint32_t size = le32(chunk.data() + 4);
        const std::int64_t body = at + 8;
        if (std::memcmp(chunk.data(), "fmt ", 4) == 0) {
            std::array<unsigned char, 16> fmt{};
            if (size < 16 || !in.read(reinterpret_cast<char*>(fmt.data()), fmt.size())) {
                return std::nullopt;
            }
            const std::uint16_t format = le16(fmt.data());
            constexpr std::uint16_t kPcm = 0x0001;
            constexpr std::uint16_t kExtensible = 0xFFFE;
            if (format != kPcm && format != kExtensible) {
                return std::nullopt;
            }
            WavHeader parsed;
            parsed.channels = le16(fmt.data() + 2);
            parsed.rate = le32(fmt.data() + 4);
            parsed.sample_width = (le16(fmt.data() + 14) + 7) / 8;
            if (format == kExtensible) {
                std::array<unsigned char, 24> extension{};
                if (size < 40 ||
                    !in.read(reinterpret_cast<char*>(extension.data()), extension.size())) {
                    return std::nullopt;
                }
                // KSDATAFORMAT_SUBTYPE_PCM.
                constexpr std::array<unsigned char, 16> kSubtypePcm = {
                    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                if (std::memcmp(extension.data() + 8, kSubtypePcm.data(), kSubtypePcm.size()) !=
                    0) {
                    return std::nullopt;
                }
            }
            if (parsed.sample_width == 0 || parsed.channels == 0) {
                return std::nullopt;
            }
            header = parsed;
            fmt_read = true;
        } else if (std::memcmp(chunk.data(), "data", 4) == 0) {
            if (!fmt_read) {
                return std::nullopt;
            }
            header->frames = static_cast<std::int64_t>(size) /
                             (static_cast<std::int64_t>(header->channels) * header->sample_width);
            return header;
        }
        // Chunks are word-aligned: an odd size has a pad byte.
        at = body + static_cast<std::int64_t>(size) + (size & 1U);
    }
    return std::nullopt;
}

// _restore_orphan: the WAV to recover from, or nullopt when an orphan cannot be restored. The row
// may have committed just before the old process died, so the canonical path it expects is made
// available before the transaction marker goes.
std::optional<std::string> restore_orphan(const std::string& artifact, const std::string& orphan,
                                          const std::string& canonical) {
    if (artifact != orphan || path_exists(path_from_utf8(canonical))) {
        return artifact;
    }
    std::error_code error;
    std::filesystem::rename(path_from_utf8(orphan), path_from_utf8(canonical), error);
    if (error) {
        logger.error("Could not restore orphan WAV {}: {}", orphan, error.message());
        return std::nullopt;
    }
    return canonical;
}

// Sorted "*.recovery.json" paths of the spool (UTF-8), or nullopt when it cannot be read.
std::optional<std::vector<std::string>> recovery_sidecars(const std::string& spool_dir) {
    std::vector<std::string> sidecars;
    std::error_code error;
    const std::filesystem::path root = path_from_utf8(spool_dir);
    std::filesystem::directory_iterator it(root, error);
    if (error) {
        logger.error("Could not scan meeting spool {}: {}", spool_dir, error.message());
        return std::nullopt;
    }
    for (; it != std::filesystem::directory_iterator(); it.increment(error)) {
        if (error) {
            logger.error("Could not scan meeting spool {}: {}", spool_dir, error.message());
            return std::nullopt;
        }
        std::error_code type_error;
        if (!it->is_regular_file(type_error)) {
            continue;
        }
        const std::string name = path_to_utf8(it->path().filename());
        if (name.ends_with(kSidecarSuffix)) {
            sidecars.push_back(path_to_utf8(root / it->path().filename()));
        }
    }
    std::sort(sidecars.begin(), sidecars.end());
    return sidecars;
}

// The repository's idempotent recovery seam: (row id, whether this call created it).
std::pair<std::int64_t, bool> register_recovered_chunk(MeetingRepository& repository,
                                                       const Json& fields) {
    const meeting_state::ChunkRegistration registration =
        repository.register_chunk_if_missing(fields);
    return {registration.id, registration.created};
}

// 16-bit samples of a PCM file; 0 when it is missing or unreadable.
std::int64_t pcm_sample_count(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return 0;
    }
    return static_cast<std::int64_t>(size / 2);
}

Json registration_fields(const std::string& meeting_id, const std::string& channel,
                         std::int64_t seq, const std::string& file_path, double start_s,
                         double duration_s, int sample_rate) {
    Json fields = Json::object();
    fields["meeting_id"] = meeting_id;
    fields["channel"] = channel;
    fields["seq"] = seq;
    fields["file_path"] = file_path;
    fields["start_s"] = start_s;
    fields["duration_s"] = duration_s;
    fields["sample_rate"] = sample_rate;
    fields["asr_status"] = "pending";
    return fields;
}

Json with_id(std::int64_t id, const Json& fields) {
    Json row = Json::object();
    row["id"] = id;
    for (const auto& [key, value] : fields.items()) {
        row[key] = value;
    }
    return row;
}

std::string meeting_id_of(const Json& meeting) {
    return detail::str_or_empty(detail::get(meeting, "id"));
}

std::string text_of(const Json& value) {
    return value.is_string() ? value.get<std::string>() : detail::py_str(value);
}

// Commits each transcribed chunk and reports progress (_FinalizeProgress). Runs on the engine's
// worker as well as the caller's thread.
class FinalizeProgress {
public:
    FinalizeProgress(MeetingRepository& repository, std::string meeting_id, int total,
                     std::function<void(int, int)> on_progress)
        : repository_(repository), meeting_id_(std::move(meeting_id)), total_(total),
          on_progress_(std::move(on_progress)) {}

    void report() {
        const std::lock_guard lock(mutex_);
        report_locked();
    }

    void on_chunk_result(const meeting_state::SpooledChunk& chunk,
                         std::vector<meeting_state::TranscriptSegment> segments) {
        repository_.commit_chunk_transcription(meeting_id_, chunk.chunk_id, segments);
        const std::lock_guard lock(mutex_);
        ++done_;
        report_locked();
    }

private:
    void report_locked() {
        if (!on_progress_) {
            return;
        }
        try {
            on_progress_(done_, total_);
        } catch (const std::exception& error) {
            logger.error_with(error, "Finalize progress callback raised");
        }
    }

    MeetingRepository& repository_;
    const std::string meeting_id_;
    const int total_;
    std::function<void(int, int)> on_progress_;
    std::mutex mutex_;
    int done_ = 0;
};

// _finalize_language: "auto" (detect) or the stripped, lower-cased code.
std::string finalize_language(const std::string& asr_language) {
    std::string language =
        py::lower(py::strip(asr_language.empty() ? std::string("auto") : asr_language));
    return language.empty() ? std::string("auto") : language;
}

void stop_engine(MeetingAsrEngine* engine, const std::string& meeting_id) {
    if (engine == nullptr) {
        return;
    }
    try {
        engine->stop();
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("ASR stop failed after finalize of {}", meeting_id));
    }
}

bool complete_finalize(MeetingRepository& repository, const Json& meeting,
                       const std::string& meeting_id, bool drained, int total, const Clock& clock) {
    if (!drained) {
        logger.error("ASR drain timed out finalizing meeting {}", meeting_id);
        return false;
    }
    if (repository.count_unfinished_chunks(meeting_id) != 0) {
        logger.error("Unfinished chunks remain after finalizing {}", meeting_id);
        repository.update_meeting(meeting_id, Json{{"status", "needs_recovery"}});
        return false;
    }
    mark_ended(repository, meeting, clock);
    logger.info("Finalized interrupted meeting {} ({} chunk(s) transcribed)", meeting_id, total);
    return true;
}

} // namespace

std::optional<std::pair<int, double>> read_wav_duration(const std::filesystem::path& path) {
    const std::optional<WavHeader> header = read_wav_header(path);
    if (!header || header->channels != 1 || header->sample_width != 2) {
        return std::nullopt;
    }
    if (header->rate == 0 || header->frames <= 0) {
        return std::nullopt;
    }
    return std::pair<int, double>{static_cast<int>(header->rate),
                                  static_cast<double>(header->frames) /
                                      static_cast<double>(header->rate)};
}

std::vector<PcmArtifact> pcm_artifacts(const std::filesystem::path& spool_dir,
                                       std::string_view channel) {
    if (read_wav_duration(session_wav_path(spool_dir, channel))) {
        return {};
    }
    const std::optional<SessionMeta> meta =
        load_session_meta(session_meta_path(spool_dir, channel));
    if (!meta || meta->sample_rate <= 0) {
        return {};
    }
    const int rate = meta->sample_rate;
    const double global_origin = meta->origin_s;
    const std::filesystem::path prefix_path = session_16k_pcm_path(spool_dir, channel);
    const std::filesystem::path raw_path = session_pcm_path(spool_dir, channel);
    std::vector<PcmArtifact> artifacts;
    const std::int64_t prefix_samples = pcm_sample_count(prefix_path);
    if (prefix_samples > 0) {
        artifacts.push_back({prefix_path, TARGET_RATE, prefix_samples, global_origin});
    }
    const std::int64_t raw_samples = pcm_sample_count(raw_path);
    // A failed unlink after a successful prefix conversion can leave the old native source; new
    // metadata marks it inactive so the same audio is not registered twice.
    if (raw_samples > 0 && meta->pcm_active.value_or(true)) {
        std::optional<double> raw_origin;
        if (meta->pcm_origin_s) {
            raw_origin = *meta->pcm_origin_s;
        } else if (prefix_samples != 0) {
            logger.warning("Cannot place legacy raw PCM suffix for {}; leaving {} intact", channel,
                           path_to_utf8(raw_path));
        } else {
            raw_origin = global_origin;
        }
        if (raw_origin) {
            artifacts.push_back({raw_path, rate, raw_samples, *raw_origin});
        }
    }
    std::stable_sort(
        artifacts.begin(), artifacts.end(),
        [](const PcmArtifact& a, const PcmArtifact& b) { return a.origin_s < b.origin_s; });
    return artifacts;
}

std::int64_t recover_stranded_wavs(MeetingRepository& repository, const Json& meeting,
                                   std::vector<Json>& chunks) {
    const std::string meeting_id = meeting_id_of(meeting);
    const std::string spool_dir = detail::str_or_empty(detail::get(meeting, "spool_dir"));
    if (meeting_id.empty() || !is_directory(spool_dir)) {
        return 0;
    }
    std::set<std::pair<std::string, std::int64_t>> known;
    for (const Json& row : chunks) {
        known.emplace(channel_of(row), detail::int_or_zero(detail::get(row, "seq")));
    }
    const std::optional<std::vector<std::string>> sidecars = recovery_sidecars(spool_dir);
    if (!sidecars) {
        return 0;
    }
    std::int64_t recovered = 0;
    for (const std::string& sidecar : *sidecars) {
        const std::string canonical = sidecar.substr(0, sidecar.size() - kSidecarSuffix.size());
        const std::string orphan = canonical + std::string(kOrphanSuffix);
        std::string artifact = is_file(path_from_utf8(canonical)) ? canonical : orphan;
        const std::optional<ChunkRecoveryMeta> meta =
            load_chunk_recovery_meta(path_from_utf8(canonical));
        if (!meta || meta->meeting_id != meeting_id || meta->channel.empty() ||
            !is_file(path_from_utf8(artifact))) {
            logger.warning("Leaving unusable recovery artifact {} untouched", sidecar);
            continue;
        }
        const std::pair<std::string, std::int64_t> identity{meta->channel, meta->seq};

        const std::optional<std::string> restored = restore_orphan(artifact, orphan, canonical);
        if (!restored) {
            continue;
        }
        artifact = *restored;

        const auto wav_info = read_wav_duration(path_from_utf8(artifact));
        if (!wav_info) {
            logger.warning("Leaving invalid recovery WAV {} untouched", artifact);
            continue;
        }
        const auto [sample_rate, duration_s] = *wav_info;
        if (known.contains(identity)) {
            remove_chunk_recovery_meta(path_from_utf8(canonical));
            continue;
        }
        // The committed WAV header's duration wins over a sidecar written around a crash.
        const Json fields = registration_fields(meeting_id, meta->channel, meta->seq, canonical,
                                                meta->start_s, duration_s, sample_rate);
        std::pair<std::int64_t, bool> registration;
        try {
            registration = register_recovered_chunk(repository, fields);
        } catch (const std::exception& error) {
            logger.error_with(error, std::format("Could not register recovery WAV {}", canonical));
            continue;
        }
        known.insert(identity);
        chunks.push_back(with_id(registration.first, fields));
        remove_chunk_recovery_meta(path_from_utf8(canonical));
        if (registration.second) {
            ++recovered;
            logger.info("Recovered stranded meeting chunk {} ({} seq {})", registration.first,
                        meta->channel, meta->seq);
        }
    }
    return recovered;
}

std::int64_t recover_pcm_tails(MeetingRepository& repository, const Json& meeting,
                               std::vector<Json>& chunks) {
    const std::string meeting_id = meeting_id_of(meeting);
    const std::string spool_dir = detail::str_or_empty(detail::get(meeting, "spool_dir"));
    if (meeting_id.empty() || !is_directory(spool_dir)) {
        return 0;
    }
    const std::filesystem::path spool = path_from_utf8(spool_dir);
    std::set<std::string> channels;
    for (const std::string_view channel : meeting_state::kChannels) {
        channels.emplace(channel);
    }
    for (const Json& row : chunks) {
        channels.insert(channel_of(row));
    }
    channels.erase("");

    std::int64_t recovered = 0;
    for (const std::string& channel : channels) {
        double coverage_end = -std::numeric_limits<double>::infinity();
        std::int64_t last_seq = -1;
        for (const Json& row : chunks) {
            if (channel_of(row) != channel) {
                continue;
            }
            coverage_end = std::max(
                coverage_end, float_or_zero(detail::get(row, "start_s")) +
                                  std::max(0.0, float_or_zero(detail::get(row, "duration_s"))));
            last_seq = std::max(last_seq, detail::int_or_zero(detail::get(row, "seq")));
        }
        std::int64_t next_seq = last_seq + 1;

        for (const PcmArtifact& artifact : pcm_artifacts(spool, channel)) {
            const double rate = static_cast<double>(artifact.rate);
            const double artifact_end =
                artifact.origin_s + static_cast<double>(artifact.samples) / rate;
            const double cursor_s = std::max(artifact.origin_s, coverage_end);
            if (cursor_s >= artifact_end - (0.5 / rate)) {
                continue;
            }
            std::int64_t start_sample = std::max<std::int64_t>(
                0,
                static_cast<std::int64_t>(std::floor((cursor_s - artifact.origin_s) * rate + 0.5)));
            const auto max_frames = std::max<std::int64_t>(
                1, static_cast<std::int64_t>(py::round_half_even(RECOVERED_CHUNK_MAX_S * rate)));
            while (start_sample < artifact.samples) {
                const std::int64_t end_sample =
                    std::min(artifact.samples, start_sample + max_frames);
                const double start_s = artifact.origin_s + static_cast<double>(start_sample) / rate;
                const std::string file_path =
                    path_to_utf8(spool / path_from_utf8(std::format("{}_{:05d}.recovered.wav",
                                                                    channel, next_seq)));
                Json fields;
                double duration_s = 0.0;
                std::pair<std::int64_t, bool> registration;
                try {
                    const std::int64_t output_frames =
                        pcm_slice_to_wav(artifact.path, artifact.rate, start_sample, end_sample,
                                         path_from_utf8(file_path));
                    if (output_frames <= 0) {
                        break;
                    }
                    duration_s = static_cast<double>(output_frames) / TARGET_RATE;
                    // The sidecar also covers a crash of this pass between the file replace and
                    // the SQLite commit.
                    write_chunk_recovery_meta(path_from_utf8(file_path), meeting_id, channel,
                                              next_seq, start_s, duration_s, TARGET_RATE);
                    fields = registration_fields(meeting_id, channel, next_seq, file_path, start_s,
                                                 duration_s, TARGET_RATE);
                    registration = register_recovered_chunk(repository, fields);
                } catch (const std::exception& error) {
                    logger.error_with(
                        error,
                        std::format("Could not materialize raw PCM recovery tail for {}", channel));
                    // Not into later audio: the next startup consumes the sidecar or retries this
                    // interval.
                    break;
                }
                remove_chunk_recovery_meta(path_from_utf8(file_path));
                chunks.push_back(with_id(registration.first, fields));
                if (registration.second) {
                    ++recovered;
                    logger.info("Recovered {}s raw PCM tail as chunk {} ({})",
                                py::format_fixed(duration_s, 2), registration.first, channel);
                }
                coverage_end = std::max(coverage_end, start_s + duration_s);
                ++next_seq;
                start_sample = end_sample;
            }
        }
    }
    return recovered;
}

ReconcileResult reconcile_meeting_audio(MeetingRepository& repository, const Json& meeting,
                                        bool strict) {
    const std::string meeting_id = meeting_id_of(meeting);
    if (meeting_id.empty()) {
        return {};
    }
    try {
        std::vector<Json> chunks = repository.get_audio_chunks(meeting_id);
        const std::int64_t orphans = recover_stranded_wavs(repository, meeting, chunks);
        // Reloaded after the orphans are registered, so their coverage keeps the session fallback
        // from creating an overlapping chunk.
        chunks = repository.get_audio_chunks(meeting_id);
        const std::int64_t tails = recover_pcm_tails(repository, meeting, chunks);
        return {orphans, tails};
    } catch (const std::exception& error) {
        logger.error_with(error,
                          std::format("Meeting audio reconciliation failed for {}", meeting_id));
        if (strict) {
            throw RecoveryScanError(
                std::format("Could not check saved audio for meeting {}", meeting_id));
        }
        return {};
    }
}

bool is_session_dead(const Json& meeting, const Clock& clock, const PidProbe& alive) {
    const Json& heartbeat = detail::get(meeting, "app_heartbeat_at");
    if (detail::truthy(heartbeat)) {
        const std::optional<double> age = meeting_state::seconds_since(heartbeat, clock);
        if (age && *age < STALE_HEARTBEAT_S) {
            const Json& pid = detail::get(meeting, "app_pid");
            if (!detail::truthy(pid)) {
                return true;
            }
            const std::int64_t value = detail::int_or_zero(pid);
            if (value <= 0) {
                return true;
            }
            return !(alive ? alive(value) : pid_alive(value));
        }
    }
    return true;
}

std::int64_t reconcile_startup_audio(MeetingRepository& repository, const Clock& clock,
                                     const PidProbe& alive) {
    std::vector<Json> meetings;
    try {
        meetings = repository.list_meetings();
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to list meetings for audio reconciliation");
        throw RecoveryScanError("Could not list meetings for audio recovery");
    }
    std::int64_t recovered = 0;
    for (const Json& meeting : meetings) {
        const std::string status = detail::str_or_empty(detail::get(meeting, "status"));
        if (status == "active" || status == "paused" || status == "ending" ||
            status == "needs_recovery") {
            if (!is_session_dead(meeting, clock, alive)) {
                continue;
            }
        }
        const ReconcileResult result = reconcile_meeting_audio(repository, meeting, true);
        recovered += result.orphan_chunks + result.tail_chunks;
    }
    return recovered;
}

std::vector<Json> find_recoverable_meetings(MeetingRepository& repository, const Clock& clock,
                                            const PidProbe& alive) {
    reconcile_startup_audio(repository, clock, alive);
    std::vector<Json> candidates;
    try {
        candidates = repository.find_interrupted_meetings();
    } catch (const std::exception& error) {
        logger.error_with(error, "Failed to scan for interrupted meetings");
        throw RecoveryScanError("Could not query interrupted meetings");
    }
    std::vector<Json> recoverable;
    for (Json& meeting : candidates) {
        if (!is_session_dead(meeting, clock, alive)) {
            continue;
        }
        try {
            if (meeting_state::finalization_from_meeting_row(meeting).card_deferred) {
                continue;
            }
        } catch (const std::exception&) {
            logger.warning("Could not read finalization deferral for meeting {}; offering recovery",
                           detail::py_str(detail::get(meeting, "id")));
        }
        recoverable.push_back(std::move(meeting));
    }
    if (!recoverable.empty()) {
        std::string ids;
        for (const Json& meeting : recoverable) {
            ids += ids.empty() ? "" : ", ";
            ids += py::repr(text_of(detail::get(meeting, "id")));
        }
        logger.info("Found {} recoverable meeting(s): [{}]", recoverable.size(), ids);
    }
    return recoverable;
}

bool finalize_meeting(MeetingRepository& repository, const Json& meeting, const AsrFactory& asr,
                      ModelLease& model_lease, std::function<void(int done, int total)> progress,
                      const CancelToken& cancel, const FinalizeOptions& options) {
    const Clock& clock = clock_of(options);
    const std::string meeting_id = meeting_id_of(meeting);
    if (meeting_id.empty()) {
        return false;
    }
    std::string model_name = detail::str_or_empty(detail::get(meeting, "asr_model"));
    if (model_name.empty()) {
        model_name = options.asr_model.empty() ? std::string("auto") : options.asr_model;
    }

    std::vector<Json> pending;
    try {
        reconcile_meeting_audio(repository, meeting);
        // An explicit recovery action of the user: blocked and exhausted rows are retried once the
        // source file or the model is fixed. Ordinary startup replay leaves them parked.
        repository.reset_unfinished_chunks(meeting_id, true);
        pending = repository.get_pending_chunks(meeting_id, std::nullopt, {});
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to list pending chunks for {}", meeting_id));
        return false;
    }
    const int total = static_cast<int>(pending.size());
    FinalizeProgress reporter(repository, meeting_id, total, std::move(progress));

    bool drained = true;
    std::unique_ptr<MeetingAsrEngine> engine;
    bool leased = false;
    bool failed = false;
    try {
        if (total != 0) {
            leased = acquire_model_lease(&model_lease);
            engine = asr ? asr(meeting_id, model_name, finalize_language(options.asr_language))
                         : nullptr;
            if (!engine || !engine->load_backend(cancel)) {
                logger.error("ASR model {} unavailable; cannot finalize {}", py::repr(model_name),
                             meeting_id);
                failed = true;
            } else {
                engine->start([&reporter](const meeting_state::SpooledChunk& chunk,
                                          std::vector<meeting_state::TranscriptSegment> segments) {
                    reporter.on_chunk_result(chunk, std::move(segments));
                });
                engine->requeue_pending();
                reporter.report();
                drained = engine->drain(options.drain_timeout);
            }
        }
    } catch (const std::exception& error) {
        logger.error_with(error,
                          std::format("Finalize pipeline failed for meeting {}", meeting_id));
        failed = true;
    }
    stop_engine(engine.get(), meeting_id);
    // The engine's threads are joined before the lease gives the dictation model back.
    engine.reset();
    if (leased) {
        release_model_lease(&model_lease);
    }
    if (failed) {
        return false;
    }
    return complete_finalize(repository, meeting, meeting_id, drained, total, clock);
}

void mark_ended(MeetingRepository& repository, const Json& meeting, const Clock& clock) {
    Json fields = Json::object();
    fields["status"] = "ended";
    if (!detail::truthy(detail::get(meeting, "ended_at"))) {
        // A meeting that ended on its own keeps that time; now would stretch its duration by
        // however long recovery waited.
        fields["ended_at"] = meeting_state::utc_now_iso(clock);
    }
    const Json& state_json = detail::get(meeting, "state_json");
    if (detail::truthy(state_json)) {
        std::optional<Json> state = meeting_state::parse_state_json(state_json);
        if (!state) {
            logger.warning("Unparseable state_json on meeting {}; leaving snapshot untouched",
                           detail::py_str(detail::get(meeting, "id")));
        } else {
            const Json& capture = detail::get(*state, "capture");
            if (capture.is_object() && detail::truthy(detail::get(capture, "audio_incomplete"))) {
                // A retry can transcribe saved chunks but cannot recreate blocks the capture
                // dropped: a terminal warning, without the same recovery prompt every startup.
                fields["status"] = "failed";
            }
            const std::string status = fields["status"].get<std::string>();
            (*state)["status"] = status;
            const Json& cloud = meeting.is_object() && meeting.contains("cloud_enabled")
                                    ? meeting.at("cloud_enabled")
                                    : detail::get(*state, "cloud_enabled");
            const bool cloud_enabled = detail::truthy(cloud);
            (*state)["cloud_enabled"] = cloud_enabled;
            (*state)["finalization"] =
                meeting_state::FinalizationState::normalize_historical(
                    detail::get(*state, "finalization"), cloud_enabled, status)
                    .to_json();
            py::DumpOptions dump;
            dump.ensure_ascii = false;
            fields["state_json"] = py::dumps(*state, dump);
        }
    }
    try {
        repository.update_meeting(meeting_id_of(meeting), fields);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to mark meeting {} ended",
                                             detail::py_str(detail::get(meeting, "id"))));
    }
}

} // namespace fv::meeting
