#include "agentapi/app_control.h"

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "automation/action_model.h"
#include "automation/permissions.h"
#include "base/executor.h"
#include "base/identity.h"
#include "dictation/automation_port.h"
#include "pyport/text.h"
#include "settings/store.h"

namespace fv::agentapi {

namespace {

namespace permissions = automation::permissions;

// How often a waiting caller looks at the clock; a call that finishes wakes it at once.
constexpr std::chrono::milliseconds kPoll{10};

std::string product() {
    return std::string(identity().product);
}

automation::ControlError unavailable(const std::string& message) {
    return automation::ControlError(std::string(automation::UNAVAILABLE), message);
}

automation::ControlError not_ready() {
    return unavailable(product() + " isn't ready for this yet.");
}

automation::ControlError refused(const std::string& message) {
    return automation::ControlError(std::string(automation::REFUSED), message);
}

// One call queued to the UI executor. The caller and the UI thread share it.
template <class T> struct Call {
    enum class State { Queued, Running, Done, Cancelled };

    std::mutex mutex;
    std::condition_variable done;
    State state = State::Queued;
    std::conditional_t<std::is_void_v<T>, bool, std::optional<T>> result{};
    std::exception_ptr error;

    // Waits until the call is done or the clock reaches `deadline`; true when done.
    bool wait_until(std::unique_lock<std::mutex>& lock, const Clock& clock,
                    std::chrono::steady_clock::time_point deadline) {
        while (state != State::Done) {
            if (clock.steady() >= deadline) {
                return false;
            }
            done.wait_for(lock, kPoll);
        }
        return true;
    }
};

// _enabled_action: the enabled action `action_id` for a program. Every caller is a program (the
// live API or an MCP agent), so the words are a program's: an action that would not take them
// (an agent or channel action without api_agent_prompts) is refused now rather than after the
// recording.
void enabled_action(const Json& saved, std::string_view action_id) {
    const std::optional<automation::Action> action = automation::find_action(saved, action_id);
    if (!action || !action->enabled) {
        throw automation::ControlError(std::string(automation::NOT_FOUND),
                                       "No enabled action has that id.");
    }
    const bool agent_kind = action->kind == automation::ActionKind::AGENT ||
                            action->kind == automation::ActionKind::CHANNEL;
    if (agent_kind && !permissions::granted(saved, permissions::AGENT_PROMPTS)) {
        throw permissions::PermissionDenied(std::string(permissions::AGENT_PROMPTS));
    }
}

std::string one_line(std::string_view text) {
    std::string out;
    for (const std::string& word : py::split(text)) {
        out += (out.empty() ? "" : " ") + word;
    }
    return out;
}

} // namespace

UiAppControl::UiAppControl(ControlHost& host, Executor& ui, SettingsStore& settings,
                           const Clock& clock)
    : host_(&host), ui_(ui), settings_(settings), clock_(clock) {}

void UiAppControl::release() {
    std::unique_lock lock(mutex_);
    host_ = nullptr;
}

ControlHost& UiAppControl::require() {
    std::shared_lock lock(mutex_);
    if (host_ == nullptr) {
        throw not_ready();
    }
    return *host_;
}

// call_on_qt: fn(host) on the UI thread, from any thread. Throws ControlError "unavailable" when
// the UI thread does not take the call within CALL_TIMEOUT (it then never runs) or does not
// finish one it started within RUNNING_GRACE more; otherwise whatever fn threw, with the
// pipeline's refusals as the API's.
template <class Fn> auto UiAppControl::call_on_ui(Fn fn) {
    using T = std::invoke_result_t<Fn, ControlHost&>;
    const auto run = [this, fn]() -> T {
        try {
            return fn(require());
        } catch (const dictation::RunRefused& refusal) {
            throw automation::ControlError(refusal.code(), refusal.message());
        } catch (const dictation::RunPermissionDenied& denied) {
            throw permissions::PermissionDenied(denied.permission());
        }
    };
    require();
    if (ui_.is_current()) {
        return run();
    }
    auto call = std::make_shared<Call<T>>();
    ui_.post([call, run] {
        {
            std::lock_guard lock(call->mutex);
            if (call->state != Call<T>::State::Queued) {
                return; // the caller gave up before the UI thread got here
            }
            call->state = Call<T>::State::Running;
        }
        try {
            if constexpr (std::is_void_v<T>) {
                run();
            } else {
                auto value = run();
                std::lock_guard lock(call->mutex);
                call->result = std::move(value);
            }
        } catch (...) {
            std::lock_guard lock(call->mutex);
            call->error = std::current_exception();
        }
        std::lock_guard lock(call->mutex);
        call->state = Call<T>::State::Done;
        call->done.notify_all();
    });
    std::unique_lock lock(call->mutex);
    bool finished = call->wait_until(lock, clock_, clock_.steady() + CALL_TIMEOUT);
    if (!finished) {
        if (call->state == Call<T>::State::Queued) {
            call->state = Call<T>::State::Cancelled;
            throw unavailable(product() + " didn't respond in time. Try again.");
        }
        finished = call->wait_until(lock, clock_, clock_.steady() + RUNNING_GRACE);
    }
    if (!finished) {
        throw unavailable(product() + " is still working on that. Check its state in a moment.");
    }
    if (call->error) {
        std::rethrow_exception(call->error);
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(*call->result);
    }
}

automation::LiveState UiAppControl::state() {
    std::shared_lock lock(mutex_);
    if (host_ == nullptr) {
        throw not_ready();
    }
    return host_->state();
}

Json UiAppControl::list_actions() {
    Json out = Json::array();
    for (const automation::Action& action : automation::load_actions(*settings_.snapshot())) {
        if (action.enabled) {
            out.push_back(action.public_dict());
        }
    }
    return out;
}

std::string UiAppControl::run_action(std::string_view action_id, std::string_view text,
                                     std::string_view source) {
    return call_on_ui([action = std::string(action_id), words = std::string(text),
                       from = std::string(source)](ControlHost& host) {
        return host.run_action_text(action, words, from);
    });
}

std::string UiAppControl::start_recording(std::optional<std::string> action_id,
                                          std::string_view source) {
    return call_on_ui(
        [this, action = action_id.value_or(""), from = std::string(source)](ControlHost& host) {
            const automation::LiveState now = host.state();
            if (now.recording) {
                throw automation::ControlError(std::string(automation::BUSY),
                                               product() + " is already recording.");
            }
            if (now.busy) {
                throw automation::ControlError(std::string(automation::BUSY),
                                               product() + " is busy with another recording.");
            }
            if (now.meeting) {
                throw automation::ControlError(std::string(automation::BUSY),
                                               "Meeting Mode is active; end the meeting first.");
            }
            if (!action.empty()) {
                enabled_action(*settings_.snapshot(), action);
            }
            // What the app says while it starts; its last word explains a refusal.
            std::vector<std::string> statuses;
            std::optional<std::string> started;
            {
                Subscription listening = host.status_updates().connect(
                    ui_, [&statuses](std::string text) { statuses.push_back(std::move(text)); });
                started = host.start_recording(
                    action.empty() ? std::nullopt : std::optional<std::string>(action), from);
            }
            if (!started) {
                throw refused(statuses.empty() ? product() + " couldn't start recording."
                                               : statuses.back());
            }
            return *started;
        });
}

void UiAppControl::stop_recording() {
    call_on_ui([](ControlHost& host) {
        if (!host.stop_recording()) {
            throw refused("Nothing is recording.");
        }
    });
}

void UiAppControl::cancel() {
    call_on_ui([](ControlHost& host) {
        if (!host.cancel()) {
            throw refused("Nothing is recording or transcribing.");
        }
    });
}

void UiAppControl::notify(std::string_view text, std::string_view title) {
    call_on_ui([words = std::string(text), heading = std::string(title)](ControlHost& host) {
        const std::string line = one_line(words);
        if (line.empty()) {
            // The MCP tool only requires one character, which may be a space.
            throw refused("There's nothing to show.");
        }
        const std::string label = one_line(heading);
        host.notify(label.empty() ? line : label + ": " + line);
    });
}

} // namespace fv::agentapi
