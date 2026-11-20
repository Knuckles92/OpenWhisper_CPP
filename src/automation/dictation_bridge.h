#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "automation/action_runtime.h"
#include "automation/filters.h"
#include "base/cancel.h"
#include "base/events.h"
#include "base/json.h"
#include "dictation/automation_port.h"
#include "dictation/job.h"
#include "dictation/messages.h"
#include "http/client.h"
#include "llm/judge.h"

// What a dictation asks of automation, answered by automation (the transcription runtime's
// calls into services/automation/filters.py, services/actions/routing.py, model.py and
// runtime.py): text filters around cleanup, Route by voice through the Fast judgments judge, the
// saved actions and the action runtime that takes an Action job's words.
namespace fv::automation {

// Affinity: thread-safe as dictation::AutomationPort says: filter_stage and route on the jobs
// strand, deliver and accepting as ActionRuntime allows, the rest anywhere.
class DictationAutomation final : public dictation::AutomationPort {
public:
    // `http` posts to http filters (filters' FilterContext; null: a curl client of its own with
    // the system proxy); `run_once` starts program filters (empty: fv::run_once). Everything
    // given must outlive this object; `judges` must also outlive a judge still asked after a
    // dictation stopped waiting for it (routing.h decide).
    DictationAutomation(ActionRuntime& actions, FilterLeases& leases, llm::JudgeProvider& judges,
                        EventSink& events, std::shared_ptr<http::Client> http = nullptr,
                        FilterRunOnce run_once = {});

    // A StageRun over the saved filters and the live leases; it returns the text unchanged
    // when none apply.
    std::unique_ptr<dictation::FilterStageRun>
    filter_stage(std::string_view stage, const Json& settings, const CancelToken& cancel) override;
    // routing.decide; never throws.
    dictation::RouteDecision route(std::string_view text, const Json& settings,
                                   std::string_view route, const CancelToken& cancel,
                                   std::string_view job_id) override;
    std::optional<dictation::RecordingAction> find_action(const Json& settings,
                                                          std::string_view id) override;
    bool program_words_allowed(const dictation::RecordingAction& action,
                               const Json& settings) override;
    bool accepting() override;
    void deliver(std::shared_ptr<const dictation::DictationJob> job,
                 dictation::ActionDelivery delivery) override;
    std::vector<dictation::ActionShortcut> action_hotkeys(const Json& settings) override;

    // Held by each judge still being asked (decide's keep_alive): what `judges` needs when it is
    // not the process-wide provider. Set before the first route.
    void keep_judges_alive(std::shared_ptr<const void> owner) { judges_owner_ = std::move(owner); }

private:
    ActionRuntime& actions_;
    FilterLeases& leases_;
    llm::JudgeProvider& judges_;
    EventSink& events_;
    std::shared_ptr<http::Client> http_;
    FilterRunOnce run_once_;
    std::shared_ptr<const void> judges_owner_;
};

} // namespace fv::automation
