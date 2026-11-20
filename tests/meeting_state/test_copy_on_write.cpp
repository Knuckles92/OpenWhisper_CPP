// Mutation isolation and metadata reconciliation for state write-through
// (tests/test_meeting_state_copy_on_write.py).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>

#include "base/testing/fake_clock.h"
#include "meeting_state/patches.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "meeting_state/testing/fixed_ids.h"
#include "pyport/errors.h"
#include "stub_repository.h"

using fv::Json;
using namespace fv::meeting_state;
using fv::meeting_state::test_support::StubRepository;

namespace {

// RejectingRepository: every write fails.
std::shared_ptr<testing::FakeMeetingRepository> rejecting(const fv::Clock& clock) {
    auto repo = std::make_shared<testing::FakeMeetingRepository>(clock);
    repo->fail_on_ops_applied("storage unavailable");
    repo->fail_persist_state("storage unavailable");
    return repo;
}

MeetingState state_for(const std::string& meeting_id) {
    MeetingState state;
    state.meeting_id = meeting_id;
    state.cloud_enabled = true;
    return state;
}

CardItem item(const std::string& id, const std::string& text) {
    CardItem card;
    card.id = id;
    card.card = "key_points";
    card.text = text;
    card.created_at = "2026-01-01T00:00:00Z";
    card.updated_at = "2026-01-01T00:00:00Z";
    return card;
}

} // namespace

// from
// tests/test_meeting_state_copy_on_write.py::test_mutated_domains_do_not_leak_when_persistence_fails
TEST_CASE("test_mutated_domains_do_not_leak_when_persistence_fails",
          "[test_meeting_state_copy_on_write]") {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingState state = state_for("m_isolated");
    CardItem budget = item("it_budget", "Budget is pending");
    budget.data = {{"nested", {{"amount", 100}}}};
    state.cards["key_points"].push_back(budget);
    Participant person;
    person.id = "p_person";
    person.display_name = "Before";
    state.participants.set("p_person", person);
    Question question;
    question.id = "q_question";
    question.text = "Budget?";
    state.questions.set("q_question", question);
    CustomReport report;
    report.id = "r_report";
    report.request = "Budget";
    report.status = "running";
    report.run_id = "run";
    state.custom_reports.push_back(report);
    MeetingStateStore store(state, rejecting(clock), {}, clock, ids);
    const Json before = store.snapshot();
    Json batch = Json::parse(R"([
        {"op": "update_item", "id": "it_budget", "base_revision": 1,
         "set": {"text": "Budget approved", "data": {"nested": {"amount": 200}}}},
        {"op": "rename_participant", "participant_id": "p_person", "display_name": "After"},
        {"op": "dismiss_question", "question_id": "q_question"},
        {"op": "set_topic", "text": "New topic"}])");
    // The custom-report ops join the vocabulary with the report module.
    if (op_domain("remove_custom_report")) {
        batch.push_back({{"op", "remove_custom_report"}, {"report_id", "r_report"}});
    }
    for (const auto& result : store.apply("host", "p_person", batch)) {
        CHECK_FALSE(result.ok);
        CHECK(result.reason == "persistence_error");
    }
    CHECK(store.snapshot() == before);
    CHECK_FALSE(store.update_runtime_fields({{"capture", {{"mic_available", true}}}}));
    CHECK(store.snapshot() == before);
}

// from
// tests/test_meeting_state_copy_on_write.py::test_runtime_finalization_invalidation_rolls_back_with_failed_write
TEST_CASE("test_runtime_finalization_invalidation_rolls_back_with_failed_write",
          "[test_meeting_state_copy_on_write]") {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingState state = state_for("m_review");
    CardItem fact = item("it_fact", "Fact");
    fact.citation_check = {{"ok", true}};
    fact.review = {{"state", "checked"}};
    state.cards["key_points"].push_back(fact);
    state.insight_review = Json::parse(
        R"({"enabled": true, "status": "ready", "questions": [{"id": "review_q", "status": "open"}]})");
    int invalidations = 0;
    set_invalidate_checks([&](MeetingState& candidate) {
        ++invalidations;
        candidate.insight_review["status"] = "stale";
        candidate.cards["key_points"][0].citation_check = Json::object();
    });
    MeetingStateStore store(state, rejecting(clock), {}, clock, ids);
    const Json before = store.snapshot();
    CHECK_FALSE(store.update_runtime_fields({{"finalization", {{"status", "running"}}}}));
    CHECK(invalidations == 1);
    CHECK(store.snapshot() == before);
    set_invalidate_checks({});
}

// from
// tests/test_meeting_state_copy_on_write.py::test_repaired_item_provenance_flag_rolls_back_with_failed_write
TEST_CASE("test_repaired_item_provenance_flag_rolls_back_with_failed_write",
          "[test_meeting_state_copy_on_write]") {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingState state = state_for("m_repair");
    CardItem repaired = item("it_repair", "Original snippet");
    repaired.author_type = "system";
    repaired.author_id = "state_repair";
    repaired.data = {{"source", "audio"}};
    repaired.evidence = {"sg_audio"};
    state.cards["key_points"].push_back(repaired);
    MeetingStateStore store(state, rejecting(clock), {}, clock, ids);
    const Json before = store.snapshot();
    const auto result = store.apply("agent", "core", Json::parse(R"([{"op": "update_item",
        "id": "it_repair", "base_revision": 1, "set": {"text": "Synthesized insight"},
        "evidence": ["sg_audio"]}])"))[0];
    CHECK_FALSE(result.ok);
    CHECK(result.reason == "persistence_error");
    CHECK(store.snapshot() == before);
}

// from
// tests/test_meeting_state_copy_on_write.py::test_runtime_values_are_owned_by_store_after_commit
TEST_CASE("test_runtime_values_are_owned_by_store_after_commit",
          "[test_meeting_state_copy_on_write]") {
    MeetingStateStore store(state_for("m_owned"));
    Json fields = {{"speech", {{"nested", {{"ready", true}}}}}};
    CHECK(store.update_runtime_fields(fields));
    fields["speech"]["nested"]["ready"] = false;
    CHECK(store.snapshot()["speech"]["nested"]["ready"] == true);
}

// from tests/test_meeting_state_copy_on_write.py::test_op_values_are_owned_by_store_after_commit
TEST_CASE("test_op_values_are_owned_by_store_after_commit", "[test_meeting_state_copy_on_write]") {
    MeetingStateStore store(state_for("m_owned"));
    Json batch = Json::parse(R"([{"op": "add_item", "card": "key_points", "text": "Ready",
        "data": {"nested": {"ready": true}}}])");
    CHECK(store.apply("host", std::nullopt, batch)[0].ok);
    batch[0]["data"]["nested"]["ready"] = false;
    CHECK(store.snapshot()["cards"]["key_points"][0]["data"]["nested"]["ready"] == true);
}

// from
// tests/test_meeting_state_copy_on_write.py::test_repository_reconciliation_keeps_canonical_title_and_contract
TEST_CASE("test_repository_reconciliation_keeps_canonical_title_and_contract",
          "[test_meeting_state_copy_on_write]") {
    const bool metadata_only = GENERATE(false, true);
    class Repository final : public StubRepository {
    public:
        explicit Repository(bool metadata_only) : metadata_only_(metadata_only) {}
        Json on_ops_applied(std::string_view, const Json& snapshot, std::vector<OpResult>&,
                            std::string_view, const std::optional<std::string>&) override {
            Json out = snapshot;
            out["title"] = "Renamed elsewhere";
            if (!metadata_only_) {
                out["rolling_summary"] = "Repository transformation";
            }
            return out;
        }
        Json persist_state(std::string_view, const Json& snapshot) override {
            Json out = snapshot;
            out["title"] = "Renamed again";
            return out;
        }
        bool state_metadata_only() const override { return metadata_only_; }

    private:
        bool metadata_only_;
    };
    MeetingStateStore store(state_for("m_title"), std::make_shared<Repository>(metadata_only));
    CHECK(store
              .apply("host", std::nullopt,
                     Json::parse(R"([{"op": "set_rolling_summary", "text": "Summary"}])"))[0]
              .ok);
    const Json snapshot = store.snapshot();
    CHECK(snapshot["title"] == "Renamed elsewhere");
    CHECK(snapshot["rolling_summary"] == (metadata_only ? "Summary" : "Repository transformation"));
    CHECK(store.update_runtime_fields({{"status", "paused"}}));
    CHECK(store.snapshot()["title"] == "Renamed again");
}

// from
// tests/test_meeting_state_copy_on_write.py::test_all_registered_handlers_have_explicit_mutation_domains
TEST_CASE("test_all_registered_handlers_have_explicit_mutation_domains",
          "[test_meeting_state_copy_on_write]") {
    // The store copies the whole document per batch, so the domains only describe what a handler
    // writes; every registered op must still name them.
    const auto names = all_ops();
    CHECK(names.size() == op_domains().size());
    for (const auto& name : names) {
        INFO(name);
        CHECK(op_domain(name).has_value());
        CHECK(op_domains().contains(name));
    }
}

TEST_CASE("runtime fields: unknown names, coerced finalization and the title refresh",
          "[meeting_state_store]") {
    fv::FakeClock clock;
    testing::FixedIds ids;
    auto repo = std::make_shared<testing::FakeMeetingRepository>(clock);
    repo->create_meeting({{"id", "m_rt"},
                          {"title", "Row title"},
                          {"status", "active"},
                          {"started_at", "2026-01-01T00:00:00Z"},
                          {"host_token", "h"},
                          {"guest_token", "g"},
                          {"spool_dir", "s"}});
    MeetingStateStore store(state_for("m_rt"), repo, {}, clock, ids);
    CHECK_THROWS_AS(store.update_runtime_fields({{"not_a_field", 1}}), fv::py::PyError);
    // finalization reads the status set in the same call.
    CHECK(
        store.update_runtime_fields({{"status", "ended"}, {"finalization", {{"status", "nope"}}}}));
    CHECK(store.snapshot()["status"] == "ended");
    CHECK(store.snapshot()["finalization"]["status"] == "unavailable");
    CHECK(store.snapshot()["title"] == "Row title");
    repo->rename_meeting("m_rt", "Renamed");
    CHECK(store.refresh_title());
    CHECK_FALSE(store.refresh_title());
    CHECK(store.snapshot()["title"] == "Renamed");
    MeetingState replaced = state_for("m_rt");
    replaced.rolling_summary = "From the remap";
    store.replace_document(replaced);
    CHECK(store.snapshot()["rolling_summary"] == "From the remap");

    int calls = 0;
    {
        const auto subscription =
            store.subscribe([&](std::int64_t, const std::vector<OpResult>&) { ++calls; });
        store.apply("host", std::nullopt,
                    Json::parse(R"([{"op": "set_rolling_summary", "text": "One"}])"));
    }
    store.apply("host", std::nullopt,
                Json::parse(R"([{"op": "set_rolling_summary", "text": "Two"}])"));
    CHECK(calls == 1);
    CHECK(store.with_state([](const MeetingState& s) { return s.rolling_summary; }) == "Two");
}
