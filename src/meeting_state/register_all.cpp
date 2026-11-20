#include "meeting_state/register_all.h"

#include "meeting_state/custom_report_ops.h"
#include "meeting_state/detail/meeting_ops.h"
#include "meeting_state/fast_ops.h"
#include "meeting_state/patches.h"
#include "meeting_state/review_ops.h"

namespace fv::meeting_state {

namespace detail {

void add_meeting_ops(const OpAdder& add) {
    // Gate flags all off: none is an agent op, and each handler checks its own actor (a guest
    // asking for a report is the handler's "host_only").
    const OpFlags own_gate{};
    // Answers and verdicts rewrite cards (the item, its user note, clarified note blocks) as
    // well as the review.
    const OpDomains review_and_cards{.cards = true, .review = true};
    const OpDomains review{.review = true};
    add("review_unavailable", review_unavailable, review, own_gate);
    add("review_begin", review_begin, review, own_gate);
    add("review_finish", review_finish, review_and_cards, own_gate);
    add("review_answer", review_answer, review_and_cards, own_gate);
    add("review_skip", review_skip, review, own_gate);
    add("review_reopen", review_reopen, review, own_gate);

    add("publish_highlights", publish_highlights, {.highlights = true}, own_gate);
    add("citation_check", citation_check, review_and_cards, own_gate);
    add("invalidate_citations", invalidate_citations, review_and_cards, own_gate);
    add("voice_feedback", voice_feedback, {.feedback = true}, own_gate);

    const OpDomains reports{.reports = true};
    add("request_custom_report", request_custom_report, reports, own_gate);
    add("finish_custom_report", finish_custom_report, reports, own_gate);
    add("remove_custom_report", remove_custom_report, reports, own_gate);
}

} // namespace detail

void register_meeting_ops() {
    // The registry adds them as it is built (patches.cpp); reading it builds it.
    (void)op_domain("review_begin");
}

} // namespace fv::meeting_state
