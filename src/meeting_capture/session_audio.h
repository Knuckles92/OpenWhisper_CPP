#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "base/json.h"

// The spool's files and the helpers recovery and the offline pass read them with
// (meeting/capture/spool.py, and load_channel_session from meeting/asr/offline.py). A channel's
// spool folder holds:
//   <channel>_<seq:05d>.wav          registered 16 kHz chunks
//   <chunk>.wav.recovery.json        a chunk's registration fields until its row is durable
//   <chunk>.wav.orphan               a chunk whose registration failed twice
//   <channel>_session.pcm            native-rate PCM of the current rate segment
//   <channel>_session.16k.pcm        16 kHz PCM of the segments already converted
//   <channel>_session.json           where the session PCM sits on the meeting clock
//   <channel>_session.wav            the whole session at 16 kHz, written at flush
// JSON is written as Python's json.dump(separators=(",", ":")) writes it, fsynced, then moved
// over the target.
namespace fv::meeting {

std::filesystem::path session_pcm_path(const std::filesystem::path& spool_dir,
                                       std::string_view channel);
std::filesystem::path session_16k_pcm_path(const std::filesystem::path& spool_dir,
                                           std::string_view channel);
std::filesystem::path session_meta_path(const std::filesystem::path& spool_dir,
                                        std::string_view channel);
std::filesystem::path session_wav_path(const std::filesystem::path& spool_dir,
                                       std::string_view channel);

// The sidecar of an unregistered chunk, beside its canonical name: the WAV may move between
// "*.wav" and "*.wav.orphan" while the database is unavailable, and recovery must still find it.
std::filesystem::path chunk_recovery_meta_path(const std::filesystem::path& file_path);

struct ChunkRecoveryMeta {
    std::string meeting_id;
    std::string channel;
    std::int64_t seq = 0;
    double start_s = 0.0;
    double duration_s = 0.0;
    int sample_rate = 16000;
    friend bool operator==(const ChunkRecoveryMeta&, const ChunkRecoveryMeta&) = default;
};

// {"version":1,"meeting_id",...,"sample_rate"}, written atomically. Throws std::system_error.
void write_chunk_recovery_meta(const std::filesystem::path& file_path, std::string_view meeting_id,
                               std::string_view channel, std::int64_t seq, double start_s,
                               double duration_s, int sample_rate = 16000);
// nullopt when the sidecar is missing, unreadable, not an object, or its fields do not convert
// as Python's int() and float() would.
std::optional<ChunkRecoveryMeta> load_chunk_recovery_meta(const std::filesystem::path& file_path);
// Best effort, once the chunk's row is durable.
void remove_chunk_recovery_meta(const std::filesystem::path& file_path);

struct SessionMeta {
    int sample_rate = 0;           // of the file the meta describes
    std::int64_t sample_count = 0; // int16 samples written so far
    double origin_s = 0.0;         // meeting time of the session's first sample
    // After a rate change the native PCM starts later than the session: where it starts, how many
    // 16 kHz samples precede it, and whether it is a current, unconverted suffix. Old files lack
    // them.
    std::optional<double> pcm_origin_s;
    std::optional<std::int64_t> prefix_16k_samples;
    std::optional<bool> pcm_active;
};

// pcm_origin_s defaults to origin_s; prefix_16k_samples is kept at 0 or more. Throws
// std::system_error.
void write_session_meta(const std::filesystem::path& path, int sample_rate,
                        std::int64_t sample_count, double origin_s,
                        std::optional<double> pcm_origin_s = std::nullopt,
                        std::int64_t prefix_16k_samples = 0, bool pcm_active = true);
std::optional<SessionMeta> load_session_meta(const std::filesystem::path& path);

// Streams a native packed int16 PCM file into 16 kHz PCM, resampling each window_s window on
// its own; appends when asked and the destination exists. Returns the samples written; fsyncs.
std::int64_t resample_pcm_file_to_16k(const std::filesystem::path& src_pcm, int src_rate,
                                      const std::filesystem::path& dest_pcm, bool append = false,
                                      double window_s = 30.0);

// The native PCM interval [start_sample, end_sample) as an atomic 16 kHz mono WAV, streamed in
// bounded windows so a long interrupted meeting needs no equally large allocation. Returns the
// frames written. The temporary file is reopened read-write before it is fsynced, because
// Windows refuses to commit a read-only descriptor.
std::int64_t pcm_slice_to_wav(const std::filesystem::path& src_pcm, int src_rate,
                              std::int64_t start_sample, std::int64_t end_sample,
                              const std::filesystem::path& wav_path, double window_s = 30.0);

// Packed 16 kHz int16 PCM wrapped as a mono WAV, replacing wav_path atomically.
void pcm16k_to_wav(const std::filesystem::path& pcm_path, const std::filesystem::path& wav_path);

// Registered 16 kHz chunks of one channel (meeting_audio_chunks rows) joined into a WAV in
// (start_s, seq) order, with silence where the meeting clock has a gap, for a session whose WAV
// is missing (a crash, an older meeting). An unreadable or incompatible chunk is skipped and
// logged. Returns the meeting time of sample 0, or nullopt when no chunk qualifies. Throws
// py::ValueError when a row's start_s or seq does not convert, as Python's float() and int() do.
std::optional<double> concat_channel_chunks_to_wav(const std::vector<Json>& chunks,
                                                   std::string_view channel,
                                                   const std::filesystem::path& output_path);

// The channel's session WAV when it holds audio (more than a header), else one stitched from
// `chunks`; nullopt when neither exists.
std::optional<std::filesystem::path> resolve_session_wav(const std::filesystem::path& spool_dir,
                                                         std::string_view channel,
                                                         const std::vector<Json>& chunks = {});

// offline.py session_origin_s: the session meta's origin_s, or 0.0.
double session_origin_s(const std::filesystem::path& spool_dir, std::string_view channel);

// offline.py load_channel_session: one channel's 16 kHz session audio (stitched from `chunks`
// when its WAV is missing), its rate and the meeting time of its first sample. The samples are
// nullopt, with 16000 and 0.0, when there is no audio or it cannot be read. When the meta gives
// no origin, the channel's earliest chunk start_s stands in.
std::tuple<std::optional<std::vector<std::int16_t>>, int, double>
load_channel_session(const std::filesystem::path& spool_dir, std::string_view channel,
                     const std::vector<Json>& chunks = {});

} // namespace fv::meeting
