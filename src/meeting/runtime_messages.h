#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Every status line and error the meeting runtime shows (services/runtime/meeting.py), so the
// tab, the tray and the tests read the same sentences. Sentences that name the product take it
// from identity().
namespace fv::meeting::messages {

// ---- start ----
inline constexpr std::string_view kBackupBusy = "Wait for backup or restore preparation to finish";
// "<product> is preparing an update. Try again shortly."
std::string update_preparing();
inline constexpr std::string_view kFinalInsightsPreparing =
    "Final insights are still being prepared.";
inline constexpr std::string_view kAlreadyInProgress = "A meeting is already in progress";
inline constexpr std::string_view kFinishRecording =
    "Finish recording or transcription before starting a meeting";
inline constexpr std::string_view kStarting = "Starting meeting...";
inline constexpr std::string_view kStartingDemo = "Starting demo meeting...";
inline constexpr std::string_view kDashboardNoAddress =
    "Meeting dashboard did not provide an address.";
// A start failure whose text begins with this is the Linux system-audio refusal.
inline constexpr std::string_view kLinuxSystemAudioPrefix = "LINUX_SYSTEM_AUDIO_REQUIRED:";
inline constexpr std::string_view kSystemAudioUnavailable =
    "System audio became unavailable while starting. Retry detection or continue "
    "microphone-only.";
// "Could not start the meeting: <message>", the Linux refusal replaced by
// kSystemAudioUnavailable.
std::string could_not_start(std::string_view message);
inline constexpr std::string_view kInProgress = "Meeting in progress";
inline constexpr std::string_view kDemoLoaded =
    "Demo meeting loaded — End to test cleanup and the report";

// ---- pause, resume, end ----
inline constexpr std::string_view kPaused = "Meeting paused";
inline constexpr std::string_view kEnding = "Ending meeting — finishing transcription...";
std::string could_not_pause(std::string_view error);
std::string could_not_resume(std::string_view error);
std::string could_not_end(std::string_view error);
// "Could not change AI insights: <error>" (_apply_cloud).
std::string could_not_change_insights(std::string_view error);

// ---- the ended status line (_ended_message) ----
inline constexpr std::string_view kEndedFinalizing = "Meeting ended — preparing final insights…";
inline constexpr std::string_view kEndedCaptureError =
    "Meeting ended — audio capture was incomplete; partial audio was kept in Meeting History";
inline constexpr std::string_view kEndedNeedsRecovery =
    "Meeting ended — transcription recovery needed";
inline constexpr std::string_view kEnded = "Meeting ended";

// ---- engine events ----
inline constexpr std::string_view kIntelligenceOnline = "Meeting intelligence online";
inline constexpr std::string_view kIntelligenceOffline =
    "Meeting intelligence offline — transcript-only";
inline constexpr std::string_view kPreparingFinalInsights = "Preparing final insights…";

// ---- the card, Keep for later, background finishing ----
inline constexpr std::string_view kSaved = "Meeting saved. Open it from Past Meetings.";
inline constexpr std::string_view kSavedForLater =
    "Meeting saved for later. Open it from Past Meetings when you want to continue.";
inline constexpr std::string_view kCouldNotSaveForLater = "Could not save this meeting for later";
inline constexpr std::string_view kCouldNotMoveToBackground =
    "Could not move this meeting to the background";
inline constexpr std::string_view kBackgroundFinished =
    "Background meeting finished. Open it from Past Meetings.";
inline constexpr std::string_view kBackgroundAttention =
    "Background meeting needs attention. Open it from Past Meetings.";
inline constexpr std::string_view kStillInBackground =
    "This meeting is still finishing in the background.";
// "<n> meeting(s) finishing in the background. Results appear in Past Meetings." ("" for none),
// after `outcome` when there is one.
std::string background_message(std::size_t count, std::string_view outcome = {});

// ---- retry ----
inline constexpr std::string_view kRetryBusy =
    "Post-meeting steps are already running for this meeting.";
inline constexpr std::string_view kRetryFailed = "Post-meeting retry failed.";
inline constexpr std::string_view kRetrying = "Retrying post-meeting steps…";
std::string retry_failed(std::string_view error); // "Post-meeting retry failed: <error>"
inline constexpr std::string_view kSpeakerIdUnavailable = "Speaker identification is unavailable.";

// ---- dashboards and past meetings ----
inline constexpr std::string_view kNoDashboard =
    "No meeting dashboard is available. Start a meeting or open one from Past Meetings.";
inline constexpr std::string_view kNoReport =
    "No meeting report is available. Select a meeting from Past Meetings.";
inline constexpr std::string_view kFinishCurrentFirst = "Finish the current meeting first.";
inline constexpr std::string_view kMeetingGone = "That meeting no longer exists";
inline constexpr std::string_view kOpeningAPastMeeting = "Opening a past meeting...";
inline constexpr std::string_view kOpeningPastMeeting = "Opening past meeting";
inline constexpr std::string_view kOpeningReport = "Opening meeting report";
inline constexpr std::string_view kLiveDashboardUnavailable =
    "The live meeting dashboard is unavailable";
inline constexpr std::string_view kNoDashboardLink = "Could not create a meeting dashboard link";
std::string could_not_load_past(std::string_view error);
std::string could_not_open_past(std::string_view error);
inline constexpr std::string_view kNoGuestLink = "No guest link available yet";
inline constexpr std::string_view kNoTranscript = "No transcript to copy";
std::string could_not_copy(std::string_view error);
inline constexpr std::string_view kWaitPostMeeting =
    "Wait for this meeting's post-meeting steps to finish.";
inline constexpr std::string_view kDeleted = "Meeting deleted";
std::string could_not_delete(std::string_view error);
inline constexpr std::string_view kCleared = "Meetings cleared";
inline constexpr std::string_view kClearedWithRecordings = "Meetings and recordings cleared";
std::string could_not_clear(std::string_view error);
// retitle_saved_meeting's ControlError texts.
inline constexpr std::string_view kRecordBusy =
    "record_busy: Wait for the meeting's current work to finish.";
inline constexpr std::string_view kUpdateRejected =
    "update_rejected: The meeting title could not be changed.";

// ---- recovery ----
inline constexpr std::string_view kRecoveryScanFailed =
    "Recovery could not check saved meetings. Retry the scan; your meeting files have not been "
    "removed.";
// kRecoveryScanFailed + " Details: <error>".
std::string recovery_scan_failed(std::string_view error);
inline constexpr std::string_view kFinalizeFailed =
    "Could not finalize the meeting — transcription may still be pending";
inline constexpr std::string_view kRecoveredIncomplete =
    "Meeting transcription finished; captured audio remains incomplete. Review the partial "
    "meeting in History.";
inline constexpr std::string_view kRecoveredFinalized = "Interrupted meeting finalized";
std::string could_not_finalize(std::string_view error);
std::string unknown_meeting(std::string_view meeting_id); // "Unknown meeting '<id>'"
inline constexpr std::string_view kDiscarded = "Interrupted meeting discarded";
std::string could_not_discard(std::string_view error);

} // namespace fv::meeting::messages
