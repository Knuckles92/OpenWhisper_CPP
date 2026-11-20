#pragma once

#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "engine/error.h"

namespace fv::speech {

// config.API_MODEL_CHOICES, in display order, and DEFAULT_API_MODEL.
inline constexpr std::array<std::string_view, 4> kApiModelChoices{
    "gpt-transcribe", "gpt-4o-transcribe", "gpt-4o-mini-transcribe", "whisper-1"};
inline constexpr std::string_view kDefaultApiModel = "gpt-transcribe";

// services/openai_retirement.py: OpenAI announced on 2026-08-26 that whisper-1,
// gpt-4o-transcribe, gpt-4o-mini-transcribe and gpt-4o-transcribe-diarize leave the API on
// 2027-02-26. Until then the three transcription models stay selectable with a "retiring" label;
// from that date saved choices resolve to gpt-transcribe.
inline constexpr std::chrono::year_month_day kShutdownDay{
    std::chrono::year{2027}, std::chrono::month{2}, std::chrono::day{26}};
inline constexpr std::string_view kShutdownLabel = "Feb 26, 2027";
inline constexpr std::array<std::string_view, 3> kRetiringTranscriptionModels{
    "gpt-4o-transcribe", "gpt-4o-mini-transcribe", "whisper-1"};
// Shown when the diarize model is gone before the local clock says so.
inline constexpr std::string_view kSpeakerModelRetiredMessage =
    "OpenAI has retired its speaker-identification model. On-device speaker labels were kept.";

// The local calendar date by `clock` (openai_retirement._today).
std::chrono::year_month_day local_today(const Clock& clock);

// Whether OpenAI's shutdown day has arrived.
bool retired(std::chrono::year_month_day today);
bool is_retiring_model(std::string_view model);

// API transcription models OpenAI still serves on `today`, in display order.
std::vector<std::string> api_model_choices(std::chrono::year_month_day today);
// Picker text for an API model; a retiring one carries the shutdown day:
// "whisper-1 (retiring Feb 26, 2027)".
std::string api_model_label(std::string_view model, std::chrono::year_month_day today);
// `model` while OpenAI serves it, otherwise the default API model.
std::string serving_api_model(std::string_view model, std::chrono::year_month_day today);
// The saved api_transcription_model when it is one of OpenAI's (mapped by serving_api_model),
// else the model a pre-2.0 selected_model name meant, else the default.
std::string resolve_api_transcription_model(const Json& settings,
                                            std::chrono::year_month_day today);

// A transcription request that failed, worded as the OpenAI SDK words it ("Error code: 404 -
// {...}", "Connection error.", "Request timed out."): the dictation runtime shows what() after
// "Failed to process audio: ". `status` is the HTTP status of an error answer, `code` the
// answer's error code ("model_not_found") when it has one.
class SpeechApiError : public EngineError {
public:
    SpeechApiError(const std::string& message, std::optional<int> status,
                   std::optional<std::string> code)
        : EngineError(message), status(status), code(std::move(code)) {}

    std::optional<int> status;
    std::optional<std::string> code;
};

// Whether a failed request says the requested model no longer exists: a 404 (the transcription
// route itself does not move, so a 404 from it means the model) or the code "model_not_found"
// on any status. Covers a shutdown that lands before this computer's clock reaches the day.
bool is_model_gone_error(const std::exception&);

} // namespace fv::speech
