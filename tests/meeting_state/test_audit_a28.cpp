// Behaviour audit of area a28 (docs/port/map/a28-meeting-state-persist-export.md): the rows whose
// code was right but whose specifics no other test pinned. Each case names its map row.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/utf8.h"
#include "meeting_state/data_lifecycle.h"
#include "meeting_state/highlights.h"
#include "meeting_state/store.h"
#include "pyport/errors.h"
#include "pyport/regex.h"
#include "sql_support.h"
#include "stub_repository.h"
#include "testkit/testkit.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> keys_of(const Json& object) {
    std::vector<std::string> keys;
    for (const auto& [key, value] : object.items()) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

void create_ended(SqlMeetingRepository& repo, const std::string& id, const std::string& started) {
    repo.create_meeting({{"id", id},
                         {"title", id},
                         {"status", "ended"},
                         {"started_at", started},
                         {"host_token", "h"},
                         {"guest_token", "g"},
                         {"spool_dir", "s"}});
}

// Meetings by id; delete_meeting records what the meetings root held at the commit point.
class TombstoneRepository final : public StubRepository {
public:
    TombstoneRepository(std::vector<Json> rows, fs::path root)
        : rows(std::move(rows)), root(std::move(root)) {}

    std::optional<Json> get_meeting(std::string_view meeting_id) override {
        for (const auto& row : rows) {
            if (row["id"] == std::string(meeting_id)) {
                return Json(row);
            }
        }
        return std::nullopt;
    }
    std::vector<Json> list_meetings() override { return rows; }
    void delete_meeting(std::string_view meeting_id) override {
        seen_at_commit.clear();
        if (fs::is_directory(root)) {
            for (const auto& entry : fs::directory_iterator(root)) {
                seen_at_commit.push_back(fv::path_to_utf8(entry.path().filename()));
            }
        }
        std::erase_if(rows, [&](const Json& row) { return row["id"] == std::string(meeting_id); });
    }

    std::vector<Json> rows;
    fs::path root;
    std::vector<std::string> seen_at_commit;
};

void touch(const fs::path& path) {
    std::ofstream(path, std::ios::binary) << "audio";
}

} // namespace

// A28-46
TEST_CASE("audit A28-46: every pulse kind has OpenWhisper's label, in its order", "[audit_a28]") {
    std::vector<std::pair<std::string, std::string>> labels;
    for (const auto& [kind, label] : PULSE_LABELS) {
        labels.emplace_back(kind, label);
    }
    CHECK(labels ==
          std::vector<std::pair<std::string, std::string>>{{"decision", "Decision"},
                                                           {"disagreement", "Disagreement"},
                                                           {"commitment", "Dated commitment"},
                                                           {"number", "Number"},
                                                           {"takeaway", "Takeaways"}});
    CHECK(pulse_label("takeaway") == "Takeaways");
    CHECK_FALSE(pulse_label("Decision").has_value());
}

// A28-29
TEST_CASE("audit A28-29: replace_tokens writes both tokens and refuses an unknown meeting",
          "[audit_a28]") {
    SqlFixture f;
    make_meeting(*f.repo);
    f.repo->replace_tokens("m_test1", "host-2", "guest-2");
    const Json row = *f.repo->get_meeting("m_test1");
    CHECK(row["host_token"] == "host-2");
    CHECK(row["guest_token"] == "guest-2");
    CHECK(row["title"] == "Test meeting");
    CHECK_THROWS_AS(f.repo->replace_tokens("m_nowhere", "a", "b"), fv::py::ValueError);
}

// A28-30
TEST_CASE("audit A28-30: the Past Meetings page limit defaults to 101 and is clamped from 1",
          "[audit_a28]") {
    SqlFixture f;
    for (int index = 0; index < 102; ++index) {
        const std::string day = std::to_string(1000 + index).substr(1);
        create_ended(*f.repo, "m_" + day, "2026-01-01T09:00:00." + day + "Z");
    }
    CHECK(f.repo->list_past_meeting_summaries().size() == 101);
    PastMeetingQuery query;
    query.limit = 0;
    const auto one = f.repo->list_past_meeting_summaries(query);
    REQUIRE(one.size() == 1);
    CHECK(one[0]["id"] == "m_101"); // newest first
    query.limit = -5;
    CHECK(f.repo->list_past_meeting_summaries(query).size() == 1);
    query.limit = 10000;
    CHECK(f.repo->list_past_meeting_summaries(query).size() == 102);
}

// A28-32
TEST_CASE("audit A28-32: a stored chunk error keeps its first 2000 characters", "[audit_a28]") {
    SqlFixture f;
    make_meeting(*f.repo);
    const auto chunk = f.repo->register_chunk({{"meeting_id", "m_test1"},
                                               {"channel", "mic"},
                                               {"seq", 0},
                                               {"file_path", "/tmp/c0.wav"},
                                               {"start_s", 0.0},
                                               {"duration_s", 1.0},
                                               {"sample_rate", 16000}});
    std::string error;
    for (int index = 0; index < 2100; ++index) {
        error += "\xC3\xA9"; // é: code points, not bytes, are counted
    }
    const auto stored_error = [&] {
        return f.repo->get_audio_chunks("m_test1").at(0).at("asr_error").get<std::string>();
    };
    f.repo->set_chunk_status(chunk, "processing", std::nullopt);
    f.repo->set_chunk_status(chunk, "failed", error);
    CHECK(stored_error() == error.substr(0, 4000));
    f.repo->set_chunk_status(chunk, "processing", std::nullopt);
    f.repo->defer_chunk_after_connection_failure(chunk, "x" + error);
    CHECK(stored_error() == "x" + error.substr(0, 3998));
    const Json row = f.repo->get_audio_chunks("m_test1").at(0);
    CHECK(row["asr_status"] == "pending");
    CHECK(row["asr_attempts"] == 1); // the deferred attempt was given back
}

// A28-17, A28-37, A28-59
TEST_CASE("audit A28-17/37/59: audit events, their shapes and the undo action over SQL",
          "[audit_a28]") {
    SqlFixture f;
    make_meeting(*f.repo);
    MeetingStateStore store(state_for("m_test1"), f.repo, {}, f.clock, f.ids);
    const auto first = store.apply("host", "p_host", Json::parse(R"([
        {"op": "add_item", "card": "decisions", "text": "Café opens at nine"},
        {"op": "add_item", "card": "risks", "text": "Budget slips"}])"));
    REQUIRE(first.size() == 2);
    REQUIRE(first[0].ok);
    REQUIRE(first[1].ok);
    const std::int64_t added = *first[0].seq;

    const Json event = *f.repo->get_event("m_test1", added);
    CHECK(keys_of(event) == std::vector<std::string>{"action", "actor_id", "actor_type", "inverse",
                                                     "payload", "seq", "target_id", "ts"});
    CHECK(event["action"] == "add_item");
    CHECK(event["actor_type"] == "host");
    CHECK(event["actor_id"] == "p_host");
    CHECK(event["payload"]["text"] == "Café opens at nine");
    CHECK_FALSE(f.repo->get_event("m_test1", 999).has_value());
    // payload_json keeps the text as UTF-8 (ensure_ascii=False).
    const auto payloads = column_values(*f.database, "SELECT payload_json FROM meeting_events "
                                                     "WHERE seq = " +
                                                         std::to_string(added));
    REQUIRE(payloads.size() == 1);
    CHECK(payloads[0].get<std::string>().find("Café") != std::string::npos);

    const auto undone = store.undo(added, "p_host");
    REQUIRE(undone.size() == 1);
    REQUIRE(undone[0].ok);

    const auto events = f.repo->list_events("m_test1");
    REQUIRE(events.size() == 3);
    // Newest first; the undo is recorded as the system's undo:<seq>.
    CHECK(events[0]["seq"] == *undone[0].seq);
    CHECK(events[0]["action"] == "undo:" + std::to_string(added));
    CHECK(events[0]["actor_type"] == "system");
    CHECK(events[0]["undoable"] == true);
    CHECK(keys_of(events[0]) == std::vector<std::string>{"action", "actor_id", "actor_type",
                                                         "payload", "seq", "target_id", "ts",
                                                         "undoable"});
    CHECK(events[1]["seq"] == *first[1].seq);
    CHECK(events[1]["undoable"] == true);
    CHECK(events[2]["seq"] == added);
    CHECK(events[2]["undoable"] == false);
    CHECK(f.repo->event_is_undone("m_test1", added));
    CHECK_FALSE(f.repo->event_is_undone("m_test1", *first[1].seq));

    // The limit is clamped to 1..500; before_seq pages backwards.
    const auto newest = f.repo->list_events("m_test1", std::nullopt, 0);
    REQUIRE(newest.size() == 1);
    CHECK(newest[0]["seq"] == *undone[0].seq);
    CHECK(f.repo->list_events("m_test1", std::nullopt, -3).size() == 1);
    const auto older = f.repo->list_events("m_test1", *first[1].seq, 100);
    REQUIRE(older.size() == 1);
    CHECK(older[0]["seq"] == added);
}

// A28-58
TEST_CASE("audit A28-58: segment reads are strictly after a start, unbounded at limit 0, and a "
          "speaker must be a participant of the meeting",
          "[audit_a28]") {
    SqlFixture f;
    make_meeting(*f.repo);
    make_meeting(*f.repo, "m_other");
    f.repo->add_segments({make_segment("m_test1", "sg_a", 1.0, 2.0, "one"),
                          make_segment("m_test1", "sg_b", 2.0, 3.0, "two"),
                          make_segment("m_test1", "sg_c", 3.0, 4.0, "three")});
    CHECK(ids_of(f.repo->get_segments("m_test1", 2.0)) == std::vector<std::string>{"sg_c"});
    CHECK(ids_of(f.repo->get_segments("m_test1", -1.0, 0)).size() == 3);
    CHECK(ids_of(f.repo->get_segments("m_test1", -1.0, 2)) ==
          std::vector<std::string>{"sg_a", "sg_b"});
    const auto created = column_values(*f.database, "SELECT DISTINCT created_at FROM "
                                                    "meeting_segments");
    REQUIRE(created.size() == 1); // one stamp for the whole batch
    CHECK(created[0].get<std::string>().starts_with("2026-01-01T00:00:00"));
    for (const auto& row : f.repo->get_segments("m_test1")) {
        CHECK_FALSE(row.contains("embedding"));
    }

    MeetingStateStore store(state_for("m_other"), f.repo, {}, f.clock, f.ids);
    const auto person = store.apply("host", "p_host", Json::parse(R"([
        {"op": "upsert_participant", "display_name": "Dana"}])"));
    REQUIRE(person.at(0).ok);
    const std::string other_meetings_person = *person[0].target_id;
    CHECK_THROWS_AS(
        f.repo->update_segment_speaker("m_test1", "sg_a", other_meetings_person, "human", true),
        fv::py::ValueError);
    CHECK(f.repo->get_segment("m_test1", "sg_a")->at("speaker_pinned") == false);
    f.repo->update_segment_speaker("m_test1", "sg_a", std::nullopt, "human", true);
    CHECK(f.repo->get_segment("m_test1", "sg_a")->at("speaker_pinned") == true);
}

// A28-60
TEST_CASE("audit A28-60: transcript search rows carry the raw text in Python's shape",
          "[audit_a28]") {
    SqlFixture f;
    make_meeting(*f.repo);
    f.repo->add_segments({make_segment("m_test1", "sg_1", 1.0, 2.0, "Entropic ships the budget")});
    f.repo->persist_state("m_test1",
                          {{"cards",
                            {{"user_notes", Json::array({{{"id", "it_fix"},
                                                          {"card", "user_notes"},
                                                          {"author_type", "user"},
                                                          {"text", "Correction"},
                                                          {"data",
                                                           {{"kind", "term_correction"},
                                                            {"selected_text", "Entropic"},
                                                            {"replacement", "Anthropic"}}}}})}}}});
    CHECK(f.repo->get_segment("m_test1", "sg_1")->at("text") == "Anthropic ships the budget");

    CHECK(f.repo->search_transcripts("   ", std::nullopt).empty());
    const auto hits = f.repo->search_transcripts("budget", std::nullopt);
    REQUIRE(hits.size() == 1);
    CHECK(keys_of(hits[0]) == std::vector<std::string>{"end_s", "meeting_id", "segment_id",
                                                       "snippet", "start_s", "started_at", "text",
                                                       "title"});
    CHECK(hits[0]["text"] == "Entropic ships the budget"); // raw, no correction applied
    CHECK(hits[0]["started_at"] == "2026-01-01T09:00:00.000001");
    // An FTS operator in the input is quoted, not obeyed.
    CHECK(f.repo->search_transcripts("budget OR nothing", std::nullopt).empty());
    CHECK(f.repo->search_transcripts("ships budget", std::nullopt).size() == 1);
}

// A28-40, A28-61
TEST_CASE("audit A28-40/61: the spool is a .deleting-<id>-<32 hex> tombstone at the commit point",
          "[audit_a28]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path spool = root / "m_del";
    fs::create_directories(spool);
    touch(spool / "audio.wav");
    TombstoneRepository repo({{{"id", "m_del"}, {"spool_dir", fv::path_to_utf8(spool)}}}, root);

    CHECK(delete_meeting_data(repo, "m_del", root));
    REQUIRE(repo.seen_at_commit.size() == 1);
    static const fv::py::Regex tombstone(R"(\.deleting-m_del-[0-9a-f]{32})");
    CHECK(tombstone.fullmatch(repo.seen_at_commit[0]).has_value());
    CHECK(fs::is_empty(root));
}

// A28-61
TEST_CASE("audit A28-61: no meetings root means rows only, and a keep-recordings clear leaves "
          "orphan folders",
          "[audit_a28]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path spool = root / "m_keep";
    const fs::path orphan = root / "m_orphan";
    fs::create_directories(spool);
    fs::create_directories(orphan);
    touch(spool / "audio.wav");

    TombstoneRepository rows_only({{{"id", "m_keep"}, {"spool_dir", fv::path_to_utf8(spool)}}},
                                  root);
    CHECK(delete_meeting_data(rows_only, "m_keep", fs::path()));
    CHECK(rows_only.rows.empty());
    CHECK(fs::is_regular_file(spool / "audio.wav"));

    TombstoneRepository blank_spool({{{"id", "m_blank"}, {"spool_dir", ""}}}, root);
    CHECK(delete_meeting_data(blank_spool, "m_blank", root));
    CHECK(blank_spool.rows.empty());

    TombstoneRepository clearing(
        {{{"id", "m_keep"}, {"status", "ended"}, {"spool_dir", fv::path_to_utf8(spool)}}}, root);
    CHECK(clear_meetings(clearing, root, false).removed == 1);
    CHECK(fs::is_directory(spool));
    CHECK(fs::is_directory(orphan)); // no orphan purge without delete_spools
    CHECK(clear_meetings(clearing, root, true).removed == 0);
    CHECK_FALSE(fs::exists(orphan));
    CHECK_FALSE(fs::exists(spool));
}
