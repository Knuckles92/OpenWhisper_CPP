#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/devices.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "meeting/engine_options.h"

// MeetingRuntime._build_options (services/runtime/meeting.py): the settings snapshot of a start
// turned into the engine's options. What needs the machine (the microphone order, the speaker
// model on disk, its download, the ONNX Runtime library) comes in as functions, so nothing here
// touches a device, the disk or the network.
namespace fv::meeting {

// The consent text version a meeting insight review is agreed under (services/settings.py
// INSIGHT_REVIEW_CONSENT, meeting_state::REVIEW_CONSENT).
inline constexpr std::string_view kInsightReviewConsent = "typesafe-text-v1";

// What a start reads besides the settings.
struct BuildOptionsSources {
    // The saved microphone order (load_priority); empty: none (the system default).
    std::function<std::vector<audio::DeviceKey>()> mic_priority;
    // A speaker model already on this computer (speaker_model_path), or nullopt.
    std::function<std::optional<std::filesystem::path>()> speaker_model_path;
    // Fetches a missing speaker model (ensure_speaker_model); handed to the engine, which calls
    // it on its speaker thread, never here. It stops early (throwing) once `cancel` fires: the
    // engine's shutdown waits only a few seconds for that thread, and its destructor for good.
    std::function<std::optional<std::filesystem::path>(const CancelToken& cancel)>
        ensure_speaker_model;
    // The ONNX Runtime library the speaker model runs on (locate_onnxruntime), or nullopt.
    std::function<std::optional<std::filesystem::path>()> onnxruntime_path;
    // config.MEETINGS_FOLDER.
    std::filesystem::path spool_root;
    // The date resolve_meeting_speaker_id_backend judges by; nullptr: system_clock().
    const Clock* clock = nullptr;
};

// resolve_meeting_insight_review: {enabled, consent, sensitivity}.
Json resolve_meeting_insight_review(const Json& settings);

// resolve_meeting_agent_model: the model saved for an installed agent ("" for its own default
// or a name that is not an installed agent).
std::string resolve_meeting_agent_model(std::string_view agent_id, const Json& settings);

// The options for a new meeting. A demo meeting is titled "Demo Planning Sync", labels speakers
// on device without any model, never asks for audio-upload consent and never re-decodes at End.
// The speaker model and ONNX Runtime are looked up only when speaker identification is not "off";
// a missing model is left to the engine's resolver, never downloaded here; the resolver runs
// sources.ensure_speaker_model with `cancel`. A remote speech route (meeting_asr_source
// "remote") arrives with remote meetings and is not set here.
MeetingEngineOptions build_options(const Json& settings, bool cloud, bool demo,
                                   std::string_view system_audio_policy, std::string_view intent,
                                   const BuildOptionsSources& sources,
                                   const CancelToken& cancel = {});

} // namespace fv::meeting
