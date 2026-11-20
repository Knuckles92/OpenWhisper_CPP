// testing/fake_meeting_repository.h: the in-memory MeetingRepository keeps the SQL
// repository's rules that later packages' tests lean on.

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/fake_clock.h"
#include "meeting_state/corrections.h"
#include "meeting_state/schema.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/errors.h"

using fv::Json;
using namespace fv::meeting_state;
using fv::meeting_state::testing::FakeMeetingRepository;

namespace {

Json meeting(const std::string& id, const std::string& started = "2026-01-01T00:00:00Z",
             const std::string& status = "active") {
    return {{"id", id},          {"status", status},   {"started_at", started},
            {"host_token", "h"}, {"guest_token", "g"}, {"spool_dir", "C:/spool/" + id}};
}

Json chunk(const std::string& meeting_id, const std::string& channel, int seq, double start_s) {
    return {{"meeting_id", meeting_id}, {"channel", channel}, {"seq", seq},
            {"file_path", "c.wav"},     {"start_s", start_s}, {"duration_s", 4.0},
            {"sample_rate", 16000}};
}

TranscriptSegment segment(const std::string& id, double start_s, const std::string& text,
                          std::optional<std::int64_t> chunk_id = std::nullopt,
                          const std::string& meeting_id = "m_1") {
    TranscriptSegment seg;
    seg.segment_id = id;
    seg.meeting_id = meeting_id;
    seg.chunk_id = chunk_id;
    seg.channel = "mic";
    seg.start_s = start_s;
    seg.end_s = start_s + 1.0;
    seg.text = text;
    return seg;
}

std::vector<std::string> ids_of(const std::vector<Json>& rows, const char* key = "id") {
    std::vector<std::string> ids;
    for (const auto& row : rows) {
        ids.push_back(row.at(key).is_string() ? row.at(key).get<std::string>()
                                              : row.at(key).dump());
    }
    return ids;
}

} // namespace

TEST_CASE("meeting rows take column defaults and NOT NULL and UNIQUE rules", "[fake_repository]") {
    const fv::FakeClock clock;
    FakeMeetingRepository repo(clock);
    repo.create_meeting(meeting("m_1"));
    const Json row = *repo.get_meeting("m_1");
    CHECK(row["title"] == "");
    CHECK(row["paused_total_s"] == 0.0);
    CHECK(row["state_seq"] == 0);
    CHECK(row["ended_at"].is_null());
    CHECK(row.size() == 21);
    CHECK_THROWS_AS(repo.create_meeting(meeting("m_1")), std::runtime_error);
    CHECK_THROWS_AS(repo.create_meeting(Json{{"id", "m_2"}}), std::runtime_error);
    CHECK_THROWS_AS(repo.create_meeting(Json{{"id", "m_3"}, {"bogus", 1}}), fv::py::TypeError);

    repo.update_meeting("m_1", {{"status", "ended"}, {"not_a_column", 1}});
    repo.update_meeting("m_unknown", {{"status", "ended"}});
    CHECK(repo.get_meeting("m_1")->at("status") == "ended");
    CHECK_FALSE(repo.get_meeting("m_1")->contains("not_a_column"));

    repo.rename_meeting("m_1", "  Weekly sync  ");
    CHECK(repo.get_meeting("m_1")->at("title") == "Weekly sync");
    CHECK(parse_state_json(repo.get_meeting("m_1")->at("state_json")) ==
          Json{{"meeting_id", "m_1"}, {"title", "Weekly sync"}});
    CHECK_THROWS_AS(repo.rename_meeting("m_x", "T"), fv::py::ValueError);
    CHECK_THROWS_AS(repo.rename_meeting("m_1", " "), fv::py::ValueError);

    repo.heartbeat("m_1", 42, "2026-01-01T00:00:05Z");
    CHECK(repo.get_meeting("m_1")->at("app_pid") == 42);

    repo.create_meeting(meeting("m_2", "2026-02-01T00:00:00Z"));
    CHECK(ids_of(repo.list_meetings()) == std::vector<std::string>{"m_2", "m_1"});
    CHECK(ids_of(repo.find_interrupted_meetings()) == std::vector<std::string>{"m_2"});
}

TEST_CASE("the chunk lifecycle follows the SQL repository", "[fake_repository]") {
    FakeMeetingRepository repo;
    repo.create_meeting(meeting("m_1", "2026-01-01T00:00:00Z", "ended"));
    const auto a = repo.register_chunk(chunk("m_1", "mic", 0, 4.0));
    const auto b = repo.register_chunk(chunk("m_1", "loopback", 0, 0.0));
    const auto c = repo.register_chunk(chunk("m_1", "mic", 1, 8.0));
    CHECK_THROWS_AS(repo.register_chunk(chunk("m_1", "mic", 1, 8.0)), std::runtime_error);
    CHECK(repo.register_chunk_if_missing(chunk("m_1", "mic", 1, 8.0)).created == false);
    CHECK(repo.register_chunk_if_missing(chunk("m_1", "mic", 1, 8.0)).id == c);
    CHECK(repo.next_chunk_seq("m_1", "mic") == 2);
    CHECK(repo.next_chunk_seq("m_1", "other") == 0);
    CHECK(ids_of(repo.find_interrupted_meetings()) == std::vector<std::string>{"m_1"});

    CHECK(ids_of(repo.get_pending_chunks("m_1", std::nullopt, {})) ==
          std::vector<std::string>{std::to_string(b), std::to_string(a), std::to_string(c)});
    CHECK(repo.get_pending_chunks("m_1", 0, {}).empty());
    CHECK(repo.get_pending_chunks("m_1", 1, {b}).at(0).at("id") == a);

    // Entering processing spends an attempt; three attempts exhaust a chunk.
    for (int n = 0; n < 3; ++n) {
        repo.set_chunk_status(a, "processing", std::nullopt);
    }
    repo.set_chunk_status(a, "failed", std::string(2100, 'e'));
    const auto chunks = repo.get_audio_chunks("m_1");
    CHECK(chunks.at(1).at("asr_attempts") == 3);
    CHECK(chunks.at(1).at("asr_error").get<std::string>().size() == 2000);
    CHECK(ids_of(repo.get_pending_chunks("m_1", std::nullopt, {})) ==
          std::vector<std::string>{std::to_string(b), std::to_string(c)});

    repo.set_chunk_status(b, "blocked", std::nullopt);
    CHECK(repo.reset_unfinished_chunks("m_1", false) == 1); // c only
    CHECK(repo.reset_unfinished_chunks("m_1", true) == 3);
    CHECK(repo.get_audio_chunks("m_1").at(1).at("asr_attempts") == 0);

    repo.set_chunk_status(c, "processing", std::nullopt);
    repo.defer_chunk_after_connection_failure(c, "offline");
    CHECK(repo.get_audio_chunks("m_1").at(2).at("asr_attempts") == 0);
    CHECK(repo.get_audio_chunks("m_1").at(2).at("asr_status") == "pending");

    repo.set_chunk_status(c, "done", std::nullopt);
    repo.set_chunk_status(c, "pending", std::nullopt); // done is terminal
    CHECK(repo.count_unfinished_chunks("m_1") == 2);
    CHECK(repo.mark_chunks_done("m_1") == 2);
    CHECK(repo.count_unfinished_chunks("m_1") == 0);
    CHECK(repo.find_interrupted_meetings().empty());

    repo.fail_register_chunk(1);
    CHECK_THROWS_AS(repo.register_chunk(chunk("m_1", "mic", 9, 1.0)), std::runtime_error);
    CHECK_NOTHROW(repo.register_chunk(chunk("m_1", "mic", 9, 1.0)));
}

TEST_CASE("racing recoveries of one chunk register one row", "[fake_repository]") {
    FakeMeetingRepository repo;
    repo.create_meeting(meeting("m_1"));
    constexpr int kThreads = 8;
    std::vector<ChunkRegistration> outcomes(kThreads);
    std::vector<std::exception_ptr> errors(kThreads);
    {
        std::vector<std::jthread> threads;
        for (int n = 0; n < kThreads; ++n) {
            threads.emplace_back([&, n] {
                try {
                    outcomes[n] = repo.register_chunk_if_missing(chunk("m_1", "mic", 0, 0.0));
                } catch (...) {
                    errors[n] = std::current_exception();
                }
            });
        }
    }
    int created = 0;
    for (int n = 0; n < kThreads; ++n) {
        CHECK_FALSE(errors[n]);
        CHECK(outcomes[n].id == outcomes[0].id);
        created += outcomes[n].created ? 1 : 0;
    }
    CHECK(created == 1);
    CHECK(repo.get_audio_chunks("m_1").size() == 1);
}

TEST_CASE("segments read with corrections, keep delivery order and commit atomically",
          "[fake_repository]") {
    const fv::FakeClock clock;
    FakeMeetingRepository repo(clock);
    repo.create_meeting(meeting("m_1"));
    const auto chunk_id = repo.register_chunk(chunk("m_1", "mic", 0, 0.0));
    repo.add_segments(
        {segment("sg_2", 5.0, "Entropic later"), segment("sg_1", 1.0, "Entropic ships")});
    CHECK_THROWS_AS(repo.add_segments({segment("sg_1", 9.0, "dup")}), std::runtime_error);

    repo.persist_state("m_1", {{"meeting_id", "m_1"},
                               {"seq", 3},
                               {"cards",
                                {{"user_notes",
                                  {{{"author_type", "user"},
                                    {"status", "edited"},
                                    {"data",
                                     {{"kind", "term_correction"},
                                      {"selected_text", "Entropic"},
                                      {"replacement", "Anthropic"}}}}}}}}});
    CHECK(repo.get_meeting("m_1")->at("state_seq") == 3);
    const Json row = *repo.get_segment("m_1", "sg_1");
    CHECK(row["text"] == "Anthropic ships");
    CHECK(row["original_text"] == "Entropic ships");
    CHECK(row["created_at"] == "2026-01-01T00:00:00Z");
    CHECK_FALSE(repo.get_segment("m_2", "sg_1").has_value());

    CHECK(ids_of(repo.get_segments("m_1", -1.0, std::nullopt)) ==
          std::vector<std::string>{"sg_1", "sg_2"});
    CHECK(ids_of(repo.get_segments("m_1", 1.0, std::nullopt)) == std::vector<std::string>{"sg_2"});
    CHECK(ids_of(repo.get_segments_page("m_1", 1.0, "sg_1", 500)) ==
          std::vector<std::string>{"sg_2"});
    CHECK(ids_of(repo.get_last_segments("m_1", 1)) == std::vector<std::string>{"sg_2"});
    // Delivery follows commit order, not speech time.
    const auto page = repo.get_segments_delivery_page("m_1", 0, 300);
    CHECK(ids_of(page) == std::vector<std::string>{"sg_2", "sg_1"});
    CHECK(page.at(1).at("delivery_seq") == 2);
    CHECK(repo.get_delivery_high_water("m_1") == 2);

    CHECK_THROWS_AS(
        repo.commit_chunk_transcription("m_1", chunk_id, {segment("sg_3", 2.0, "x", 999)}),
        fv::py::ValueError);
    const auto committed =
        repo.commit_chunk_transcription("m_1", chunk_id, {segment("sg_3", 2.0, "Hello", chunk_id)});
    CHECK(committed.committed);
    CHECK(ids_of(committed.rows) == std::vector<std::string>{"sg_3"});
    const auto again = repo.commit_chunk_transcription("m_1", chunk_id,
                                                       {segment("sg_4", 3.0, "Ignored", chunk_id)});
    CHECK_FALSE(again.committed);
    CHECK(ids_of(again.rows) == std::vector<std::string>{"sg_3"});
    CHECK_FALSE(repo.segment_exists("m_1", "sg_4"));
    CHECK(repo.segment_flags("m_1", {"sg_1", "sg_9", ""}) ==
          std::map<std::string, bool, std::less<>>{{"sg_1", false}});

    CHECK_THROWS_AS(repo.get_delivery_cursor("m_1", "other"), fv::py::ValueError);
    repo.advance_delivery_cursor("m_1", "cards", 5);
    repo.advance_delivery_cursor("m_1", "cards", 2);
    CHECK(repo.get_delivery_cursor("m_1", "cards") == 5);
    CHECK(repo.get_delivery_cursor("m_1", "notes") == 0);

    CHECK_THROWS_AS(repo.update_segment_speaker("m_1", "sg_1", "p_x", "human", true),
                    fv::py::ValueError);
    repo.update_segment_speaker("m_1", "sg_1", std::nullopt, "human", true);
    CHECK(repo.segment_flags("m_1", {"sg_1"}).at("sg_1"));

    const auto replaced = repo.replace_final_transcript(
        "m_1", {segment("off_1", 0.5, "Offline one"), segment("sg_2", 5.0, "Offline two")});
    CHECK(replaced.deleted_ids == std::vector<std::string>{"sg_3"});
    CHECK(ids_of(replaced.rows) == std::vector<std::string>{"off_1", "sg_1", "sg_2"});

    CHECK(ids_of(repo.search_transcripts("anthropic", std::nullopt, 200), "segment_id").empty());
    CHECK(ids_of(repo.search_transcripts(" offline  TWO ", std::nullopt, 200), "segment_id") ==
          std::vector<std::string>{"sg_2"});
    CHECK(repo.search_transcripts("  ", std::nullopt, 200).empty());

    repo.delete_meeting("m_1");
    CHECK_FALSE(repo.get_meeting("m_1").has_value());
    CHECK_FALSE(repo.segment_exists("m_1", "sg_1"));
    CHECK(repo.get_delivery_high_water("m_1") == 0);
}

TEST_CASE("on_ops_applied writes events, mirrors and the snapshot", "[fake_repository]") {
    const fv::FakeClock clock;
    FakeMeetingRepository repo(clock);
    repo.create_meeting(meeting("m_1"));
    repo.add_segments({segment("sg_1", 1.0, "Entropic ships")});

    OpResult participant;
    participant.ok = true;
    participant.op = {{"op", "upsert_participant"}};
    participant.target_id = "p_1";
    participant.effect = {{"entity", "participant"}, {"participant", {{"id", "p_1"}}}};
    participant.seq = 1;
    OpResult speaker;
    speaker.ok = true;
    speaker.op = {{"op", "reassign_segment_speaker"}};
    speaker.effect = {{"entity", "segment_speaker"},
                      {"segment_id", "sg_1"},
                      {"participant_id", "p_1"},
                      {"source", "human"},
                      {"pinned", true}};
    speaker.inverse = {{"op", "reassign_segment_speaker"}};
    speaker.seq = 2;
    OpResult title;
    title.ok = true;
    title.op = {{"op", "set_title"}, {"text", " Renamed "}};
    title.seq = 3;
    OpResult text;
    text.ok = true;
    text.op = {{"op", "revise_segment_text"}};
    text.effect = {{"entity", "segment_text"}, {"segment_id", "sg_1"}, {"text", " Cleaned "}};
    text.seq = 4;
    std::vector<OpResult> results{participant, speaker, title, text};

    const Json snapshot = repo.on_ops_applied("m_1", {{"seq", 4}, {"cloud_enabled", true}}, results,
                                              "host", std::string("p_host"));
    CHECK(snapshot["title"] == "Renamed");
    CHECK(repo.get_meeting("m_1")->at("state_seq") == 4);
    CHECK(repo.get_meeting("m_1")->at("cloud_enabled") == true);
    CHECK(repo.mirrored_participant("m_1", "p_1").has_value());
    CHECK(repo.segment_flags("m_1", {"sg_1"}).at("sg_1"));
    CHECK(results[3].effect["segment"]["text"] == "Cleaned");
    CHECK(repo.get_event("m_1", 2)->at("inverse") == Json{{"op", "reassign_segment_speaker"}});
    CHECK(repo.get_event("m_1", 1)->at("inverse").is_null());

    OpResult undo;
    undo.ok = true;
    undo.op = {{"op", "reassign_segment_speaker"}, {"_undo_event_seq", 2}};
    undo.effect = {
        {"entity", "segment_speaker"}, {"segment_id", "sg_1"}, {"participant_id", nullptr}};
    undo.seq = 5;
    std::vector<OpResult> undo_results{undo};
    repo.on_ops_applied("m_1", {{"seq", 5}}, undo_results, "system", std::nullopt);
    CHECK(repo.event_is_undone("m_1", 2));
    const auto events = repo.list_events("m_1", std::nullopt, 100);
    CHECK(events.at(0).at("action") == "undo:2");
    CHECK(events.at(3).at("seq") == 2);
    CHECK(events.at(3).at("undoable") == false);
    CHECK(repo.list_events("m_1", 3, 1).size() == 1);

    // A failing mirror rolls the batch back.
    OpResult ghost = speaker;
    ghost.effect["segment_id"] = "sg_missing";
    ghost.seq = 6;
    std::vector<OpResult> failing{ghost};
    CHECK_THROWS_AS(repo.on_ops_applied("m_1", {{"seq", 6}}, failing, "host", std::nullopt),
                    fv::py::ValueError);
    CHECK_FALSE(repo.get_event("m_1", 6).has_value());

    // A one-occurrence correction must still match its passage.
    OpResult scoped;
    scoped.ok = true;
    scoped.op = {{"op", "add_item"},
                 {"card", "user_notes"},
                 {"evidence", {"sg_1"}},
                 {"data",
                  {{"kind", "occurrence_correction"},
                   {"base_fingerprint", segment_fingerprint("Entropic ships")}}}};
    std::vector<OpResult> stale{scoped};
    CHECK_THROWS_AS(repo.on_ops_applied("m_1", Json::object(), stale, "host", std::nullopt),
                    StaleCorrectionSelection);
    scoped.op["data"]["base_fingerprint"] = segment_fingerprint("Cleaned");
    std::vector<OpResult> fresh{scoped};
    CHECK_NOTHROW(repo.on_ops_applied("m_1", Json::object(), fresh, "host", std::nullopt));

    repo.fail_on_ops_applied("database unavailable");
    std::vector<OpResult> none;
    CHECK_THROWS_AS(repo.on_ops_applied("m_1", Json::object(), none, "host", std::nullopt),
                    std::runtime_error);
    CHECK(repo.ops_applied_calls().size() == 6);
}
