#include "meeting_agent/agent_loop.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <thread>
#include <utility>

#include "meeting_agent/agent_request.h"
#include "meeting_agent/usage.h"

namespace fv::meeting_agent {
namespace {

using namespace std::chrono_literals;

class Deadline {
public:
    Deadline(const Clock& clock, PassBudget budget, const CancelToken& parent)
        : clock_(clock), budget_(budget), source_(parent), started_(clock.steady()),
          last_(started_), worker_([this](std::stop_token stop) {
              while (!stop.stop_requested() && !source_.cancelled()) {
                  check();
                  source_.token().wait_for(20ms);
              }
          }) {}

    void check() {
        const auto now = clock_.steady();
        const auto seconds = [](auto duration) {
            return std::chrono::duration<double>(duration).count();
        };
        if (seconds(now - started_) >= budget_.wall_s) {
            expire(1);
        } else if (!tool_running_ && budget_.stall_s &&
                   seconds(now - last_.load()) >= *budget_.stall_s) {
            expire(2);
        }
    }
    void progress() { last_ = clock_.steady(); }
    void tool_running(bool running) {
        progress();
        tool_running_ = running;
    }
    CancelToken token() const { return source_.token(); }
    std::string error() const {
        switch (expired_.load()) {
        case 1:
            return std::format("Agent pass timed out after {:.0f}s", budget_.wall_s);
        case 2:
            return std::format("Agent pass stalled after {:.0f}s without agent progress",
                               *budget_.stall_s);
        default:
            // sidecar.py's value for a canceled pass, as InstalledAgentCore's.
            return "canceled";
        }
    }
    bool timed_out() const { return expired_.load() != 0; }

private:
    void expire(int reason) {
        int expected = 0;
        if (expired_.compare_exchange_strong(expected, reason)) {
            source_.cancel();
        }
    }
    const Clock& clock_;
    PassBudget budget_;
    CancelSource source_;
    std::chrono::steady_clock::time_point started_;
    std::atomic<std::chrono::steady_clock::time_point> last_;
    std::atomic<bool> tool_running_{false};
    std::atomic<int> expired_{0};
    std::jthread worker_;
};

} // namespace

AgentLoop::AgentLoop(std::shared_ptr<http::Client> http, llm::TextLLMProfile profile,
                     std::string model, llm::TextModelSpec spec, http::Headers headers,
                     std::string api_key, const Clock& clock)
    : client_(std::move(http), profile.base_url.value_or(""), std::move(api_key),
              std::move(headers), llm::kSdkDefaultTimeoutS, 0),
      profile_(std::move(profile)), model_(std::move(model)), spec_(std::move(spec)),
      clock_(clock) {
    // Keep the resolved server capabilities stable through every tool round of this pass.
    auto metadata = spec_.to_dict();
    metadata["model"] = model_;
    profile_.model_metadata = std::move(metadata);
}

PassOutcome AgentLoop::run_pass(std::string_view charter, std::string_view prompt,
                                const Json& tools, ToolExecutor execute, PassBudget budget,
                                const CancelToken& cancel,
                                std::function<void(const LoopEvent&)> notify) {
    PassOutcome result;
    Deadline deadline(clock_, budget, cancel);
    const auto token = deadline.token();
    std::optional<std::chrono::steady_clock::time_point> lastEvent;
    const auto emit_event = [&](std::string type, std::string delta = {}, std::string tool = {}) {
        const auto now = clock_.steady();
        // Every event is agent progress for the stall budget; only the notification of the
        // frequent deltas is throttled, as runner.ts did (sidecar.py's stub sent them unthrottled).
        deadline.progress();
        const bool frequent =
            delta == "thinking_delta" || delta == "text_delta" || delta == "toolcall_delta";
        if (frequent && lastEvent && now - *lastEvent < 2s) {
            return;
        }
        lastEvent = now;
        if (notify) {
            notify({std::move(type), std::move(delta), std::move(tool)});
        }
    };
    Json messages = Json::array(
        {{{"role", "system"}, {"content", charter}}, {{"role", "user"}, {"content", prompt}}});
    try {
        emit_event("agent_start");
        while (!token.cancelled()) {
            emit_event("turn_start");
            llm::TextResult turn;
            for (int retries = 0;; ++retries) {
                deadline.check();
                try {
                    // A turn cut off at the output limit settles with what arrived, as Pi's
                    // turnFailure counts a "length" stop (M8.md change 4).
                    turn = llm::stream_generate(
                        client_, profile_,
                        agent_request(profile_, model_, spec_, messages, tools, token),
                        [&](const llm::StreamEvent& event) {
                            emit_event("message_update",
                                       std::string(llm::stream_delta_name(event.kind)),
                                       event.tool_name);
                        },
                        llm::StreamOptions{.settle_length_stop = true});
                    if (retries != 0) {
                        emit_event("auto_retry_end");
                    }
                    break;
                } catch (const llm::StreamError& error) {
                    if (!error.transient || retries == 3 || token.cancelled()) {
                        throw;
                    }
                    emit_event("auto_retry_start");
                    const double delay = std::clamp(
                        error.retry_after_s.value_or(
                            std::chrono::duration<double>(client_.retry_delay(retries, {}))
                                .count()),
                        0.0, 10.0);
                    if (token.wait_for(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::duration<double>(delay)))) {
                        throw;
                    }
                }
            }
            merge_usage(result.usage, turn.usage);
            if (turn.assistant_message.empty()) {
                result.error = "Model produced no assistant message.";
                break;
            }
            if (turn.tool_calls.empty()) {
                emit_event("turn_end");
                deadline.check();
                result.ok = !token.cancelled();
                break;
            }
            messages.push_back(std::move(turn.assistant_message));
            for (std::size_t index = 0; index < turn.tool_calls.size(); ++index) {
                const auto& call = turn.tool_calls[index];
                const auto skip_rest = [&] {
                    result.skipped_calls.assign(turn.tool_calls.begin() +
                                                    static_cast<std::ptrdiff_t>(index),
                                                turn.tool_calls.end());
                };
                deadline.check();
                if (token.cancelled()) {
                    skip_rest();
                    break;
                }
                emit_event("tool_execution_start", {}, call.function.name);
                deadline.check();
                if (token.cancelled()) {
                    skip_rest();
                    break;
                }
                deadline.tool_running(true);
                const auto args = call.function.arguments.empty()
                                      ? Json::object()
                                      : Json::parse(call.function.arguments, nullptr, false);
                std::string text = args.is_object()
                                       ? execute(call.function.name, args)
                                       : "Tool bridge error: Tool arguments must be a JSON object.";
                deadline.tool_running(false);
                emit_event("tool_execution_end", {}, call.function.name);
                messages.push_back({{"role", "tool"},
                                    {"tool_call_id", call.id},
                                    {"name", call.function.name},
                                    {"content", std::move(text)}});
            }
            emit_event("turn_end");
        }
    } catch (const llm::StreamError& error) {
        result.error = error.what();
        if (error.status) {
            // ARCHITECTURE.md "Provider errors (M8)", with M10.22's cleaned provider message.
            result.error =
                profile_.name + " model turn failed (HTTP " + std::to_string(*error.status) + ")";
            if (!error.provider_message.empty()) {
                result.error += ": " + error.provider_message;
            }
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    emit_event("agent_end");
    if (token.cancelled()) {
        result.ok = false;
        result.canceled = !deadline.timed_out();
        result.error = deadline.error();
    }
    return result;
}

} // namespace fv::meeting_agent
