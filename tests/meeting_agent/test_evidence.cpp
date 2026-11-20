// Tolerant evidence-id repair: the evidence tests of tests/test_meeting_repair.py.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "meeting_agent/evidence.h"
#include "meeting_agent/testing/fake_tool_host.h"
#include "meeting_agent/tool_policy.h"

using fv::Json;
using namespace fv::meeting_agent;

namespace {

const std::vector<std::string> kKnown = {
    "sg_5f2c721de88ac0aa2321",
    "sg_e991e94d6decd27d036a",
    "sg_025f2c0b2c30e3e8dcb3",
    "sg_5f2c721de88ac0aa2329",
};

bool exists(std::string_view id) {
    return std::ranges::find(kKnown, id) != kKnown.end();
}

std::pair<Json, int> repair(const Json& ops, const std::vector<std::string>& citable = kKnown) {
    return repair_evidence_ids(ops, citable, exists);
}

Json op_citing(const char* op, const Json& evidence) {
    return Json{{"op", op}, {"evidence", evidence}};
}

} // namespace

// from tests/test_meeting_repair.py::test_truncated_id_repaired_to_unique_prefix
TEST_CASE("test_truncated_id_repaired_to_unique_prefix", "[test_meeting_repair]") {
    const auto [ops, count] =
        repair(Json::array({op_citing("add_item", Json::array({"sg_e991e94d6decd27d03"}))}));
    CHECK(count == 1);
    CHECK(ops[0]["evidence"] == Json::array({"sg_e991e94d6decd27d036a"}));
}

// from tests/test_meeting_repair.py::test_one_char_typo_repaired_by_fuzzy_match
TEST_CASE("test_one_char_typo_repaired_by_fuzzy_match", "[test_meeting_repair]") {
    const auto [ops, count] =
        repair(Json::array({op_citing("set_topic", Json::array({"sg_025f2c0b2c30e3e8dcb4"}))}));
    CHECK(count == 1);
    CHECK(ops[0]["evidence"] == Json::array({"sg_025f2c0b2c30e3e8dcb3"}));
}

// from tests/test_meeting_repair.py::test_reconstructed_id_with_several_wrong_chars_repaired
TEST_CASE("test_reconstructed_id_with_several_wrong_chars_repaired", "[test_meeting_repair]") {
    // A model rebuilding an id from memory with 4 to 6 wrong characters mid-string scores about
    // 0.74 against the intended id while every other id sits below 0.4, so the margin guard
    // still identifies it.
    auto citable = kKnown;
    citable.emplace_back("sg_97bd44598ee4da9dc5f9");
    const auto [ops, count] = repair(
        Json::array({op_citing("set_topic", Json::array({"sg_97bd44598ee4f6c7091f"}))}), citable);
    CHECK(count == 1);
    CHECK(ops[0]["evidence"] == Json::array({"sg_97bd44598ee4da9dc5f9"}));
}

// from tests/test_meeting_repair.py::test_ambiguous_mid_string_corruption_left_alone
TEST_CASE("test_ambiguous_mid_string_corruption_left_alone", "[test_meeting_repair]") {
    // Two citable ids differ only in their tail: a corrupted id between them is not guessed.
    const auto [ops, count] =
        repair(Json::array({op_citing("set_topic", Json::array({"sg_5f2c921de38ac0af2321"}))}));
    CHECK(count == 0);
    CHECK(ops[0]["evidence"] == Json::array({"sg_5f2c921de38ac0af2321"}));
}

// from tests/test_meeting_repair.py::test_ambiguous_prefix_left_alone
TEST_CASE("test_ambiguous_prefix_left_alone", "[test_meeting_repair]") {
    const auto [ops, count] =
        repair(Json::array({op_citing("add_item", Json::array({"sg_5f2c721de88ac0aa23"}))}));
    CHECK(count == 0);
    CHECK(ops[0]["evidence"] == Json::array({"sg_5f2c721de88ac0aa23"}));
}

// from tests/test_meeting_repair.py::test_invented_id_left_for_honest_rejection
TEST_CASE("test_invented_id_left_for_honest_rejection", "[test_meeting_repair]") {
    const auto [ops, count] =
        repair(Json::array({op_citing("add_item", Json::array({"sg_ffffffffffffffffffff"}))}));
    CHECK(count == 0);
    CHECK(ops[0]["evidence"] == Json::array({"sg_ffffffffffffffffffff"}));
}

// from tests/test_meeting_repair.py::test_existing_and_non_sg_ids_untouched
TEST_CASE("test_existing_and_non_sg_ids_untouched", "[test_meeting_repair]") {
    // Known ids and unknown non-sg ids are kept; non-strings are dropped.
    const auto [ops, count] =
        repair(Json::array({op_citing("add_item", Json::array({kKnown[0], "q_123", 7}))}));
    CHECK(count == 0);
    CHECK(ops[0]["evidence"] == Json::array({kKnown[0], "q_123"}));
}

// from tests/test_meeting_repair.py::test_repair_counts_each_id_once_and_caches
TEST_CASE("test_repair_counts_each_id_once_and_caches", "[test_meeting_repair]") {
    const Json input =
        Json::array({op_citing("add_item", Json::array({"sg_025f2c0b2c30e3e8dcb4"})),
                     op_citing("add_item", Json::array({"sg_025f2c0b2c30e3e8dcb4"}))});
    const auto [ops, count] = repair(input);
    CHECK(count == 2);
    for (const auto& op : ops) {
        CHECK(op["evidence"] == Json::array({"sg_025f2c0b2c30e3e8dcb3"}));
    }
    // The input is never changed.
    CHECK(input[0]["evidence"] == Json::array({"sg_025f2c0b2c30e3e8dcb4"}));
}

// from tests/test_meeting_repair.py::test_tool_policy_repairs_before_dispatch
TEST_CASE("test_tool_policy_repairs_before_dispatch", "[test_meeting_repair]") {
    testing::FakeToolHost host;
    host.set_known_segments({kKnown.begin(), kKnown.end()});
    ToolScope scope;
    scope.citable_ids = kKnown;
    run_tool(host, "patch_state",
             {{"ops", Json::array({{{"op", "add_item"},
                                    {"card", "decisions"},
                                    {"text", "x"},
                                    {"evidence", Json::array({"sg_e991e94d6decd27d03"})}}})}},
             scope);
    REQUIRE(host.ops().size() == 1);
    CHECK(host.ops()[0]["evidence"] == Json::array({"sg_e991e94d6decd27d036a"}));
    run_tool(host, "ask_question",
             {{"text", "Who?"}, {"evidence", Json::array({"sg_e991e94d6decd27d03"})}}, scope);
    const auto questions = host.question_calls();
    REQUIRE(questions.size() == 1);
    CHECK(questions[0].evidence == Json::array({"sg_e991e94d6decd27d036a"}));

    // Without a citable universe (no payload) nothing is touched.
    const Json raw = Json::array({op_citing("add_item", Json::array({"sg_e991e94d6decd27d03"}))});
    CHECK(repair_evidence(host, raw, {}) == raw);
}
