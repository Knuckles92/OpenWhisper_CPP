#include "automation/dictation_bridge.h"

#include <string>
#include <utility>

#include "automation/action_model.h"
#include "automation/routing.h"

namespace fv::automation {

DictationAutomation::DictationAutomation(ActionRuntime& actions, FilterLeases& leases,
                                         llm::JudgeProvider& judges, EventSink& events,
                                         std::shared_ptr<http::Client> http, FilterRunOnce run_once)
    : actions_(actions), leases_(leases), judges_(judges), events_(events),
      // StageRun posts http filters through it without a check: never null.
      http_(http ? std::move(http) : http::make_curl_client()), run_once_(std::move(run_once)) {}

std::unique_ptr<dictation::FilterStageRun>
DictationAutomation::filter_stage(std::string_view stage, const Json& settings,
                                  const CancelToken& cancel) {
    return std::make_unique<StageRun>(
        std::string(stage), std::optional<Json>(std::in_place, settings),
        FilterContext{leases_, http_, events_, run_once_, {}}, cancel);
}

dictation::RouteDecision DictationAutomation::route(std::string_view text, const Json& settings,
                                                    std::string_view route,
                                                    const CancelToken& cancel,
                                                    std::string_view job_id) {
    const RouteDecision decision =
        decide(text, settings, route, judges_, events_, cancel, job_id, judges_owner_);
    return dictation::RouteDecision{decision.action_id, decision.payload, decision.confidence,
                                    decision.failure};
}

std::optional<dictation::RecordingAction> DictationAutomation::find_action(const Json& settings,
                                                                           std::string_view id) {
    const std::optional<Action> action = automation::find_action(settings, id);
    if (!action) {
        return std::nullopt;
    }
    return to_recording_action(*action);
}

bool DictationAutomation::program_words_allowed(const dictation::RecordingAction& action,
                                                const Json& settings) {
    return automation::program_words_allowed(std::string_view(action.kind), settings);
}

bool DictationAutomation::accepting() {
    return actions_.accepting();
}

void DictationAutomation::deliver(std::shared_ptr<const dictation::DictationJob> job,
                                  dictation::ActionDelivery delivery) {
    actions_.deliver(std::move(job), std::move(delivery.text), std::move(delivery.raw_text),
                     std::move(delivery.html), std::move(delivery.notice));
}

std::vector<dictation::ActionShortcut> DictationAutomation::action_hotkeys(const Json& settings) {
    std::vector<dictation::ActionShortcut> shortcuts;
    for (const Action& action : load_actions(settings)) {
        shortcuts.push_back({action.id, action.name, action.hotkey, action.enabled});
    }
    return shortcuts;
}

} // namespace fv::automation
