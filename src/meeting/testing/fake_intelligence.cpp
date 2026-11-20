#include "meeting/testing/fake_intelligence.h"

#include <stdexcept>
#include <utility>

#include "pyport/errors.h"

namespace fv::meeting {

void FakeIntelligence::set_available(bool available) {
    const std::lock_guard lock(mutex_);
    available_ = available;
}

void FakeIntelligence::set_live(bool live) {
    const std::lock_guard lock(mutex_);
    live_ = live;
}

std::vector<std::shared_ptr<meeting_agent::testing::FakeAgentCore>>
FakeIntelligence::live_cores() const {
    const std::lock_guard lock(mutex_);
    return live_cores_;
}

std::shared_ptr<meeting_agent::AgentCore> FakeIntelligence::create_live_core(const RerunContext&,
                                                                             const CancelToken&) {
    const std::lock_guard lock(mutex_);
    if (!live_) {
        return {};
    }
    live_cores_.push_back(std::make_shared<meeting_agent::testing::FakeAgentCore>());
    return live_cores_.back();
}

void FakeIntelligence::script_polish(Json outcome) {
    const std::lock_guard lock(mutex_);
    polish_script_.push_back(std::move(outcome));
}

void FakeIntelligence::script_consolidate(Json outcome) {
    const std::lock_guard lock(mutex_);
    consolidate_script_.push_back(std::move(outcome));
}

std::vector<std::string> FakeIntelligence::polished() const {
    const std::lock_guard lock(mutex_);
    return polished_;
}

std::vector<std::string> FakeIntelligence::consolidated() const {
    const std::lock_guard lock(mutex_);
    return consolidated_;
}

RerunContext FakeIntelligence::last_context() const {
    const std::lock_guard lock(mutex_);
    return last_context_;
}

bool FakeIntelligence::available() const {
    const std::lock_guard lock(mutex_);
    return available_;
}

Json FakeIntelligence::next(std::deque<Json>& script) {
    if (script.empty()) {
        return Json{{"status", "completed"}, {"message", ""}};
    }
    Json outcome = std::move(script.front());
    script.pop_front();
    return outcome;
}

Json FakeIntelligence::polish(const RerunContext& context, const CancelToken&) {
    Json outcome;
    {
        const std::lock_guard lock(mutex_);
        polished_.push_back(context.meeting_id);
        last_context_ = context;
        outcome = next(polish_script_);
    }
    if (outcome.is_object() && outcome.contains("throw")) {
        throw std::runtime_error(outcome["throw"].get<std::string>());
    }
    if (outcome.is_object() && outcome.contains("value_error")) {
        throw py::ValueError(outcome["value_error"].get<std::string>());
    }
    return outcome;
}

Json FakeIntelligence::consolidate(const RerunContext& context, const CancelToken&) {
    Json outcome;
    {
        const std::lock_guard lock(mutex_);
        consolidated_.push_back(context.meeting_id);
        last_context_ = context;
        outcome = next(consolidate_script_);
    }
    if (outcome.is_object() && outcome.contains("throw")) {
        throw std::runtime_error(outcome["throw"].get<std::string>());
    }
    if (outcome.is_object() && outcome.contains("value_error")) {
        throw py::ValueError(outcome["value_error"].get<std::string>());
    }
    return outcome;
}

} // namespace fv::meeting
