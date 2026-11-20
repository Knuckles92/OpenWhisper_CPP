// Audit A29-11, A29-13 and A29-14: what the review worker (run_insight_review) asks, how much of
// the transcript it sends and what it publishes, against meeting/insight_review.py: MAX_ITEMS,
// _finish_message, evidence_context (neighbours, MAX_CONTEXT_CHARS) and assess's questions
// (_FIELD_QUESTIONS, _DEFAULT_CHOICES, _FIELD_CHOICES, _question_sources, owners, priority).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../meeting_state/stub_repository.h"
#include "base/testing/fake_clock.h"
#include "llm/testing/fake_judge.h"
#include "meeting_intel/insight_review.h"
#include "meeting_state/review_ops.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"
#include "pyport/json.h"
#include "pyport/text.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

using Scores = std::map<std::string, double>;

class Repo final : public test_support::StubRepository {
public:
    std::vector<Json> segments;

    std::vector<Json> get_segments(std::string_view, double, std::optional<std::int64_t>) override {
        return segments;
    }
    Json on_ops_applied(std::string_view, const Json&, std::vector<OpResult>&, std::string_view,
                        const std::optional<std::string>&) override {
        return Json();
    }
    Json persist_state(std::string_view, const Json&) override { return Json(); }
};

Json segment(int index, const std::string& text, const std::string& speaker = "") {
    Json row{{"id", std::format("sg{}", index)}, {"start_s", index}, {"text", text}};
    if (!speaker.empty()) {
        row["speaker_participant_id"] = speaker;
    }
    return row;
}

CardItem item(const std::string& id, const std::string& card, const std::string& text,
              std::vector<std::string> evidence, const Json& data = Json::object()) {
    CardItem out;
    out.id = id;
    out.card = card;
    out.text = text;
    out.evidence = std::move(evidence);
    out.data = data;
    out.created_at = out.updated_at = "2026-01-01T00:00:00Z";
    return out;
}

// An ended meeting with AI insights on and the review consented, holding `items`.
struct Review {
    fv::FakeClock clock;
    testing::FixedIds ids;
    std::shared_ptr<Repo> repo = std::make_shared<Repo>();
    std::shared_ptr<fv::llm::FakeJudge> judge = std::make_shared<fv::llm::FakeJudge>();
    std::unique_ptr<MeetingStateStore> store;

    Review(const std::vector<CardItem>& items, std::vector<Json> segments,
           const std::vector<Participant>& people = {}) {
        repo->segments = std::move(segments);
        MeetingState state;
        state.meeting_id = "synthetic";
        state.status = "ended";
        state.cloud_enabled = true;
        state.finalization.status = "completed";
        state.insight_review = Json{{"enabled", true},
                                    {"consent", std::string(REVIEW_CONSENT)},
                                    {"sensitivity", "normal"},
                                    {"status", "pending"},
                                    {"questions", Json::array()},
                                    {"message", ""}};
        for (const auto& card_item : items) {
            state.cards[card_item.card].push_back(card_item);
        }
        for (const auto& person : people) {
            state.participants.set(person.id, person);
        }
        store = std::make_unique<MeetingStateStore>(state, repo, SegmentHandler{}, clock, ids);
        judge->set_local(true);
        judge->set_model("local-judge");
    }

    // Every asked question is answered from `scores`; one left out reads as no concern.
    void answer(const Scores& scores) {
        judge->set_handler([scores](const Json&, const Json& questions) {
            Json answers = Json::object();
            for (auto it = questions.begin(); it != questions.end(); ++it) {
                const bool inverted = it.key() == "contradiction" || it.key() == "asr_uncertain";
                const auto found = scores.find(it.key());
                answers[it.key()] =
                    Json{{"noul", found != scores.end() ? found->second : (inverted ? 0.0 : 1.0)}};
            }
            return std::optional(answers);
        });
    }

    Json run() {
        fv::meeting_intel::run_insight_review(*store, *repo,
                                              {.judge = [this](double) { return judge; },
                                               .allowed = [] { return true; },
                                               .clock = &clock});
        return store->snapshot().at("insight_review");
    }
};

std::vector<Json> filler(int count) {
    std::vector<Json> rows;
    for (int i = 0; i < count; ++i) {
        rows.push_back(segment(i, "Unrelated discussion"));
    }
    return rows;
}

std::vector<std::string> ids_of(const Json& rows) {
    std::vector<std::string> out;
    for (const auto& row : rows) {
        out.push_back(row.at("id").get<std::string>());
    }
    return out;
}

const Json kDefaultChoices{
    {"accurate", "Accurate"}, {"edit", "Correct it"}, {"incorrect", "Incorrect"}};
const Json kClarifyChoices{{"edit", "Save clarification"}, {"incorrect", "Incorrect"}};

} // namespace

TEST_CASE("audit A29-11: at most 40 items are checked and more make the review partial",
          "[test_meeting_insight_review]") {
    std::vector<CardItem> items;
    for (int i = 0; i < 41; ++i) {
        items.push_back(item(std::format("it{}", i), "risks", std::format("Risk {}", i), {"sg0"}));
    }
    Review review(items, filler(1));
    review.answer({});
    const Json result = review.run();
    CHECK(review.judge->calls().size() == 40);
    CHECK(result.at("status") == "partial");
    CHECK(result.at("message") == "Some insights could not be checked. You can retry.");
}

TEST_CASE("audit A29-11: the finish messages of a ready review", "[test_meeting_insight_review]") {
    SECTION("no question selected") {
        Review review({item("it0", "risks", "The vendor may slip.", {"sg0"})}, filler(1));
        review.answer({});
        const Json result = review.run();
        CHECK(result.at("status") == "ready");
        CHECK(result.at("message") ==
              "No questions were selected. This does not certify that every insight is correct.");
        CHECK(result.at("questions").empty());
    }
    SECTION("questions to answer") {
        Review review({item("it0", "risks", "The vendor may slip.", {"sg0"})}, filler(1));
        review.answer({{"support", 0.5}});
        const Json result = review.run();
        CHECK(result.at("status") == "ready");
        CHECK(result.at("message") == "Review the details below, or skip and return later.");
        CHECK(result.at("questions").size() == 1);
    }
}

TEST_CASE("audit A29-13: the evidence context is the anchors, two neighbours each side and "
          "related speech, in time order",
          "[test_meeting_insight_review]") {
    auto rows = filler(30);
    rows[25]["text"] = "Paper links canceled";
    rows[3]["text"] = "Paper links maybe";
    Review review({item("it0", "risks", "Paper links task", {"sg10", "sg11"})}, rows);
    review.answer({});
    review.run();
    const auto calls = review.judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(ids_of(calls[0].state.at("transcript")) ==
          std::vector<std::string>{"sg3", "sg8", "sg9", "sg10", "sg11", "sg12", "sg13", "sg25"});
    // Each row carries _CONTEXT_FIELDS, missing ones as null.
    CHECK(calls[0].state.at("transcript")[0] == Json{{"id", "sg3"},
                                                     {"start_s", 3},
                                                     {"text", "Paper links maybe"},
                                                     {"original_text", nullptr},
                                                     {"speaker_participant_id", nullptr},
                                                     {"speaker_source", nullptr}});
}

TEST_CASE("audit A29-13: the context stays within 16000 JSON characters, skipping a row that "
          "does not fit and keeping smaller later ones",
          "[test_meeting_insight_review]") {
    auto rows = filler(30);
    // The best related row (two shared words) cannot fit; one-word rows still do.
    rows[20]["text"] = "paper links " + std::string(16'500, 'x');
    for (int i = 21; i < 30; ++i) {
        rows[i]["text"] = "paper " + std::string(1'800, 'y');
    }
    Review review({item("it0", "risks", "paper links", {"sg0"})}, rows);
    review.answer({});
    review.run();
    const auto calls = review.judge->calls();
    REQUIRE(calls.size() == 1);
    const Json context = calls[0].state.at("transcript");
    std::size_t size = 0;
    for (const auto& row : context) {
        size += fv::py::len(fv::py::dumps(row));
    }
    CHECK(size <= 16'000);
    const auto chosen = ids_of(context);
    CHECK(chosen.front() == "sg0");
    CHECK(std::ranges::find(chosen, "sg20") == chosen.end());
    // Later rows, highest index first among equal overlap, until the budget is spent.
    CHECK(std::ranges::find(chosen, "sg29") != chosen.end());
    CHECK(std::ranges::find(chosen, "sg21") == chosen.end());
    CHECK(std::ranges::is_sorted(chosen, {},
                                 [](const std::string& id) { return std::stoi(id.substr(2)); }));
}

TEST_CASE("audit A29-14: each field's question text and choices", "[test_meeting_insight_review]") {
    const Json owner_set{{"owner_participant_id", "p1"}};
    struct Case {
        CardItem insight;
        Scores scores;
        std::string field;
        std::string text;
        Json choices;
        double priority;
    };
    const std::vector<Case> cases{
        {item("it0", "risks", "The vendor may slip.", {"sg0"}),
         {{"support", 0.5}},
         "support",
         "Is this insight accurate?",
         kDefaultChoices,
         0.5 * 1.5},
        {item("it0", "action_items", "Send the plan.", {"sg0"}, owner_set),
         {{"acceptance", 0.3}},
         "acceptance",
         "Was this agreed, or only proposed?",
         Json{{"accepted", "Agreed"},
              {"offered", "Only proposed"},
              {"edit", "Correct it"},
              {"incorrect", "Incorrect"}},
         0.7 * 2},
        {item("it0", "risks", "The vendor may slip.", {"sg0"}),
         {{"resolution", 0.4}},
         "resolution",
         "Did this risk remain open or was it resolved?",
         Json{{"open", "Still open"},
              {"resolved", "Resolved"},
              {"edit", "Correct it"},
              {"incorrect", "Incorrect"}},
         0.6 * 1.5},
        {item("it0", "action_items", "Send the plan.", {"sg0"}),
         {},
         "owner",
         "Who agreed to take responsibility for this task?",
         kClarifyChoices,
         1.0 * 2},
        {item("it0", "action_items", "Send the plan by Friday.", {"sg0"}, owner_set),
         {{"deadline", 0.2}},
         "deadline",
         "What deadline, if any, was agreed?",
         kClarifyChoices,
         0.8 * 2},
    };
    for (const auto& c : cases) {
        CAPTURE(c.field);
        Review review({c.insight}, filler(1));
        review.answer(c.scores);
        const Json questions = review.run().at("questions");
        REQUIRE(questions.size() == 1);
        const Json& q = questions[0];
        CHECK(q.at("field") == c.field);
        CHECK(q.at("text") == c.text);
        CHECK(q.at("choices") == c.choices); // ordered: Python's dict order
        CHECK(q.at("status") == "open");
        CHECK(q.at("reason") == "The evidence needs clarification on this point.");
        CHECK_THAT(q.at("priority").get<double>(), Catch::Matchers::WithinAbs(c.priority, 1e-9));
    }
}

TEST_CASE("audit A29-14: a question cites three sources and the last context row, and offers "
          "only settled participants as owners",
          "[test_meeting_insight_review]") {
    std::vector<Json> rows;
    for (int i = 0; i < 8; ++i) {
        rows.push_back(segment(i, "Unrelated discussion", i % 2 == 0 ? "p1" : "p2"));
    }
    rows.push_back(segment(8, "Unrelated discussion", "p3"));
    Participant settled{.id = "p1", .display_name = "Ana", .kind = "me"};
    Participant provisional{.id = "p2", .display_name = "Speaker 2", .is_provisional = true};
    Participant absent{.id = "p9", .display_name = "Not here"};
    Review review({item("it0", "risks", "The vendor may slip.", {"sg0", "sg1", "sg2", "sg3"})},
                  rows, {settled, provisional, absent});
    review.answer({{"support", 0.5}});
    const Json q = review.run().at("questions").at(0);
    // Context is sg0..sg5 (anchors and their neighbours); sources the first three cited rows,
    // then the last context row.
    CHECK(ids_of(q.at("sources")) == std::vector<std::string>{"sg0", "sg1", "sg2", "sg5"});
    CHECK(q.at("evidence") == Json::array({"sg0", "sg1", "sg2", "sg3"}));
    CHECK(q.at("owners") ==
          Json::array({Json{{"id", "p1"}, {"display_name", "Ana"}, {"is_provisional", false}}}));
    const auto calls = review.judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(
        calls[0].state.at("participants") ==
        Json::array({Json{{"id", "p1"}, {"display_name", "Ana"}, {"is_provisional", false}},
                     Json{{"id", "p2"}, {"display_name", "Speaker 2"}, {"is_provisional", true}}}));
}
