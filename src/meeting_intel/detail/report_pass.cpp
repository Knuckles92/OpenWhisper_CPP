// meeting/custom_report.py:675-1001: one report pass, through a text model's read rounds or
// through the user's installed agent.

#include <chrono>
#include <exception>
#include <format>
#include <optional>
#include <utility>

#include "agents/agent_tasks.h"
#include "base/log.h"
#include "llm/model_catalog.h"
#include "llm/openai_client.h"
#include "llm/profiles.h"
#include "meeting_agent/agent_settings.h"
#include "meeting_agent/usage.h"
#include "meeting_intel/context_folder.h"
#include "meeting_intel/custom_report.h"
#include "meeting_intel/detail/report.h"
#include "meeting_intel/detail/text.h"
#include "meeting_state/custom_report_ops.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_intel {

namespace {

constexpr log::Logger logger("meeting.custom_report");

using detail::member;
using detail::text_or_empty;
using detail::truthy;

Json settings_of(const ReportDeps& deps) {
    return deps.settings ? deps.settings() : Json::object();
}

// The pieces both paths share: the corpus, the consent-gated tool set and the dispatcher.
struct PassInputs {
    TranscriptCorpus corpus;
    std::string meeting_id;
    bool past_recall = false;
    bool context_files = false;

    PassInputs(const Json& meeting, const Json& state, const std::vector<Json>& segments,
               ReportDeps& deps)
        : corpus(segments, member(state, "participants")) {
        const Json& state_id = member(state, "meeting_id");
        const Json& meeting_id_value = member(meeting, "id");
        meeting_id = truthy(state_id)           ? detail::py_str(state_id)
                     : truthy(meeting_id_value) ? detail::py_str(meeting_id_value)
                                                : std::string();
        // Python offers recall only with a repository to read past meetings from.
        past_recall = deps.recall != nullptr && deps.recall->history != nullptr &&
                      past_recall_enabled(deps.settings);
        context_files = context_folder_enabled(deps.settings);
    }

    Json sources(std::string_view model, std::string_view provider, const Json& used) const {
        Json out = Json::object();
        out["transcript_lines"] = corpus.size();
        out["duration_s"] = py::round_ndigits(corpus.duration_s(), 1);
        out["transcript_complete"] = corpus.total_chars() <= kReportInlineTranscriptChars;
        out["tools_used"] = used;
        out["past_meetings"] = past_recall;
        out["knowledge_folder"] = context_files;
        out["model"] = std::string(model);
        out["provider"] = std::string(provider);
        return out;
    }
};

struct Endpoint {
    llm::TextLLMProfile profile;
    llm::OpenAiCompatClient client;
};

// _build_client: the meeting's text endpoint as a profile and a client.
Endpoint build_client(const ReportRoute& route, ReportDeps& deps, double timeout_s) {
    llm::TextLLMProfile profile =
        llm::profile_from_agent_config(route.provider, route.endpoint, settings_of(deps));
    try {
        auto client = llm::create_openai_client(deps.http, profile, timeout_s, {}, {}, std::nullopt,
                                                &deps.keys);
        return Endpoint{std::move(profile), std::move(client)};
    } catch (const std::exception& error) {
        logger.warning("Report client unavailable: {}", detail::type_name(error));
        throw ReportUnavailable("No API key is configured for the meeting text endpoint. Add one "
                                "in Settings, then retry.");
    }
}

std::string default_model(const llm::TextLLMProfile& profile) {
    try {
        return llm::default_model_for_profile(profile);
    } catch (const std::exception& error) {
        logger.warning("Could not resolve a default report model ({})", detail::type_name(error));
        return {};
    }
}

bool supports_tools(const llm::TextLLMProfile& profile, std::string_view model) {
    try {
        return llm::model_spec(profile, model).tools;
    } catch (const std::exception& error) {
        logger.debug("No model spec for the report model; assuming tool support ({})",
                     detail::type_name(error));
        return true;
    }
}

// _ReportPass: the model's read rounds; its latest non-empty text is the report.
class ReportPass {
public:
    ReportPass(Endpoint& endpoint, std::string model, Json tools, detail::ReportTools& dispatcher,
               const CancelToken& cancel, const Clock& clock, const std::function<bool()>& allowed)
        : endpoint_(endpoint), model_(std::move(model)), tools_(std::move(tools)),
          dispatcher_(dispatcher), cancel_(cancel), clock_(clock), allowed_(allowed) {}

    void run(Json& messages, std::chrono::steady_clock::time_point deadline) {
        const bool with_tools = !tools_.empty();
        const int rounds = with_tools ? kReportMaxToolRounds : 1;
        for (int round = 0; round < rounds; ++round) {
            if (cancel_.cancelled() || (allowed_ && !allowed_())) {
                throw ReportUnavailable("Approval for this meeting and destination has expired.");
            }
            const double remaining =
                std::chrono::duration<double>(deadline - clock_.steady()).count();
            if (remaining <= 0) {
                throw ReportUnavailable(
                    "The report took too long to write. Try a narrower request.");
            }
            const bool last_round = with_tools && round == kReportMaxToolRounds - 1;
            if (last_round) {
                messages.push_back(Json{{"role", "user"}, {"content", kReportStopReading}});
            }
            llm::TextResult result = generate(messages, remaining, last_round);
            meeting_agent::merge_usage(usage, result.usage);
            std::string text = py::strip(result.text);
            if (!text.empty()) {
                markdown = std::move(text);
            }
            if (result.tool_calls.empty()) {
                break;
            }
            messages.push_back(result.assistant_message);
            for (const auto& call : result.tool_calls) {
                messages.push_back(Json{{"role", "tool"},
                                        {"tool_call_id", call.id},
                                        {"content", detail::run_report_tool(dispatcher_, call)}});
            }
        }
    }

    Json usage = Json::object();
    std::string markdown;

private:
    llm::TextResult generate(const Json& messages, double remaining, bool last_round) {
        try {
            llm::OpenAiCompatClient client = endpoint_.client.with_options(remaining);
            llm::GenerateRequest request;
            request.model = model_;
            request.messages = messages;
            if (!tools_.empty() && !last_round) {
                request.tools = tools_;
            }
            request.cancel = cancel_;
            request.options = Json{{"temperature", kReportTemperature}};
            return llm::generate(client, endpoint_.profile, std::move(request));
        } catch (const llm::TextGenerationError& error) {
            throw ReportUnavailable(error.what());
        } catch (const std::exception& error) {
            logger.error("Report generation call failed ({})", detail::type_name(error));
            throw ReportUnavailable("The report model could not be reached. Check the meeting "
                                    "text endpoint and retry.");
        }
    }

    Endpoint& endpoint_;
    std::string model_;
    Json tools_;
    detail::ReportTools& dispatcher_;
    const CancelToken& cancel_;
    const Clock& clock_;
    const std::function<bool()>& allowed_;
};

// _generate_with_installed_agent: the same report, written by the user's installed agent over
// the same read tools served through MCP; its closing reply is the report.
Json generate_with_installed_agent(const Json& meeting, const Json& state,
                                   const std::vector<Json>& segments, const std::string& request,
                                   const ReportRoute& route, ReportDeps& deps, double timeout_s,
                                   const CancelToken& cancel) {
    const PassInputs inputs(meeting, state, segments, deps);
    const Json tools = detail::report_mcp_tools(inputs.past_recall, inputs.context_files);
    detail::ReportTools dispatcher(inputs.corpus, deps.recall, deps.settings, inputs.meeting_id);
    agents::ToolHandler handle = [&dispatcher, &deps,
                                  cancel](std::string_view name, const Json& args,
                                          const Json&) -> std::pair<std::string, bool> {
        if (cancel.cancelled() || (deps.destination_allowed && !deps.destination_allowed())) {
            return {"Approval for this meeting and destination has expired.", true};
        }
        try {
            return {dispatcher.dispatch(name, args), false};
        } catch (const std::exception& error) {
            logger.error("Report read tool {} failed ({})", name, detail::type_name(error));
            return {std::string(name) + " failed; continue without it.", true};
        }
    };
    const std::string prompt = build_corpus_prompt(meeting, state, inputs.corpus, request);
    if (cancel.cancelled() || (deps.destination_allowed && !deps.destination_allowed())) {
        throw ReportUnavailable("Approval for this meeting and destination has expired.");
    }
    agents::PassOutcome outcome;
    try {
        if (deps.agent_task) {
            outcome = deps.agent_task(route.provider, route.model, report_system_prompt(), prompt,
                                      tools, handle, timeout_s, kReportAgentClosing, cancel);
        } else {
            outcome = agents::run_agent_task(route.provider, route.model, report_system_prompt(),
                                             prompt, tools, handle, timeout_s, kReportAgentClosing,
                                             agents::FINAL_EFFORT, cancel);
        }
    } catch (const agents::AgentUnavailable& error) {
        throw ReportUnavailable(error.what());
    }
    if (outcome.canceled) {
        throw ReportUnavailable("The report was canceled.");
    }
    if (!outcome.ok) {
        throw ReportUnavailable(outcome.error.empty() ? "The agent could not write the report."
                                                      : outcome.error);
    }
    const std::string markdown = detail::clean_markdown(outcome.text);
    if (markdown.empty()) {
        throw ReportUnavailable("The report came back empty. Try rephrasing the request.");
    }
    Json out = Json::object();
    out["markdown"] = markdown;
    out["title"] = report_title(markdown, request);
    out["sources"] = inputs.sources(route.model.empty() ? "default" : route.model, route.provider,
                                    dispatcher.used());
    out["usage"] = outcome.usage;
    return out;
}

} // namespace

namespace detail {

std::string run_report_tool(ReportTools& tools, const llm::ToolCall& call) {
    const std::string& name = call.function.name;
    const std::string raw = call.function.arguments.empty() ? "{}" : call.function.arguments;
    Json args;
    try {
        args = py::loads(raw);
    } catch (const py::RecursionError&) {
        throw; // json.loads' RecursionError is not one of the errors _run_tool turns into text
    } catch (const std::exception&) {
        return "Could not parse the arguments for " + name + ".";
    }
    if (!args.is_object()) {
        return "Arguments for " + name + " must be an object.";
    }
    try {
        return tools.dispatch(name, args);
    } catch (const std::exception& error) {
        logger.error("Report read tool {} failed ({})", name, type_name(error));
        return name + " failed; continue without it.";
    }
}

std::string clean_markdown(std::string_view markdown) {
    std::string text = py::strip(markdown);
    if (text.starts_with("```")) {
        const std::vector<std::string> lines = splitlines(text);
        if (lines.size() >= 2 && py::strip(lines.back()).starts_with("```")) {
            std::string inner;
            for (std::size_t i = 1; i + 1 < lines.size(); ++i) {
                inner += (i == 1 ? "" : "\n") + lines[i];
            }
            text = py::strip(inner);
        }
    }
    if (py::len(text) > meeting_state::MAX_REPORT_CHARS) {
        // The notice counts toward the cap, which the store enforces again.
        constexpr std::string_view notice = "\n\n*(Report truncated.)*";
        const auto keep =
            static_cast<std::int64_t>(meeting_state::MAX_REPORT_CHARS - py::len(notice));
        text = py::rstrip(py::slice(text, 0, keep)) + std::string(notice);
    }
    return text;
}

std::string validated_request(std::string_view request) {
    std::string cleaned = py::strip(request);
    if (cleaned.empty()) {
        throw ReportUnavailable("Describe the report you want.");
    }
    if (py::len(cleaned) > meeting_state::MAX_REQUEST_CHARS) {
        throw ReportUnavailable(
            std::format("Keep the request under {} characters.", meeting_state::MAX_REQUEST_CHARS));
    }
    return cleaned;
}

} // namespace detail

Json generate_report(const Json& meeting, const Json& state, const std::vector<Json>& segments,
                     std::string_view request_text, const ReportRoute& route, ReportDeps& deps,
                     double timeout_s, const CancelToken& cancel) {
    const std::string request = detail::validated_request(request_text);
    if (cancel.cancelled() || (deps.destination_allowed && !deps.destination_allowed())) {
        throw ReportUnavailable("Approval for this meeting and destination has expired.");
    }
    if (meeting_agent::is_installed_core(route.provider)) {
        return generate_with_installed_agent(meeting, state, segments, request, route, deps,
                                             timeout_s, cancel);
    }
    Endpoint endpoint = build_client(route, deps, timeout_s);
    const std::string model = route.model.empty() ? default_model(endpoint.profile) : route.model;
    if (model.empty()) {
        throw ReportUnavailable("Choose a meeting text model first.");
    }
    const PassInputs inputs(meeting, state, segments, deps);
    Json tools = detail::report_tool_schemas(inputs.past_recall, inputs.context_files);
    if (!supports_tools(endpoint.profile, model)) {
        // Without tool calls the model gets one shot at whatever the prompt carries; most
        // meetings fit inline.
        tools = Json::array();
    }
    detail::ReportTools dispatcher(inputs.corpus, deps.recall, deps.settings, inputs.meeting_id);
    Json messages = Json::array();
    messages.push_back(Json{{"role", "system"}, {"content", report_system_prompt()}});
    messages.push_back(
        Json{{"role", "user"},
             {"content", build_corpus_prompt(meeting, state, inputs.corpus, request)}});
    const auto deadline =
        deps.clock.steady() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(timeout_s));
    ReportPass pass(endpoint, model, std::move(tools), dispatcher, cancel, deps.clock,
                    deps.destination_allowed);
    pass.run(messages, deadline);

    const std::string markdown = detail::clean_markdown(pass.markdown);
    if (markdown.empty()) {
        throw ReportUnavailable("The report came back empty. Try rephrasing the request.");
    }
    const std::string provider = endpoint.profile.id.empty() ? route.provider : endpoint.profile.id;
    Json out = Json::object();
    out["markdown"] = markdown;
    out["title"] = report_title(markdown, request);
    out["sources"] = inputs.sources(model, provider, dispatcher.used());
    out["usage"] = pass.usage;
    return out;
}

} // namespace fv::meeting_intel
