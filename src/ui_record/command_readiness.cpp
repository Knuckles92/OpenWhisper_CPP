#include "ui_record/command_readiness.h"

#include <exception>
#include <typeinfo>
#include <utility>

#include "base/log.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.quick_record_tab"};

} // namespace

// Shared with the tasks in flight, so a task that lands after the object went finds `alive`
// false instead of a dangling pointer. Touched on the GUI thread only; the probe is the one thing
// that runs elsewhere, and it is a copy.
struct CommandHintReadiness::State {
    Probe probe;
    Executor* worker = nullptr;
    Executor* ui = nullptr;
    std::function<void()> changed;
    std::optional<bool> known;
    bool checking = false;
    // The settings asked about while a check ran: they are checked when it ends.
    std::optional<Json> later;
    // The settings of the check running, and of the answer in `known`.
    std::optional<Json> inflight;
    std::optional<Json> answered;
    // True while `changed` runs: the refresh it causes reads the answer that just landed, and
    // asks again only for settings no check has seen, so one change costs one credential read.
    bool delivering = false;
    bool alive = true;

    // Whether the refresh `changed` caused asks about settings already checked or being checked.
    bool alreadyAsked(const Json& settings) const {
        return (answered && *answered == settings) ||
               (checking && inflight && *inflight == settings);
    }

    void start(const std::shared_ptr<State>& self, Json settings) {
        checking = true;
        inflight = settings;
        worker->post([self, probe = probe, settings = std::move(settings)] {
            std::optional<bool> answer;
            bool failed = false;
            try {
                answer = probe(settings);
            } catch (const std::exception& error) {
                kLog.warning("Couldn't check the AI model for Command Mode ({})",
                             typeid(error).name());
                failed = true;
            }
            self->ui->post([self, answer, failed] { self->land(self, answer, failed); });
        });
    }

    void land(const std::shared_ptr<State>& self, std::optional<bool> answer, bool failed) {
        if (!alive) {
            return;
        }
        checking = false;
        // A failed check keeps the last answer, so a hiccup does not flip the hint.
        const bool different = !failed && known != answer;
        if (different) {
            known = answer;
        }
        if (!failed) {
            answered = std::move(inflight);
        }
        inflight.reset();
        if (later) {
            Json settings = std::move(*later);
            later.reset();
            start(self, std::move(settings));
        }
        if (different && changed) {
            delivering = true;
            try {
                changed();
            } catch (...) {
                delivering = false;
                throw;
            }
            delivering = false;
        }
    }
};

CommandHintReadiness::CommandHintReadiness(Probe probe, Executor& worker, Executor& ui)
    : state_(std::make_shared<State>()) {
    state_->probe = std::move(probe);
    state_->worker = &worker;
    state_->ui = &ui;
}

CommandHintReadiness::~CommandHintReadiness() {
    state_->alive = false;
    state_->changed = nullptr;
}

void CommandHintReadiness::setChanged(std::function<void()> changed) {
    state_->changed = std::move(changed);
}

QuickRecordTab::CommandModeReadiness CommandHintReadiness::supplier() {
    return [state = state_](const Json& settings) -> std::optional<bool> {
        if (!state->alive) {
            return std::nullopt;
        }
        if (state->delivering && state->alreadyAsked(settings)) {
            return state->known;
        }
        if (state->checking) {
            state->later = settings;
        } else {
            state->start(state, settings);
        }
        return state->known;
    };
}

} // namespace fv
