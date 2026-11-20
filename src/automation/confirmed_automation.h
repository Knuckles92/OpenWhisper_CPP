#pragma once
#include "automation/action_runtime.h"
#include "automation/routing.h"
#include "dictation/automation_port.h"
#include <functional>

namespace fv::automation {
struct ActionDisclosure {
    std::string destination;
    std::string material;
};
// The confirmation callback may be called from the jobs thread for a routing judgment and on
// the UI thread for delivery. It must return false on cancellation or if it cannot display the
// complete destination and material. No saved filters run through this port.
class ConfirmedAutomation final : public dictation::AutomationPort {
public:
    using Confirm = std::function<bool(const ActionDisclosure&, const CancelToken&)>;
    using JudgeDestination = std::function<std::string(const Json&)>;
    using JudgeRun =
        std::function<dictation::RouteDecision(const RoutePlan&, const Json&, const CancelToken&)>;
    ConfirmedAutomation(dictation::AutomationPort& delegate, ActionRuntime& runtime,
                        SettingsStore& settings, Confirm confirm, JudgeDestination judge,
                        std::function<void(std::string)> refused, JudgeRun run = {});
    bool raw_before_consent() const override { return true; }
    std::unique_ptr<dictation::FilterStageRun> filter_stage(std::string_view, const Json&,
                                                            const CancelToken&) override;
    dictation::RouteDecision route(std::string_view, const Json&, std::string_view,
                                   const CancelToken&, std::string_view) override;
    std::optional<dictation::RecordingAction> find_action(const Json&, std::string_view) override;
    bool program_words_allowed(const dictation::RecordingAction&, const Json&) override;
    bool accepting() override;
    void deliver(std::shared_ptr<const dictation::DictationJob>,
                 dictation::ActionDelivery) override;
    std::vector<dictation::ActionShortcut> action_hotkeys(const Json&) override;

private:
    dictation::AutomationPort& delegate_;
    ActionRuntime& runtime_;
    SettingsStore& settings_;
    Confirm confirm_;
    JudgeDestination judge_;
    std::function<void(std::string)> refused_;
    JudgeRun judge_run_;
};
} // namespace fv::automation
