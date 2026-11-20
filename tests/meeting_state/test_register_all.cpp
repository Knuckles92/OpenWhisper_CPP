// The review, custom-report and fast ops in the op vocabulary (register_all.h), with the domains
// store.py's _OP_DOMAINS gives them.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "meeting_state/patches.h"
#include "meeting_state/register_all.h"

using namespace fv::meeting_state;

TEST_CASE("register_all: every extra op is registered with Python's domains and no gate flag",
          "[meeting_state_register]") {
    // Registered as the registry is built; asking again changes nothing.
    register_meeting_ops();
    register_meeting_ops();
    const OpDomains cards_and_review{.cards = true, .review = true};
    const OpDomains review{.review = true};
    const OpDomains reports{.reports = true};
    const std::vector<std::pair<std::string, OpDomains>> expected = {
        // ("cards", "insight_review")
        {"review_finish", cards_and_review},
        {"review_answer", cards_and_review},
        {"citation_check", cards_and_review},
        {"invalidate_citations", cards_and_review},
        // ("insight_review",)
        {"review_unavailable", review},
        {"review_begin", review},
        {"review_skip", review},
        {"review_reopen", review},
        // ("custom_reports",)
        {"request_custom_report", reports},
        {"finish_custom_report", reports},
        {"remove_custom_report", reports},
        // ("live_highlights",)
        {"publish_highlights", OpDomains{.highlights = true}},
        // () in Python, which reassigns the field on a shallow copy; here the domain names what
        // the handler writes.
        {"voice_feedback", OpDomains{.feedback = true}},
    };
    for (const auto& [name, domains] : expected) {
        INFO(name);
        REQUIRE(op_domain(name).has_value());
        CHECK(*op_domain(name) == domains);
        REQUIRE(op_flags(name).has_value());
        CHECK_FALSE(op_flags(name)->agent);
        CHECK_FALSE(op_flags(name)->agent_only);
        CHECK_FALSE(op_flags(name)->host_only);
        CHECK_FALSE(op_flags(name)->segment);
        CHECK(all_ops().contains(name));
    }
}
