#pragma once
#include "llm/generation.h"
#include "llm/model_catalog.h"
#include "meeting_agent/interfaces.h"

namespace fv::meeting_agent {

Json endpoint_fields(const AgentConfig&, const Json& settings = Json::object());
llm::GenerateRequest agent_request(const llm::TextLLMProfile&, std::string model,
                                   const llm::TextModelSpec&, const Json& messages,
                                   const Json& tools, const CancelToken&);

} // namespace fv::meeting_agent
