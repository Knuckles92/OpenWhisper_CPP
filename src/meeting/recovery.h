#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "meeting/model_lease.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_state/repository.h"

// Crash recovery for Meeting Mode (meeting/recovery.py). A crash leaves a meeting row active or
// paused with a stale pid and heartbeat and its spooled chunks pending, and it can leave audio the
// database never heard of: chunk WAVs whose registration failed (their recovery sidecars) and
// native session PCM past the last registered chunk. On startup the app reconciles that audio
// into pending chunk rows, finds the meetings whose owning session is dead, and offers to finalize
// them headless (transcribe what is left, mark the meeting ended) or discard them.
// Affinity: functions; they block on the repository and the file system, never on the GUI thread.
namespace fv::meeting {

// A live session heartbeats at least this often, in seconds.
inline constexpr double STALE_HEARTBEAT_S = 60.0;
// The ASR drain budget of a headless finalize, in seconds.
inline constexpr double FINALIZE_DRAIN_TIMEOUT_S = 600.0;
// Recovered units keep the upper bound of live chunks, in seconds; this matters when SQLite was
// unavailable for long and the native session PCM is the only complete source left.
inline constexpr double RECOVERED_CHUNK_MAX_S = 20.0;

// The recovery inventory could not be checked reliably, so a caller must not read the scan as
// "nothing to recover".
class RecoveryScanError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Whether process `pid` is running, read-only: on Windows an OpenProcess(0x1000) probe (access
// denied means a protected process that exists) and GetExitCodeProcess == STILL_ACTIVE (259),
// because os.kill(pid, 0) would terminate the target there; elsewhere kill(pid, 0), where EPERM
// means a process owned by someone else. False for 0 or less.
bool pid_alive(std::int64_t pid);
using PidProbe = std::function<bool(std::int64_t pid)>;

// (rate, duration_s) of a recoverable WAV, read as Python's wave module reads the header (the
// data chunk's declared size, not the file's): nullopt unless it is mono 16-bit with frames and a
// rate, or when it cannot be read.
std::optional<std::pair<int, double>> read_wav_duration(const std::filesystem::path& path);

// One native PCM span of a channel's session: the file, its rate, its 16-bit samples and the
// meeting time of its first sample.
struct PcmArtifact {
    std::filesystem::path path;
    int rate = 0;
    std::int64_t samples = 0;
    double origin_s = 0.0;

    friend bool operator==(const PcmArtifact&, const PcmArtifact&) = default;
};

// _pcm_artifacts: the channel's session PCM spans in timeline order. None once the session WAV is
// complete (normal flush finished; older meetings keep their native PCM on purpose), or without a
// session meta with a rate. The 16 kHz prefix comes first; the native file follows unless its meta
// marks it inactive (already converted), and a legacy meta that has a prefix but no pcm_origin_s
// leaves the native file alone, since it cannot tell an old converted source from a newer suffix.
std::vector<PcmArtifact> pcm_artifacts(const std::filesystem::path& spool_dir,
                                       std::string_view channel);

// What one reconciliation registered.
struct ReconcileResult {
    std::int64_t orphan_chunks = 0;
    std::int64_t tail_chunks = 0;

    friend bool operator==(const ReconcileResult&, const ReconcileResult&) = default;
};

// _recover_stranded_wavs: registers each valid "*.recovery.json" WAV of the meeting's spool once
// (pending, the WAV header's duration), restoring an orphaned WAV to its canonical name first.
// `chunks` are the meeting's chunk rows; registered rows are appended. Returns how many rows were
// created.
std::int64_t recover_stranded_wavs(meeting_state::MeetingRepository& repository,
                                   const Json& meeting, std::vector<Json>& chunks);

// _recover_pcm_tails: materializes the session PCM past each channel's last registered chunk as
// "<channel>_<seq:05d>.recovered.wav" chunks of at most RECOVERED_CHUNK_MAX_S, each protected by
// its recovery sidecar until its row is durable. A failed write or registration stops that
// channel, so the next startup retries the same interval. Returns how many rows were created.
std::int64_t recover_pcm_tails(meeting_state::MeetingRepository& repository, const Json& meeting,
                               std::vector<Json>& chunks);

// Turns crash artifacts into durable pending chunk rows; safe before every scan and finalize
// (identity is the unique (meeting_id, channel, seq)). A failure is logged and reads as nothing
// recovered, or with `strict` throws RecoveryScanError "Could not check saved audio for meeting
// <id>".
ReconcileResult reconcile_meeting_audio(meeting_state::MeetingRepository& repository,
                                        const Json& meeting, bool strict = false);

// Whether the app session that owned an interrupted meeting is dead: the heartbeat is missing,
// unreadable or older than STALE_HEARTBEAT_S, or it is fresh but the recorded pid is not running
// (a reused pid is covered by the heartbeat). `alive` replaces pid_alive (tests).
bool is_session_dead(const Json& meeting, const Clock& clock = system_clock(),
                     const PidProbe& alive = {});

// Reconciles the spools of every meeting whose session is dead or that is no longer live, before
// the recovery query runs; returns how many chunks it registered. Throws RecoveryScanError
// "Could not list meetings for audio recovery" or the reconciliation's.
std::int64_t reconcile_startup_audio(meeting_state::MeetingRepository& repository,
                                     const Clock& clock = system_clock(),
                                     const PidProbe& alive = {});

// Interrupted meetings whose owning session is dead and whose finalization card is not deferred,
// for finalize, resume or discard. Reconciles the spools first: registration failures and
// sub-chunk tails are file evidence the query needs. Throws RecoveryScanError ("Could not query
// interrupted meetings", or the reconciliation's) so that a failed scan never reads as none.
std::vector<Json> find_recoverable_meetings(meeting_state::MeetingRepository& repository,
                                            const Clock& clock = system_clock(),
                                            const PidProbe& alive = {});

// The ASR engine a headless finalize or a redecode retry decodes with, for `meeting_id`, the
// meeting's model and `language` ("auto" or an ISO-639-1 code). The factory selects the model in
// the speech service's Meeting slot when the meeting has one of its own; the engine loads
// nothing until load_backend. Null when no engine can be built.
using AsrFactory = std::function<std::unique_ptr<MeetingAsrEngine>(
    const std::string& meeting_id, const std::string& model, const std::string& language)>;

struct FinalizeOptions {
    // The model when the meeting row records none.
    std::string asr_model = "auto";
    // The spoken-language preference: "auto" or an ISO-639-1 code.
    std::string asr_language = "auto";
    std::chrono::milliseconds drain_timeout{
        static_cast<std::int64_t>(FINALIZE_DRAIN_TIMEOUT_S * 1000)};
    const Clock* clock = nullptr; // nullptr: system_clock()
};

// Headless finalization, an explicit recovery action of the user: reconciles the spool, forces
// unfinished chunks (blocked and exhausted ones too) back to pending, decodes them with an engine
// from `asr` (the lease held while it lives), commits each chunk's segments as it arrives, waits
// for the queue to drain and marks the meeting ended. `progress` hears (done, total) chunks. False
// when the meeting has no id, the chunks cannot be listed, no engine is available, the pipeline
// fails or the drain times out (the meeting stays recoverable), or chunks are still unfinished
// (the meeting is then marked needs_recovery). The lease is released before it returns, whatever
// failed.
bool finalize_meeting(meeting_state::MeetingRepository& repository, const Json& meeting,
                      const AsrFactory& asr, ModelLease& model_lease,
                      std::function<void(int done, int total)> progress = {},
                      const CancelToken& cancel = {}, const FinalizeOptions& options = {});

// _mark_ended: the meeting ended (keeping an ended_at it already has) without hiding permanent
// capture loss (capture.audio_incomplete makes it "failed"). Its raw state_json is patched in
// place, so keys this version does not model survive: the status, cloud_enabled (the row's, else
// the snapshot's), and an interrupted pending or running finalization normalized to a durable
// outcome, since a headless finalize never runs the consolidation. An unparseable snapshot is left
// untouched. A failed update is logged.
void mark_ended(meeting_state::MeetingRepository& repository, const Json& meeting,
                const Clock& clock = system_clock());

} // namespace fv::meeting
