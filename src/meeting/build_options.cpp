#include "meeting/build_options.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "llm/cleanup_settings.h"
#include "meeting_state/report_views.h"
#include "models/meeting_speech.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::meeting {

namespace {

// MeetingAgentCore.INSTALLED: the agents that run as installed command-line tools.
constexpr std::array<std::string_view, 3> kInstalledAgents = {"claude_code", "codex",
                                                              "opencode_cli"};

bool installed_agent(std::string_view kind) {
    return std::ranges::find(kInstalledAgents, kind) != kInstalledAgents.end();
}

} // namespace

Json resolve_meeting_insight_review(const Json& settings) {
    const bool enabled = keys::meeting_insight_review.read(settings);
    const bool consent =
        keys::meeting_insight_review_consent.read(settings) == std::string(kInsightReviewConsent);
    Json review = Json::object();
    review["enabled"] = enabled && consent && keys::typesafe_enabled.read(settings);
    review["consent"] = consent ? std::string(kInsightReviewConsent) : std::string();
    review["sensitivity"] = keys::meeting_insight_review_sensitivity.read(settings);
    return review;
}

std::string resolve_meeting_agent_model(std::string_view agent_id, const Json& settings) {
    if (!installed_agent(agent_id)) {
        return {};
    }
    const Json models = keys::meeting_agent_models.read(settings);
    if (!models.is_object()) {
        return {};
    }
    const auto found = models.find(std::string(agent_id));
    if (found == models.end() || !found->is_string()) {
        return {};
    }
    return py::strip(found->get<std::string>());
}

MeetingEngineOptions build_options(const Json& settings, bool cloud, bool demo,
                                   std::string_view system_audio_policy, std::string_view intent,
                                   const BuildOptionsSources& sources, const CancelToken& cancel) {
    const Clock& clock = sources.clock != nullptr ? *sources.clock : system_clock();
    MeetingEngineOptions options;
    options.title = demo ? "Demo Planning Sync" : "";
    options.intent = std::string(intent);
    options.cloud_enabled = cloud;
    if (sources.mic_priority) {
        options.mic_priority = sources.mic_priority();
    }
    options.asr_model = models::resolve_meeting_whisper_model(settings);
    options.asr_language = keys::meeting_language.read(settings);

    // resolve_meeting_agent_route for a new meeting. The packaged sidecars are one in-process
    // agent loop here, so no payload folder is named.
    const std::string kind = keys::meeting_agent_core.read(settings);
    options.agent_core_kind = kind;
    if (installed_agent(kind)) {
        options.llm_provider = kind;
        options.llm_model = resolve_meeting_agent_model(kind, settings);
        options.llm_endpoint = std::nullopt;
    } else {
        options.llm_provider = llm::resolve_meeting_llm_provider(settings);
        options.llm_model = llm::resolve_meeting_llm_model(settings);
        options.llm_endpoint = llm::resolve_meeting_llm_endpoint(settings);
    }
    options.backup_agent_kind = keys::meeting_backup_agent.read(settings);
    options.backup_agent_model = resolve_meeting_agent_model(options.backup_agent_kind, settings);

    const std::string speaker_backend =
        demo ? std::string("local") : models::resolve_meeting_speaker_id_backend(settings, clock);
    // Any backend but "off" labels speakers on device (the OpenAI pass runs after the live one).
    const bool want_speaker_model = !demo && speaker_backend != "off";
    if (want_speaker_model) {
        if (sources.speaker_model_path) {
            options.diarization_model_path = sources.speaker_model_path();
        }
        if (sources.ensure_speaker_model) {
            options.diarization_model_resolver = [resolver = sources.ensure_speaker_model, cancel] {
                return resolver(cancel);
            };
        }
        if (sources.onnxruntime_path) {
            options.onnxruntime_path = sources.onnxruntime_path();
        }
    }
    options.speaker_id_backend = speaker_backend;
    options.speaker_id_audio_consent =
        demo ? false : keys::meeting_audio_upload_consent_given.read(settings);
    options.server_bind = keys::meeting_server_bind.read(settings);
    options.server_port = static_cast<std::uint16_t>(
        std::clamp<std::int64_t>(keys::meeting_server_port.read(settings), 0, 65535));
    options.spool_root = sources.spool_root;
    options.end_redecode = demo ? false : keys::meeting_end_redecode.read(settings);
    options.redecode_coverage_guard = keys::meeting_redecode_coverage_guard.read(settings);
    options.end_polish = keys::meeting_end_polish.read(settings);
    options.end_report = keys::meeting_end_report.read(settings);
    options.insight_review = resolve_meeting_insight_review(settings);
    options.report_views = meeting_state::resolve_meeting_report_views(settings);
    options.demo_mode = demo;
    options.system_audio_policy =
        system_audio_policy.empty() ? std::string("auto") : std::string(system_audio_policy);
    return options;
}

} // namespace fv::meeting
