#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "audio/devices.h"
#include "base/json.h"
#include "meeting_agent/route.h"

// meeting/engine.py MeetingEngineOptions: the plain-value configuration of one meeting, resolved
// by the caller (the meeting runtime builds it from the settings).
// Affinity: value; copied into the engine at construction.
namespace fv::meeting {

struct LiveIntelligenceApproval {
    meeting_agent::MeetingAgentRoute primary;
    std::optional<meeting_agent::MeetingAgentRoute> backup;
};

struct MeetingEngineOptions {
    std::string title;
    // The host's standing brief before the meeting starts: seeds MeetingState::intent, which the
    // host can still edit from the dashboard.
    std::string intent;
    bool cloud_enabled = false;
    std::optional<LiveIntelligenceApproval> agent_approval;
    // The ranked {name, hostapi} microphones from Settings; empty means the system default.
    // mic_device_id is the older index form.
    std::vector<audio::DeviceKey> mic_priority;
    std::optional<int> mic_device_id;
    std::string asr_model = "auto";
    // A remote speech route (M9); recorded on the meeting row as given.
    std::optional<Json> asr_remote;
    std::string asr_language = "auto";
    std::string llm_provider = "openrouter";
    std::string llm_model;
    // The text endpoint snapshot written to the meeting row (agent_endpoint_json).
    std::optional<Json> llm_endpoint;
    // Packaged "pi" | "opencode", or installed "claude_code" | "codex" | "opencode_cli".
    std::string agent_core_kind = "pi";
    std::optional<std::string> sidecar_payload_dir;
    // The installed agent AI insights move to when agent_core_kind fails; "" for none.
    std::string backup_agent_kind;
    std::string backup_agent_model;
    std::optional<std::filesystem::path> diarization_model_path;
    // Resolves (or downloads) a missing speaker model on the speaker thread. It throws with a
    // sentence for the user when no model can be had.
    std::function<std::optional<std::filesystem::path>()> diarization_model_resolver;
    // The ONNX Runtime library the speaker model runs on (located by the caller).
    std::optional<std::filesystem::path> onnxruntime_path;
    std::string speaker_id_backend = "local"; // off | local | openai
    bool speaker_id_audio_consent = false;
    std::string server_bind = "localhost"; // localhost | lan
    std::uint16_t server_port = 0;
    // Parent folder of the meeting spool folders; empty means "meeting_audio".
    std::filesystem::path spool_root;
    bool end_redecode = false;
    bool redecode_coverage_guard = false;
    bool end_polish = true;
    bool end_report = true;
    std::vector<std::string> report_views = {"ribbon", "brief", "signal"};
    // review_config's keyword arguments: enabled, sensitivity, consent.
    Json insight_review = Json::object();
    bool demo_mode = false;
    // This session only, never persisted: "auto" degrades to microphone-only, "required" fails
    // without system audio, "disabled" is an explicit microphone-only choice.
    std::string system_audio_policy = "auto";
};

// meeting/insight_review.py review_config: the state's insight_review field for a new meeting,
// {enabled, consent, sensitivity, status, questions, message}. `config` holds the optional
// "enabled", "sensitivity" (default "normal") and "consent" (default "") arguments.
Json review_config(const Json& config, bool cloud_enabled);

} // namespace fv::meeting
