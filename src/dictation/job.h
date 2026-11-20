#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/json.h"
#include "focus/capture_service.h"
#include "focus/snapshot.h"

// One dictation from the recording start to its history entry: what a job knows about where its
// text will go (services/dictation_pipeline.py). TranscriptionRuntime calls the stages in order;
// each returns its input unchanged when it has nothing to do, and none raises into the runtime.
namespace fv::dictation {

// JobMode's values: "dictation", "command", "transform", "action" (the words go to a saved
// action), "walkthrough" (a spoken walkthrough with screenshots, copied as a document).
enum class JobMode { Dictation, Command, Transform, Action, Walkthrough };

std::string_view to_string(JobMode mode);
// nullopt for a string that names no mode.
std::optional<JobMode> job_mode_from(std::string_view value);

// entry_kind of transcripts that were not dictated live: uploads, batches and re-transcribed
// recordings.
inline constexpr std::string_view FILE_ENTRY_KIND = "file";

// services/automation/catalog.py SOURCES: who started a job.
inline constexpr std::string_view SOURCES[] = {"hotkey", "ui", "api", "mcp", "dbus"};
// Whether a job of `source` was started by the user ("hotkey", "ui", "dbus") rather than a
// program. An empty or unknown source counts as a program's. catalog.user_started.
bool user_started(std::string_view source);

// The language and vocabulary of one final pass (services/recognition_context.RecognitionContext,
// which a12 owns; M4 adopts or aliases this one).
// Affinity: value type.
struct RecognitionContext {
    std::string language;
    std::vector<std::string> phrases;

    bool operator==(const RecognitionContext&) const = default;
};

// A selection deliberately left unread (an excluded app, a password field, the shortcut's keys
// still down), and the message that says why: command.py's UnreadSelection. It resolves a job's
// selection in place of the text, so selection_text() gives "".
// Affinity: value type.
struct UnreadSelection {
    std::string message;

    bool operator==(const UnreadSelection&) const = default;
};
using SelectionValue = std::variant<std::string, UnreadSelection>;
// Python's Future[str]: ready, pending, failed (an exception set) or abandoned (its promise
// destroyed unset, Python's cancelled future). An invalid (default) future is None.
using SelectionFuture = std::shared_future<SelectionValue>;

// _as_future: a selection already known, as a resolved future.
SelectionFuture as_future(std::string text);

// _resolved for a selection: its value, or nullopt when there is none, it failed or was
// abandoned, or it is not ready within `timeout` (zero never waits). Never throws.
std::optional<SelectionValue> resolved(const SelectionFuture& future,
                                       std::chrono::milliseconds timeout);
// _resolved for a focus capture: never waits for zero; a capture resolves exactly once and never
// fails.
std::optional<focus::FocusSnapshot> resolved(const std::shared_ptr<focus::FocusRequest>& request,
                                             std::chrono::milliseconds timeout);

// What one recording or rewrite knows about where its text will go. Shared read-only between
// the UI thread and workers (as std::shared_ptr<const DictationJob>); UI-thread callers only
// ever use a zero timeout.
// Affinity: value type; the requests it holds are thread-safe.
struct DictationJob {
    JobMode mode = JobMode::Dictation;
    // The capture service's answer, or null when app awareness is off.
    std::shared_ptr<focus::FocusRequest> focus;
    std::optional<RecognitionContext> recognition;
    // The selected text a command or transform rewrites, or an UnreadSelection.
    SelectionFuture selection;
    // Where a command, transform or routable dictation started, for the paste check only, also
    // with app awareness off; history, styles and prompts read `focus` alone.
    std::shared_ptr<focus::FocusRequest> target;
    // Names this job in events, so a listener can match its completion and history entry to its
    // start: 16 lowercase hex digits (secrets.token_hex(8)).
    std::string id = new_id();
    // The saved action an Action job runs.
    std::string action_id;
    // Who started it: one of SOURCES.
    std::string source;
    // "hotkey" when recorded with Route by voice, "auto" when a dictation may be routed by the
    // action name it starts with; "" never routes.
    std::string route;
    // Spoken routing turned this dictation into an Action job.
    bool routed = false;
    // A text filter a program leased over the live API changed the words (the user's own saved
    // filters don't count).
    bool changed_by_program = false;

    // Whether a program supplied this job's words (any source but the user's own, an unknown
    // one included) or changed them. Such words reach the user's coding agents only with
    // "Send words to my coding agents".
    bool program_words() const;
    // The focus capture, or nullopt when it failed or isn't ready; zero never waits.
    std::optional<focus::FocusSnapshot>
    snapshot(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) const;
    // The selected text, or "" when there is none, it was left unread, or it isn't ready.
    std::string
    selection_text(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero()) const;

    static std::string new_id();
};

// What begin_job is told besides the mode and settings.
struct JobOptions {
    // The text to rewrite, for command and transform jobs (as_future for a known one).
    SelectionFuture selection;
    // The action an Action job runs.
    std::string action_id;
    // Who started the job (DictationJob::source).
    std::string source;
    // DictationJob::route.
    std::string route;
    // An Action job's action wants the selected text, which the focus capture reads when app
    // awareness is on.
    bool include_selection = false;
};

// The (focus, target) captures _focus_requests starts for a job starting now.
struct FocusRequests {
    std::shared_ptr<focus::FocusRequest> focus;
    std::shared_ptr<focus::FocusRequest> target;
};

// _focus_requests: with app awareness on, one capture that reads the text near the cursor when
// the user started a dictation with "read text" on, and the selection for Command Mode, a
// dictation that reads text, or an action that asks; a job a program started reads neither.
// With awareness off, a job that may paste later (anything but a plain dictation, or a routable
// one) still learns where it started, without text. Uses `service` (focus::get_service() when
// null). Never throws: a capture that cannot start logs a warning and the job goes without.
FocusRequests focus_requests(JobMode mode, const Json& settings, std::string_view source,
                             std::string_view route, bool include_selection,
                             focus::CaptureService* service = nullptr);

// Starts a job's background captures as its recording starts; never throws. Called right after
// the recorder opens and before the preview starts, so the focus capture runs while the user
// speaks. A walkthrough reads nothing: its words become a document rather than a paste.
// `recognition` is the caller's recognition_for(nullptr, settings) (M1.14).
DictationJob begin_job(JobMode mode, const Json& settings, RecognitionContext recognition,
                       JobOptions options = {}, focus::CaptureService* service = nullptr);

} // namespace fv::dictation
