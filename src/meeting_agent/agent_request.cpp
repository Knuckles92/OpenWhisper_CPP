#include "meeting_agent/agent_request.h"

#include <stdexcept>
#include <utility>

namespace fv::meeting_agent {

Json endpoint_fields(const AgentConfig& config, const Json& settings) {
    const auto profile = llm::profile_from_agent_config(config.provider, config.endpoint, settings);
    const auto spec = llm::model_spec(profile, config.model);
    if (!spec.tools) {
        throw std::runtime_error("This model cannot use meeting tools with this meeting engine.");
    }
    Json headers = Json::object();
    if (auto provided = llm::provider_headers(profile, config.meeting_id)) {
        for (const auto& [name, value] : *provided) {
            headers[name] = value;
        }
    }
    return {{"base_url", profile.base_url.value_or("")},
            {"kind", profile.kind},
            {"model_metadata", spec.to_dict()},
            {"headers", std::move(headers)}};
}

llm::GenerateRequest agent_request(const llm::TextLLMProfile& /*profile*/, std::string model,
                                   const llm::TextModelSpec& spec, const Json& messages,
                                   const Json& tools, const CancelToken& token) {
    llm::GenerateRequest request;
    request.model = std::move(model);
    request.messages = messages;
    request.tools = tools;
    request.cancel = token;
    // pi-adapter.ts registered `reasoning: metadata.reasoning ?? <kind is openrouter or openai>`,
    // but sidecar.py always sent spec.to_dict(), whose reasoning is a bool, so the kind default
    // never applied: Pi thought (at medium) exactly when the spec reasons.
    if (spec.reasoning) {
        request.reasoning_level = "medium";
    }
    request.options["max_tokens"] = spec.max_output_tokens > 0 ? spec.max_output_tokens : 16384;
    return request;
}

} // namespace fv::meeting_agent
