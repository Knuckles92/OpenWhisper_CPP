#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "dictation/job.h"
#include "dictation/messages.h"

// What a dictation asks of automation (services/runtime/transcription.py's calls into
// services/automation/filters.py, services/actions/routing.py, services/actions/model.py and
// services/actions/runtime.py): text filters around cleanup, Route by voice, the saved actions and
// the action runtime that takes an Action job's words. fv_dictation cannot depend on
// fv_automation, which reads its jobs, so automation implements this port
// (automation::DictationAutomation) and the transcription runtime only calls it.
namespace fv::dictation {

// services/automation/models.py stage names: the filters run on the words before AI cleanup and
// on the finished words after it.
inline constexpr std::string_view BEFORE_CLEANUP = "before_cleanup";
inline constexpr std::string_view AFTER_CLEANUP = "after_cleanup";

// services/actions/routing.py RouteDecision: the action the words name and what to send it.
// Affinity: value type.
struct RouteDecision {
    // The action named; nullopt when the words name none (Python's decide returning None).
    std::optional<std::string> action_id;
    // The words for the action, without its name; blank sends the whole dictation.
    std::string payload;
    std::optional<double> confidence;
    // Why the judge could not answer ("timeout", "unreachable", ...); "" when it answered. A
    // failure pastes the dictation and Route by voice says so.
    std::string failure;

    bool operator==(const RouteDecision&) const = default;
};

// services/automation/filters.py FilterOutcome.
// Affinity: value type.
struct FilterResult {
    std::string text;
    // A filter answered that it handled the words itself: they are saved, never delivered.
    bool canceled = false;
    bool changed = false;
    // A filter a program leased over the live API (not one of the user's saved filters) changed
    // the words.
    bool program_changed = false;

    bool operator==(const FilterResult&) const = default;
};

// One stage's filters for one job (filters.StageRun), sharing one time budget across its parts.
// Each call returns what filters.apply would for its filters alone, and the text unchanged when
// none apply (a mode filters skip, blank text, a cancel already set). A call may throw; the
// runtime then keeps the words.
// Affinity: the transcription runtime's jobs strand, one call at a time.
class FilterStageRun {
public:
    virtual ~FilterStageRun() = default;
    // The user's saved filters; never program_changed.
    virtual FilterResult saved(std::string text, const DictationJob& job) = 0;
    // The filters programs leased, for `job` as it is now: a dictation Route by voice may send to
    // an action runs them after routing, so a lease sees where the words go and cannot steer
    // them.
    virtual FilterResult leased(std::string text, const DictationJob& job) = 0;
    // Both, in one run (filters.apply): the stage of a job that is not routed.
    virtual FilterResult apply(std::string text, const DictationJob& job) = 0;
};

// What an Action job hands the action runtime (ActionRuntime.deliver's arguments).
// Affinity: value type.
struct ActionDelivery {
    // The finished words.
    std::string text;
    // The words before AI cleanup changed them, or "" when it did not.
    std::string raw_text;
    // The rich-text twin of `text`, or "".
    std::string html;
    // Why the words are not what the action asked for (its transform could not run, its
    // profile's formatting failed), shown with the outcome; "" when nothing went wrong.
    std::string notice;
    // The recording's cancellation, checked again after any confirmation prompt.
    CancelToken cancel;

    bool operator==(const ActionDelivery& other) const {
        return text == other.text && raw_text == other.raw_text && html == other.html &&
               notice == other.notice;
    }
};

// One saved action's shortcut as load_actions reads it. Every action, enabled or not, takes part
// in the shortcut conflict check (hotkey_conflicts.py); only an enabled one is registered.
// Affinity: value type.
struct ActionShortcut {
    std::string id;
    std::string name;
    std::string hotkey;
    bool enabled = true;

    bool operator==(const ActionShortcut&) const = default;
};

// run_action_text's refusals (services/automation/control.py ControlError): the code the live
// API and MCP answer with and a message to show as it is.
inline constexpr std::string_view RUN_BUSY = "busy";
inline constexpr std::string_view RUN_NOT_FOUND = "not_found";
inline constexpr std::string_view RUN_UNAVAILABLE = "unavailable";
inline constexpr std::string_view RUN_REFUSED = "refused";

class RunRefused : public std::runtime_error {
public:
    RunRefused(std::string_view code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    const std::string& code() const noexcept { return code_; }
    std::string message() const { return what(); }

private:
    std::string code_;
};

// A program asked for an action that would hand its words to the user's coding agent, without
// the permission that allows it (permissions.PermissionDenied). what() is the settings key that
// was refused; automation answers with permissions::PermissionDenied for it.
class RunPermissionDenied : public std::runtime_error {
public:
    explicit RunPermissionDenied(const std::string& permission)
        : std::runtime_error(permission), permission_(permission) {}
    const std::string& permission() const noexcept { return permission_; }

private:
    std::string permission_;
};

// Implemented by automation::DictationAutomation (M5.15); null_automation_port() runs no filters,
// routes nothing and knows no action. A method may throw: the runtime treats a failing filter or
// judge as none and logs only the exception's type, never the words.
// Affinity: thread-safe. filter_stage and route run on the transcription runtime's jobs strand;
// find_action on either; the rest on the UI thread.
class AutomationPort {
public:
    virtual ~AutomationPort() = default;
    // A consent-bound port must not send words to shaping providers before approval.
    virtual bool raw_before_consent() const { return false; }
    // The filters of `stage` (BEFORE_CLEANUP or AFTER_CLEANUP), or null when none could apply.
    // `cancel` is the job's: a Cancel ends the wait on a filter at once.
    virtual std::unique_ptr<FilterStageRun>
    filter_stage(std::string_view stage, const Json& settings, const CancelToken& cancel) = 0;
    // routing.decide: which action the dictated `text` names. `route` is the job's ("hotkey" or
    // "auto"). Blocking, at most the judge's budget; `cancel` ends it.
    virtual RouteDecision route(std::string_view text, const Json& settings, std::string_view route,
                                const CancelToken& cancel, std::string_view job_id) = 0;
    // model.find_action: the saved action with this id, enabled or not, with its id, shaping and
    // enabled state filled; nullopt when there is none.
    virtual std::optional<RecordingAction> find_action(const Json& settings,
                                                       std::string_view id) = 0;
    // runtime.program_words_allowed: whether the action takes words a program supplied (an agent
    // or channel action needs "Send words to my coding agents").
    virtual bool program_words_allowed(const RecordingAction& action, const Json& settings) = 0;
    // ActionRuntime.accepting: the action runtime takes new runs.
    virtual bool accepting() = 0;
    // ActionRuntime.deliver: an Action job's words go to its action, which shows its own
    // outcome.
    virtual void deliver(std::shared_ptr<const DictationJob> job, ActionDelivery delivery) = 0;
    // The saved actions' shortcuts (load_actions), in order.
    virtual std::vector<ActionShortcut> action_hotkeys(const Json& settings) = 0;
};

// No filters, no routing, no actions: dictation as it was before automation.
AutomationPort& null_automation_port();

} // namespace fv::dictation
