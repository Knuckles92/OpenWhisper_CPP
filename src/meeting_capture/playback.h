#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "base/json.h"
#include "meeting_state/repository.h"

// meeting/audio_playback.py: one seekable mono WAV of a meeting for the dashboard's player,
// built from the durable 16 kHz chunks and aligned to meeting time, so the transcript's
// timestamps are positions in it.
namespace fv::meeting {

inline constexpr int PLAYBACK_RATE = 16000;
// Samples rendered at a time, so a long meeting never needs one large buffer.
inline constexpr std::int64_t BLOCK_SAMPLES = PLAYBACK_RATE * 10;

// <spool_dir>/playback.wav, rendered when its cache is stale. The cache is playback.json's
// {"watermark": [[chunk id, size, mtime_ns], ...]}, valid while it equals the chunks' current
// one and playback.wav exists; both files are written beside their targets as ".tmp" and moved
// over them, and the temporary files are removed whatever happens. One build per meeting at a
// time. Throws py::ValueError("unknown meeting"), py::PyError("FileNotFoundError", "meeting
// audio is unavailable") when the meeting has no spool folder or no chunks, and
// py::ValueError("audio chunk path is outside its meeting spool") for a chunk file that is not
// directly inside the spool folder (after resolving links); what reading a chunk throws
// (std::system_error, detail::WaveError) reaches the caller.
// Affinity: thread-safe; blocks on disk I/O.
std::filesystem::path build_playback(meeting_state::MeetingRepository& repository,
                                     const std::string& meeting_id);

// _render_mono: the chunk rows (start_s, duration_s, file_path) mixed into a 16 kHz mono WAV as
// long as the latest chunk end, BLOCK_SAMPLES at a time; where more than one chunk contributes
// the samples are averaged (floor division), elsewhere a chunk's samples are taken as they are,
// and nothing is silence. A chunk that is not 16 kHz mono 16-bit is skipped with a warning.
void render_mono(const std::vector<Json>& chunks, const std::filesystem::path& output_path);

} // namespace fv::meeting
