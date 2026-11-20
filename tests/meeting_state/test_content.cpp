// Durable meeting content capabilities (tests/test_meeting_content.py). OpenWhisper's _Repo
// serves chunks and segments without the aggregate read, which the in-memory repository does
// here; the SQL repository's aggregate path is pinned by the golden vectors and the fixture.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "base/testing/fake_clock.h"
#include "meeting_state/content.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

void add_chunk(testing::FakeMeetingRepository& repo, const std::string& channel, int seq,
               double duration_s) {
    repo.register_chunk({{"meeting_id", "m_1"},
                         {"channel", channel},
                         {"seq", seq},
                         {"file_path", "c.wav"},
                         {"start_s", 0.0},
                         {"duration_s", duration_s},
                         {"sample_rate", 16000}});
}

} // namespace

// from tests/test_meeting_content.py::test_empty_meeting_has_no_content_capabilities
TEST_CASE("test_empty_meeting_has_no_content_capabilities", "[test_meeting_content]") {
    fv::FakeClock clock;
    testing::FakeMeetingRepository repo(clock);
    const Json summary = summarize_meeting_content(repo, "m_empty");
    CHECK(summary["is_empty"] == true);
    CHECK(summary["has_audio"] == false);
    CHECK(summary["has_transcript"] == false);
    CHECK(summary["can_rerun_speakers"] == false);
    CHECK(summary["preview_text"] == "");
}

// from tests/test_meeting_content.py::test_speaker_rerun_requires_nonempty_loopback_audio
TEST_CASE("test_speaker_rerun_requires_nonempty_loopback_audio", "[test_meeting_content]") {
    fv::FakeClock clock;
    testing::FakeMeetingRepository repo(clock);
    add_chunk(repo, "mic", 0, 3.0);
    add_chunk(repo, "loopback", 0, 4.0);
    repo.add_segments({make_segment("m_1", "sg_1", 1.0, 2.0, "hello")});
    const Json summary = summarize_meeting_content(repo, "m_1");
    CHECK(summary["is_empty"] == false);
    CHECK(summary["has_audio"] == true);
    CHECK(summary["has_transcript"] == true);
    CHECK(summary["can_rerun_speakers"] == true);
    CHECK(summary["preview_text"] == "hello");
}

// from tests/test_meeting_content.py::test_zero_duration_chunks_and_blank_segments_are_not_content
TEST_CASE("test_zero_duration_chunks_and_blank_segments_are_not_content",
          "[test_meeting_content]") {
    fv::FakeClock clock;
    testing::FakeMeetingRepository repo(clock);
    add_chunk(repo, "loopback", 0, 0.0);
    repo.add_segments({make_segment("m_1", "sg_1", 1.0, 2.0, "   ")});
    const Json summary = summarize_meeting_content(repo, "m_1");
    CHECK(summary["is_empty"] == true);
    CHECK(summary["audio_chunks"] == 0);
    CHECK(summary["transcript_segments"] == 0);
    CHECK(summary["preview_text"] == "");
}

// from tests/test_meeting_content.py::test_meeting_preview_prefers_summary_then_topic
TEST_CASE("test_meeting_preview_prefers_summary_then_topic", "[test_meeting_content]") {
    CHECK(meeting_preview_text({{"content_summary", {{"preview_text", "Hello from ASR"}}},
                                {"state_json", R"({"topic": {"current": "Ignored topic"}})"}}) ==
          "Hello from ASR");
    CHECK(
        meeting_preview_text({{"state_json", R"({"topic": {"current": "Quarterly roadmap"}})"}}) ==
        "Quarterly roadmap");
    CHECK(meeting_preview_text(Json::object()).empty());
}

TEST_CASE("the preview joins segments until 100 characters, then clips", "[meeting_state_store]") {
    fv::FakeClock clock;
    testing::FakeMeetingRepository repo(clock);
    std::vector<TranscriptSegment> segments;
    for (int index = 0; index < 5; ++index) {
        segments.push_back(
            make_segment("m_1", "sg_" + std::to_string(index), index, index + 1.0,
                         "  " + std::string(30, static_cast<char>('a' + index)) + "  "));
    }
    repo.add_segments(segments);
    const std::string preview =
        summarize_meeting_content(repo, "m_1")["preview_text"].get<std::string>();
    // Four segments reach 120 characters; 100 are kept, then the ellipsis.
    CHECK(preview == std::string(30, 'a') + " " + std::string(30, 'b') + " " +
                         std::string(30, 'c') + " " + std::string(7, 'd') + "…");
    CHECK(summarize_meeting_content(repo, "m_1")["transcript_segments"] == 5);

    SqlFixture f;
    make_meeting(*f.repo, "m_sql");
    f.repo->add_segments({make_segment("m_sql", "sg_x", 1.0, 2.0, "from the aggregate read")});
    CHECK(summarize_meeting_content(*f.repo, "m_sql")["preview_text"] == "from the aggregate read");
}
