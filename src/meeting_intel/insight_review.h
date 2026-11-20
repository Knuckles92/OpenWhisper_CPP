#pragma once

#include <functional>
#include <memory>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "llm/judge.h"
#include "meeting_state/repository.h"
#include "meeting_state/store.h"

namespace fv::meeting_intel {

// insight_review.REVIEW_TIMEOUT_S: the least time a review gives the judge for one question.
inline constexpr double REVIEW_TIMEOUT_S = 12.0;

struct InsightReviewDeps {
    // The judge to check with, built with its own per-question budget of at least `at_least_s`
    // (insight_review._default_reviewer: judge_from_settings(timeout_s=max(REVIEW_TIMEOUT_S,
    // route.timeout_s))); the review asks it without a timeout of its own. Null: no judge.
    std::function<std::shared_ptr<llm::Judge>(double at_least_s)> judge;
    std::function<bool()> allowed;
    const Clock* clock = nullptr;
};

// insight_review._default_reviewer over OpenWhisper_CPP's rule that meeting judgments stay on this
// computer (a29-local-meeting-judgments): null unless Fast judgments are on and `route` (the
// chosen one) is device-local; else `build` with a per-question budget of
// max(at_least_s, route.timeout_s).
std::shared_ptr<llm::Judge> build_review_judge(
    const Json& settings, const llm::JudgeRoute& route, double at_least_s,
    const std::function<std::shared_ptr<llm::Judge>(const llm::JudgeOptions&)>& build);

// Claims and checks a saved meeting on the caller's worker thread. Only a local Judge can
// receive evidence. The state store owns conditional publication by run ID and item revision.
Json run_insight_review(meeting_state::MeetingStateStore& store,
                        meeting_state::MeetingRepository& repository, const InsightReviewDeps& deps,
                        const CancelToken& cancel = {});

} // namespace fv::meeting_intel
