#pragma once

#include "llm/model_catalog.h"
#include "llm/streaming.h"

namespace fv::meeting_agent {

struct LoopEvent {
    std::string type;
    std::string delta;
    std::string tool;
};

struct PassBudget {
    double wall_s;
    std::optional<double> stall_s;
};

struct PassOutcome {
    bool ok = false;
    bool canceled = false;
    std::string error;
    Json usage = Json::object();
    // The tool calls of the last model turn that never ran because the pass was canceled or
    // timed out, in the model's order (Pi's bridge still answered them, refused).
    std::vector<llm::ToolCall> skipped_calls;
};

using ToolExecutor = std::function<std::string(std::string_view, const Json&)>;

// A pass starts with a fresh charter and prompt. Tool results are replayed only inside it.
class AgentLoop {
public:
    AgentLoop(std::shared_ptr<http::Client>, llm::TextLLMProfile, std::string model,
              llm::TextModelSpec, http::Headers, std::string api_key, const Clock&);
    PassOutcome run_pass(std::string_view charter, std::string_view user_prompt, const Json& tools,
                         ToolExecutor, PassBudget, const CancelToken&,
                         std::function<void(const LoopEvent&)> = {});

private:
    llm::OpenAiCompatClient client_;
    llm::TextLLMProfile profile_;
    std::string model_;
    llm::TextModelSpec spec_;
    const Clock& clock_;
};

} // namespace fv::meeting_agent
