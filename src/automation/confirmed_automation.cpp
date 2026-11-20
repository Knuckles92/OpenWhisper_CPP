#include "automation/confirmed_automation.h"
#include "automation/routing.h"
#include "focus/capture_service.h"
#include "settings/keys.h"
#include <utility>

namespace fv::automation {
namespace {
std::string param(const Action& action, std::string_view name) {
    const auto value = action.param(name);
    return value.is_string() ? value.get<std::string>() : std::string();
}
ActionDisclosure disclose(const Action& action, const dictation::DictationJob& job,
                          const dictation::ActionDelivery& delivery, const Json& settings) {
    ActionDisclosure result{action.name, delivery.text};
    if (action.kind == ActionKind::WEBHOOK) {
        result.destination += "\n" + param(action, "url");
        result.material =
            webhook_payload(job, action, delivery.text, delivery.raw_text, settings).dump(2);
    } else if (action.kind == ActionKind::OPEN_LINK) {
        result.destination += "\n" + expand_link(param(action, "template"), delivery.text);
    } else if (action.kind == ActionKind::PROGRAM) {
        result.destination += "\n" + param(action, "program");
        result.destination += "\nArguments: " + action.param("args").dump();
    } else if (action.kind == ActionKind::AGENT) {
        const auto agent = param(action, "agent");
        result.destination += "\n" + std::string(agent_display_name(agent).value_or(agent));
        if (const auto model = param(action, "model"); !model.empty())
            result.destination += "\nModel: " + model;
        result.destination += "\nMode: " + param(action, "mode");
        if (const auto folder = param(action, "folder"); !folder.empty())
            result.destination += "\nFolder: " + folder;
        const auto include = action.param("include_selection");
        if (include.is_boolean() && include.get<bool>())
            result.material = prompt_with_context(delivery.text, job.snapshot());
    } else if (action.kind == ActionKind::CHANNEL) {
        result.destination += "\nConnected Claude Code sessions";
    } else if (action.kind == ActionKind::SCRIPTS) {
        result.destination += "\nConnected scripts";
    }
    return result;
}
} // namespace
ConfirmedAutomation::ConfirmedAutomation(dictation::AutomationPort& delegate,
                                         ActionRuntime& runtime, SettingsStore& settings,
                                         Confirm confirm, JudgeDestination judge,
                                         std::function<void(std::string)> refused, JudgeRun run)
    : delegate_(delegate), runtime_(runtime), settings_(settings), confirm_(std::move(confirm)),
      judge_(std::move(judge)), refused_(std::move(refused)), judge_run_(std::move(run)) {}
std::unique_ptr<dictation::FilterStageRun>
ConfirmedAutomation::filter_stage(std::string_view, const Json&, const CancelToken&) {
    return {};
}
dictation::RouteDecision ConfirmedAutomation::route(std::string_view text, const Json& settings,
                                                    std::string_view route,
                                                    const CancelToken& cancel,
                                                    std::string_view job_id) {
    if (route != "hotkey" || !keys::voice_routing_enabled.read(settings) || cancel.cancelled() ||
        !confirm_ || !judge_)
        return {};
    std::vector<Action> enabled;
    for (auto& action : load_actions(settings))
        if (action.enabled)
            enabled.push_back(std::move(action));
    const auto request = automation::plan(text, enabled, route);
    if (!request)
        return {};
    const auto destination = judge_(settings);
    const auto material =
        Json{{"state", request->state}, {"questions", request->questions}}.dump(2);
    if (destination.empty() || !confirm_({destination, material}, cancel) || cancel.cancelled())
        return {.failure = "consent_declined"};
    if (judge_run_)
        return judge_run_(*request, settings, cancel);
    return delegate_.route(text, settings, route, cancel, job_id);
}
std::optional<dictation::RecordingAction> ConfirmedAutomation::find_action(const Json& s,
                                                                           std::string_view id) {
    auto action = delegate_.find_action(s, id);
    // Shaping can invoke an external model. No transcript has been disclosed to the user for
    // approval at recording start, so this explicit path sends the dictated words unshaped.
    if (action)
        action->shaping = std::string(dictation::SHAPING_RAW);
    return action;
}
bool ConfirmedAutomation::program_words_allowed(const dictation::RecordingAction& a,
                                                const Json& s) {
    return delegate_.program_words_allowed(a, s);
}
bool ConfirmedAutomation::accepting() {
    return runtime_.accepting();
}
std::vector<dictation::ActionShortcut> ConfirmedAutomation::action_hotkeys(const Json& s) {
    return delegate_.action_hotkeys(s);
}
void ConfirmedAutomation::deliver(std::shared_ptr<const dictation::DictationJob> job,
                                  dictation::ActionDelivery delivery) {
    if (!job || !dictation::user_started(job->source) || delivery.cancel.cancelled())
        return;
    auto snapshot = settings_.snapshot();
    auto action = automation::find_action(*snapshot, job->action_id);
    if (!action || !action->enabled) {
        if (refused_)
            refused_("That action is unavailable.");
        return;
    }
    auto frozen = std::make_shared<dictation::DictationJob>(*job);
    if (const auto focus = job->snapshot())
        frozen->focus = focus::FocusRequest::resolved(*focus);
    else
        frozen->focus.reset();
    // A listener list does not identify individual script recipients, so fail closed rather than
    // present an ambiguous destination for program-supplied text.
    if (action->kind == ActionKind::SCRIPTS ||
        (action->kind != ActionKind::CLIPBOARD &&
         (!confirm_ ||
          !confirm_(disclose(*action, *frozen, delivery, *snapshot), delivery.cancel))) ||
        delivery.cancel.cancelled()) {
        if (refused_)
            refused_("Action canceled.");
        return;
    }
    runtime_.deliver_approved(std::move(frozen), std::move(*action), std::move(snapshot),
                              std::move(delivery.text), std::move(delivery.raw_text),
                              std::move(delivery.html), std::move(delivery.notice));
}
} // namespace fv::automation
