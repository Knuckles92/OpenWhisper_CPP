#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "dictation/cleanup_info.h"
#include "dictation/job.h"

// The events a dictation publishes, with one payload builder per type
// (services/automation/catalog.py). Payloads never carry selected text, caret context, window
// titles, file paths or audio. Every function here is thread-safe.
namespace fv::dictation::events {

inline constexpr std::string_view RECORDING_STARTED = "recording.started";
inline constexpr std::string_view RECORDING_STOPPED = "recording.stopped";
inline constexpr std::string_view RECORDING_CANCELED = "recording.canceled";
inline constexpr std::string_view TRANSCRIPTION_PREVIEW = "transcription.preview";
inline constexpr std::string_view TRANSCRIPTION_COMPLETED = "transcription.completed";
inline constexpr std::string_view TRANSCRIPTION_FAILED = "transcription.failed";
inline constexpr std::string_view HISTORY_SAVED = "history.saved";

// The mode of a transcription read from a file rather than dictated live.
inline constexpr std::string_view FILE_MODE = "file";

// transcription.failed's error: why a job ended without text.
inline constexpr std::string_view FAILURE_CANCELED = "canceled";
inline constexpr std::string_view FAILURE_NO_SPEECH = "no_speech";
// Nothing to do, for a reason the user was told (the payload's message).
inline constexpr std::string_view FAILURE_REFUSED = "refused";
inline constexpr std::string_view FAILURE_FAILED = "failed";

// job_id, mode, action_id, source and route of a job; a null job is a file ("mode": "file").
Json job_fields(const DictationJob* job);
// The app object of history context fields (history_fields), or null without an app_id.
Json app_from_fields(const Json& fields);

Json recording_started(const DictationJob* job);
// duration_s rounded to 3 places as Python's round does, or null.
Json recording_stopped(const DictationJob* job, std::optional<double> duration_s);
Json recording_canceled(const DictationJob* job);
Json transcription_preview(const DictationJob* job, std::string_view text, bool final);
// Finished text; `fields` are the entry's history_fields. raw_text is published only where it
// is the words before AI cleanup (dictation, action, walkthrough, file) and differs from text.
Json transcription_completed(const DictationJob* job, std::string_view text,
                             const std::optional<std::string>& raw_text, const Json& fields,
                             const std::optional<CleanupInfo>& cleanup,
                             std::optional<double> audio_s);
// `error` is a FAILURE_* category (anything else is FAILURE_FAILED); `message` is kept, cut to
// 500 code points, only for a refusal, never an exception's text (it can name files).
Json transcription_failed(const DictationJob* job, std::string_view error,
                          std::optional<std::string_view> message = std::nullopt);
Json history_saved(std::string_view job_id, std::string_view record_id, std::string_view kind);

} // namespace fv::dictation::events
