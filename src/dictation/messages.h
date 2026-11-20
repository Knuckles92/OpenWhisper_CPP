#pragma once

#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "appcore/overlay_state.h"
#include "base/json.h"
#include "dictation/job.h"

// The status and notice strings the transcription runtime decides
// (services/runtime/transcription.py), as builders the runtime and the golden vectors share.
// Every function here is thread-safe.
namespace fv::dictation {

// What the status line says when the final pass found no words.
inline constexpr std::string_view EMPTY_ASR_MESSAGE = "No speech detected (empty after VAD)";
// A saved recording smaller than this many bytes is reported as empty or corrupted.
inline constexpr std::size_t MIN_RECORDING_BYTES = 100;
// How the overlay, the status line and History name a walkthrough.
inline constexpr std::string_view WALKTHROUGH_LABEL = "Walkthrough";

// The engine can't take a job yet; the message says what to do instead and is shown as it is.
class EngineNotReadyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// _start_failure_message: an EngineNotReadyError's own message, else
// "Failed to process audio: {error}".
std::string start_failure_message(const std::exception& error);

// The parts of a saved action (services/actions/model.py Action) the recording builders read.
// Affinity: value type.
struct RecordingAction {
    std::string name;
    // ActionKind: "open_link", "agent", "channel", "webhook", "program", "clipboard", "scripts".
    std::string kind;
    Json params = Json::object();
    std::string id;
    // How the words are prepared: "cleanup", "raw", "profile:<id>" or "transform:<id>".
    std::string shaping = "cleanup";
    bool enabled = true;

    // Action.param: the stored parameter, else the kind's default (an agent action's "agent"
    // is "claude_code" and its "include_selection" false), else null.
    Json param(std::string_view key) const;
    // The id after "profile:" or "transform:" in shaping, else "".
    std::string profile_id() const;
    std::string transform_id() const;
};

// actions/model.py SHAPING_RAW: the words go as dictated, with the dictionary and snippets but
// no AI cleanup.
inline constexpr std::string_view SHAPING_RAW = "raw";

// What the status line says when a text filter answered that it handled the words itself.
inline constexpr std::string_view FILTER_CANCELED_MESSAGE = "Not pasted — a text filter handled it";

// _reads_selection: whether an action job asks for the selected text as context (its
// include_selection parameter is exactly true). begin_job reads it only for a job the user
// started.
bool reads_selection(const RecordingAction& action);

// _recording_destination: what the overlay says a recording is for: "Walkthrough", an agent
// action's "Agent · {agent name}" ("Agent" for an agent it doesn't know), or "".
std::string recording_destination(const RecordingAction* action, JobMode mode = JobMode::Dictation);

// _report_busy: "{product} is preparing an update. Try again shortly." while a paired PC updates
// this host, else "A transcription is already in progress — wait before {action}" (action as
// "starting another recording", "re-transcribing audio", ...).
std::string busy_message(std::string_view action, bool host_update_active);

// _show_listening: the overlay state and status a recording starts with.
struct Listening {
    appcore::OverlayState overlay = appcore::OverlayState::Recording;
    std::string status;

    bool operator==(const Listening&) const = default;
};
// Command Mode: "Command Mode · Listening..."; an action: "{action name} · Listening...";
// a walkthrough: "Walkthrough · Recording..."; else "Recording · {profile}..." or
// "Recording...".
Listening listening_status(JobMode mode, std::optional<std::string_view> action_name,
                           std::optional<std::string_view> profile_name);

// _error_status: "Error: {message}" (a refusal's message bare), and for a walkthrough the note
// that says where its screenshots are (kept_note, M6).
std::string error_status(std::string_view error_message, bool refused, JobMode mode,
                         std::string_view walkthrough_kept_note = {});

// How _save_failed_recording ended.
enum class FailedRecordingSave {
    // The History entry holds the audio.
    Saved,
    // The entry was added but its audio could not be retained.
    AudioNotRetained,
    // History could not be saved at all.
    HistoryFailed,
};
// A status line and the notice near the pointer.
struct StatusNotice {
    std::string status;
    std::string notice;

    bool operator==(const StatusNotice&) const = default;
};
// _save_failed_recording's status and notice for a failed dictation whose error status is
// `status`. `remote_retry_exhausted`: the remote engine's reconnect attempts ran out.
StatusNotice failed_recording_status(std::string status, FailedRecordingSave outcome,
                                     bool remote_retry_exhausted = false);

// How _preserve_failed_audio ended.
enum class PreservedAudio {
    // Copied into Recordings under a name.
    Saved,
    // Not copied, and the original file is still there.
    NotRetained,
    // Not copied, and there was no file.
    Missing,
    // The copy raised.
    Failed,
};
// _preserve_failed_audio's status: " — audio saved in Recordings as {name}", " — audio
// retention failed; original and recovery copy kept", nothing, or " — could not retain audio;
// recovery copy kept" after `status`.
std::string preserved_audio_status(std::string status, PreservedAudio outcome,
                                   std::string_view name = {});

// What _copy_walkthrough's status reads from the walkthrough and the copy.
// Affinity: value type.
struct WalkthroughCopy {
    // WalkthroughResult::name(): "Walkthrough" or "Write-up".
    std::string name;
    int shots = 0;
    // Why the write-up that was asked for isn't there, or "".
    std::string write_up_error;
    // The walkthrough_copy_format setting as read ("pdf", "agent_task", "both", "agent",
    // "word").
    std::string copy_format;
    // The Agent format with auto-paste on: the PDF and the instructions are pasted next.
    bool auto_paste = false;
    // The clipboard took the copy.
    bool copied = false;
    // The copy put text on the clipboard (Markdown, or the agent's instructions).
    bool with_text = false;
    // Screenshots kept during the recording whose file could not be written.
    int lost_shots = 0;

    bool operator==(const WalkthroughCopy&) const = default;
};
// _copy_walkthrough's status: "{name} copied ({n} screenshot(s)) — {where to paste it}",
// "{name} saved (...); copying it failed — drag it from the card", or, when a write-up failed,
// "Write-up failed — {name} {what happened} (...): {why}" first, so a status line cut short
// still says it; ". {n} screenshot(s) couldn't be saved and is/are left out" after any of them.
std::string walkthrough_copy_status(const WalkthroughCopy& copy);

// _deliver_walkthrough's status when the walkthrough has no document: "Couldn't save the
// walkthrough document ({document_error})", or "Walkthrough finished, but its document is
// missing" without an error; then " — its words are copied and kept in History" when `copied`,
// else " — its words are kept in History" when it has words.
std::string walkthrough_missing_status(std::string_view document_error, bool copied,
                                       bool has_words);

// _delivery_notice: why the words delivered aren't the ones asked for, or "": an action's
// shaping notice, else "{profile} formatting failed ({reason}); using raw transcript" when a
// cleanup profile's pass fell back to raw text.
std::string delivery_notice(std::string_view shaping_notice,
                            std::optional<std::string_view> formatting_profile,
                            std::optional<std::string_view> cleanup_failure);

} // namespace fv::dictation
