#include "meeting/meeting_intelligence.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>
#include <utility>

#include "base/log.h"
#include "meeting/finalization.h"
#include "meeting_agent/prompts.h"
#include "meeting_intel/store_tool_host.h"
#include "meeting_state/ids.h"
#include "pyport/number.h"

namespace fv::meeting {
namespace {

// finalization.py's logger: polish_transcript names each block and its request id.
const log::Logger logger("meeting.finalization");

Json status(std::string_view kind, std::string_view message, int applied = 0) {
    return {{"status", kind}, {"message", message}, {"applied", applied}};
}

struct CoreSession {
    std::unique_ptr<meeting_agent::AgentCore> core;
    ~CoreSession() {
        if (core) {
            try {
                core->shutdown();
            } catch (...) {
                // The pass already returned its outcome.
            }
        }
    }
};

// What every core is handed for a meeting (engine.py _adopt_agent_core, reinsight.py
// stored_agent): the destination and the copilot charter. An installed agent runs its cards,
// polish and report passes under that charter alone; the key is resolved inside the agent layer.
meeting_agent::AgentConfig agent_config(const std::string& meeting_id,
                                        const meeting_agent::MeetingAgentRoute& route) {
    meeting_agent::AgentConfig config;
    config.meeting_id = meeting_id;
    config.provider = route.provider;
    config.model = route.model;
    config.endpoint = route.endpoint;
    config.system_prompt = meeting_agent::build_system_prompt();
    return config;
}

// finalization.py polish_transcript and reinsight.py rerun_insights: why a pass that ran did
// not succeed. `error` is the core's own; the block and the request id let a log be matched.
std::string pass_failure(bool polish, const std::string& error, int index, int size,
                         const std::string& request_id) {
    if (!polish) {
        return error.empty() ? std::string("agent failed") : error;
    }
    return std::format("{} (block {}/{}; request ID: {})",
                       error.empty() ? std::string("transcript cleanup failed") : error, index,
                       size, request_id);
}

std::string pass_timeout(bool polish, double budget_s, int index, int size,
                         const std::string& request_id) {
    if (!polish) {
        return std::format("timed out after {:.0f}s", budget_s);
    }
    return std::format("Transcript cleanup timed out after {:g}s on block {}/{}. Request ID: {}.",
                       budget_s, index, size, request_id);
}

void progress(const RerunContext& context, const std::string& message, int index, int size) {
    if (context.progress) {
        try {
            context.progress(message, index, size);
        } catch (...) {
            // The optional progress callback cannot interrupt a completed edit.
        }
    }
}

} // namespace

MeetingIntelligence::MeetingIntelligence(AgentCoreFactory factory, std::function<Json()> settings,
                                         Authorize authorize, meeting_intel::RecallDeps* recall)
    : factory_(std::move(factory)), settings_(std::move(settings)),
      authorize_(std::move(authorize)), recall_(recall) {}

bool MeetingIntelligence::available() const {
    return factory_ && authorize_;
}

std::optional<meeting_agent::MeetingAgentRoute> MeetingIntelligence::live_destination() const {
    if (!available() || !settings_) {
        return std::nullopt;
    }
    return meeting_agent::resolve_meeting_agent_route(settings_());
}

bool MeetingIntelligence::authorize_live_destination(
    const meeting_agent::MeetingAgentRoute& route) const {
    return live_destination() == route && authorize_(route);
}

std::shared_ptr<meeting_agent::AgentCore>
MeetingIntelligence::create_live_core(const RerunContext& context, const CancelToken& token) {
    if (!available() || !context.repository || !context.store || token.cancelled()) {
        return {};
    }
    const auto settings = settings_ ? settings_() : Json::object();
    const auto route = context.approved_route
                           ? *context.approved_route
                           : meeting_agent::resolve_meeting_agent_route(
                                 settings, std::optional<Json>(std::in_place, context.meeting));
    if (!authorize_(route)) {
        return {};
    }
    CoreSession session{factory_(route.kind)};
    auto& core = session.core;
    if (!core) {
        return {};
    }
    auto tools = std::make_shared<meeting_intel::StoreToolHost>(
        [store = context.store, repository = context.repository] {
            (void)repository;
            return store;
        },
        *context.repository,
        [authorize = authorize_, route, token, allowed = context.writes_allowed] {
            return !token.cancelled() && (!allowed || allowed()) && authorize(route);
        },
        recall_, settings_);
    const auto stop = token.on_cancel([agent = core.get()] { agent->cancel(); });
    if (token.cancelled()) {
        return {};
    }
    core->initialize(agent_config(context.meeting_id, route), std::move(tools));
    return token.cancelled() ? std::shared_ptr<meeting_agent::AgentCore>{}
                             : std::shared_ptr<meeting_agent::AgentCore>(std::move(core));
}

Json MeetingIntelligence::polish(const RerunContext& context, const CancelToken& cancel) {
    return run(context, cancel, true);
}

Json MeetingIntelligence::consolidate(const RerunContext& context, const CancelToken& cancel) {
    return run(context, cancel, false);
}

Json MeetingIntelligence::run(const RerunContext& context, const CancelToken& cancel, bool polish) {
    if (!available() || !context.repository || !context.store) {
        return status("unavailable", kIntelligenceUnavailableMessage);
    }
    // A re-run with a fresh desktop approval (a pinned destination and its one-use permit)
    // runs whatever the meeting's recorded toggle says, as OpenWhisper's rerun_polish and
    // rerun_insights do: that approval is what authorises it. Without one (End, a caller with
    // no approval flow) the meeting's own AI insights setting is the only consent there is.
    const bool approved_rerun = context.approved_route && context.saved_destination_allowed;
    if (!approved_rerun && !context.store->with_state([](const meeting_state::MeetingState& state) {
            return state.cloud_enabled;
        })) {
        return status("unavailable", "AI insights are off for this meeting.");
    }
    if (polish && context.segments.empty()) {
        return status("completed", "Transcript cleanup finished");
    }
    if (cancel.cancelled()) {
        return status("failed", "Meeting agent pass canceled.");
    }
    std::shared_ptr<meeting_intel::StoreToolHost> tools;
    try {
        const Json settings = settings_ ? settings_() : Json::object();
        const auto route = context.approved_route
                               ? *context.approved_route
                               : meeting_agent::resolve_meeting_agent_route(
                                     settings, std::optional<Json>(std::in_place, context.meeting));
        const auto saved_allowed = context.saved_destination_allowed;
        const bool pinned = context.approved_route.has_value();
        const auto allowed = [this, saved_allowed, pinned, route] {
            return saved_allowed ? pinned && saved_allowed() : authorize_(route);
        };
        if (!allowed()) {
            return status("unavailable",
                          "Allow this meeting intelligence destination before continuing.");
        }
        CoreSession session{factory_(route.kind)};
        if (!session.core) {
            return status("unavailable", kIntelligenceUnavailableMessage);
        }
        CancelSource pass(cancel);
        tools = std::make_shared<meeting_intel::StoreToolHost>(
            [store = context.store] { return store; }, *context.repository,
            [allowed, token = pass.token(), writable = context.writes_allowed] {
                return !token.cancelled() && (!writable || writable()) && allowed();
            },
            recall_, settings_);
        const auto stop = pass.token().on_cancel([core = session.core.get()] { core->cancel(); });
        const double defaultBudget = polish ? kPolishTimeoutS : kRerunTimeoutS;
        const double budgetSeconds = std::min(
            defaultBudget,
            std::isfinite(context.options.timeout_s) ? context.options.timeout_s : defaultBudget);
        if (budgetSeconds <= 0.0) {
            return status("failed", "Meeting agent pass timed out.");
        }
        session.core->initialize(agent_config(context.meeting_id, route), tools);
        const auto blocks =
            polish ? polish_blocks(context.segments)
                   : std::vector<std::pair<std::size_t, std::size_t>>{{0, context.segments.size()}};
        const int size = static_cast<int>(blocks.size());
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (pass.cancelled() || !allowed()) {
                return status("failed", "Meeting agent pass canceled.", tools->applied());
            }
            const auto [first, last] = blocks[i];
            const int index = static_cast<int>(i + 1);
            if (polish) {
                progress(context,
                         std::format("Cleaning transcript formatting and grammar "
                                     "(block {}/{}, {} segments)...",
                                     index, size, last - first),
                         index, size);
            } else {
                session.core->set_progress_callback(
                    [&context](const std::string& message) { progress(context, message, 1, 1); });
            }
            meeting_agent::CheckpointPayload payload;
            payload.request_id = meeting_state::random_ids().hex12();
            payload.state_snapshot = context.store->snapshot();
            const auto start = context.segments.begin() + static_cast<std::ptrdiff_t>(first);
            const auto end = context.segments.begin() + static_cast<std::ptrdiff_t>(last);
            payload.new_segments.assign(start, end);
            payload.is_polish = polish;
            payload.is_consolidation = !polish;
            const auto started = std::chrono::steady_clock::now();
            const auto elapsed = [&started] {
                return std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                    .count();
            };
            if (polish) {
                logger.info("Final polish started meeting_id={} request_id={} block={}/{} "
                            "segments={} timeout_s={}",
                            payload.state_snapshot.value("meeting_id", std::string("unknown")),
                            payload.request_id, index, size, last - first,
                            py::float_repr(budgetSeconds));
            }
            std::mutex timerMutex;
            std::condition_variable_any wake;
            std::atomic<bool> timedOut = false;
            std::jthread watchdog([&](std::stop_token stopToken) {
                std::unique_lock lock(timerMutex);
                wake.wait_for(lock, stopToken, std::chrono::duration<double>(budgetSeconds),
                              [] { return false; });
                if (!stopToken.stop_requested()) {
                    timedOut = true;
                    pass.cancel();
                }
            });
            const auto result =
                polish ? session.core->checkpoint(payload) : session.core->consolidate(payload);
            watchdog.request_stop();
            watchdog.join();
            if (timedOut) {
                if (polish) {
                    logger.warning("Final polish timed out request_id={} block={}/{} "
                                   "elapsed_s={:.2f} timeout_s={}; canceled",
                                   payload.request_id, index, size, elapsed(),
                                   py::float_repr(budgetSeconds));
                }
                return status("failed",
                              pass_timeout(polish, budgetSeconds, index, size, payload.request_id),
                              tools->applied());
            }
            if (pass.cancelled() || !result.ok) {
                if (polish) {
                    logger.warning(
                        "Final polish failed request_id={} block={}/{} elapsed_s={:.2f} error={}",
                        payload.request_id, index, size, elapsed(),
                        result.error.empty() ? std::string("transcript cleanup failed")
                                             : result.error);
                }
                // Blocks that already succeeded keep their edits, so a later failure leaves the
                // pass incomplete rather than done.
                return status("failed",
                              pass_failure(polish, result.error, index, size, payload.request_id),
                              tools->applied());
            }
            if (polish) {
                logger.info("Final polish completed request_id={} block={}/{} elapsed_s={:.2f}",
                            payload.request_id, index, size, elapsed());
            }
        }
        return status("completed",
                      polish ? "Transcript cleanup finished" : "Summary & action items ready",
                      tools->applied());
    } catch (const std::exception& error) {
        return status("failed", error.what(), tools ? tools->applied() : 0);
    }
}

} // namespace fv::meeting
