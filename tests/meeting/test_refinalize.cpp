// tests/test_meeting_refinalize.py, and the re-run cases of test_meeting_cloud_speakers.py,
// test_meeting_notes_agent.py and test_meeting_state.py: the headless post-meeting retry.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "meeting/finalization.h"
#include "meeting/model_lease.h"
#include "meeting/refinalize.h"
#include "meeting_capture/session_audio.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "recovery_support.h"

using fv::Json;
using fv::meeting_state::CardItem;
using fv::meeting_state::MeetingState;
using fv::meeting_state::MeetingStateStore;
using fv::meeting_state::TranscriptSegment;
using namespace fv::meeting;
using namespace fv::meeting::test;
using Catch::Matchers::ContainsSubstring;

namespace {

// Before OpenAI's shutdown day, so the speaker gate reads an "openai" backend as offered.
const fv::FakeClock& test_clock() {
    static const fv::FakeClock clock(
        std::chrono::sys_days{std::chrono::year{2026} / 1 / 1} + std::chrono::hours(12), "UTC");
    return clock;
}

TranscriptSegment segment(const std::string& id, const std::string& channel, double start,
                          double end, const std::string& text,
                          const std::string& meeting_id = "m_retry") {
    TranscriptSegment out;
    out.segment_id = id;
    out.meeting_id = meeting_id;
    out.channel = channel;
    out.start_s = start;
    out.end_s = end;
    out.text = text;
    return out;
}

// make_meeting: an ended meeting with a spool folder.
std::string make_meeting(SqlRepo& sql, const std::optional<std::string>& state_json,
                         bool cloud_enabled = true, const std::string& spool_dir = "/tmp/spool",
                         const std::string& meeting_id = "m_retry") {
    sql.repo->create_meeting({{"id", meeting_id},
                              {"title", "Budget sync"},
                              {"status", "ended"},
                              {"started_at", "2026-01-01T11:00:00"},
                              {"ended_at", "2026-01-01T11:30:00"},
                              {"host_token", "host-token"},
                              {"guest_token", "guest-token"},
                              {"cloud_enabled", cloud_enabled},
                              {"spool_dir", spool_dir},
                              {"agent_provider", "openrouter"},
                              {"agent_model", "test/model"},
                              {"state_json", state_json ? Json(*state_json) : Json()},
                              {"state_seq", 0}});
    return meeting_id;
}

void add_transcript(SqlRepo& sql, const std::string& meeting_id = "m_retry") {
    sql.repo->add_segments({
        segment("sg_1", "mic", 0.0, 2.0, "We should ship the budget review on Friday.", meeting_id),
        segment("sg_2", "loopback", 2.0, 4.0, "Agreed, Friday works.", meeting_id),
    });
}

std::string seeded_state(const std::string& meeting_id, const Json& steps,
                         bool cloud_enabled = true, bool card_deferred = false) {
    MeetingState state;
    state.meeting_id = meeting_id;
    state.status = "ended";
    state.cloud_enabled = cloud_enabled;
    state.title = "Budget sync";
    state.finalization.status = "failed";
    state.finalization.message = "needs retry";
    state.finalization.steps = steps;
    state.finalization.total_steps = static_cast<std::int64_t>(steps.size());
    state.finalization.card_deferred = card_deferred;
    return fv::py::dumps(state.to_json());
}

Json step(const std::string& id, const std::string& name, const std::string& status,
          const std::optional<std::string>& detail = std::nullopt) {
    Json out = {{"id", id}, {"name", name}, {"status", status}};
    if (detail) {
        out["detail"] = *detail;
    }
    return out;
}

const Json& default_steps() {
    static const Json steps = Json::array({
        step("redecode", "Audio Re-transcription", "failed",
             "Re-decoding failed; kept live transcript"),
        step("polish", "Transcript Cleanup", "completed", "Done"),
        step("consolidation", "Summary & Action Items", "completed", "Done"),
        step("finalize", "State Finalization", "completed", "Done"),
    });
    return steps;
}

const Json& speaker_steps() {
    static const Json steps = Json::array({
        step("speaker_id", "Speaker Identification", "failed", "api down"),
        step("finalize", "State Finalization", "completed", "Done"),
    });
    return steps;
}

std::vector<TranscriptSegment> rich_decode(const std::filesystem::path&, const std::vector<Json>&,
                                           const OfflineProgress&) {
    return {
        segment("sg_new_1", "mic", 0.0, 2.2,
                "We should ship the budget review on Friday as planned."),
        segment("sg_new_2", "loopback", 2.2, 4.4,
                "Agreed, Friday works for the whole team review."),
    };
}

RerunRequest request_for(SqlRepo& sql, const std::string& from_step) {
    RerunRequest request;
    request.repository = sql.repo;
    request.from_step = from_step;
    request.clock = &test_clock();
    return request;
}

std::set<std::string> segment_ids(SqlRepo& sql, const std::string& meeting_id = "m_retry") {
    std::set<std::string> ids;
    for (const Json& row : sql.repo->get_segments(meeting_id, -1.0, std::nullopt)) {
        ids.insert(row.at("id").get<std::string>());
    }
    return ids;
}

const Json& step_of(const Json& result, const std::string& id) {
    for (const Json& entry : result.at("finalization").at("steps")) {
        if (entry.at("id") == id) {
            return entry;
        }
    }
    throw std::runtime_error("no step " + id);
}

RedecodeRequest redecode_request(RedecodeFn transcribe = {}) {
    RedecodeRequest request;
    request.transcribe = std::move(transcribe);
    request.clock = &test_clock();
    return request;
}

// A spool folder holding a mic session WAV of one second of signal, for an engine to decode.
std::filesystem::path session_spool(SqlRepo& sql) {
    const std::filesystem::path spool = sql.folder("spool");
    write_wav(session_wav_path(spool, "mic"), std::vector<std::int16_t>(16000, 1000));
    return spool;
}

std::map<std::string, std::string> statuses(const Json& snapshot, const std::string& card) {
    std::map<std::string, std::string> out;
    for (const Json& item : snapshot.at("cards").at(card)) {
        out[item.at("id").get<std::string>()] = item.at("status").get<std::string>();
    }
    return out;
}

CardItem card_item(const std::string& id, const std::string& card,
                   const std::string& status = "proposed", bool pinned = false) {
    CardItem item;
    item.id = id;
    item.card = card;
    item.text = "text " + id;
    item.status = status;
    item.pinned = pinned;
    return item;
}

} // namespace

// ---- TestRedeocdeGuard ----

TEST_CASE("TestRedeocdeGuard::test_internal_decoder_typeerror_is_not_retried",
          "[meeting][refinalize]") {
    SqlRepo sql;
    const std::string meeting_id = make_meeting(sql, std::nullopt);
    add_transcript(sql);
    const auto before = sql.repo->get_segments(meeting_id, -1.0, std::nullopt);
    int calls = 0;

    const Json result = rerun_redecode(
        sql.repo, meeting_id,
        redecode_request([&calls](const std::filesystem::path&, const std::vector<Json>&,
                                  const OfflineProgress&) -> std::vector<TranscriptSegment> {
            ++calls;
            throw std::logic_error("decoder internal failure");
        }));

    CHECK(result == Json{{"ok", false}, {"error", "decoder internal failure"}});
    CHECK(calls == 1);
    CHECK(sql.repo->get_segments(meeting_id, -1.0, std::nullopt) == before);
}

TEST_CASE("TestRedeocdeGuard::test_sparse_redecode_keeps_draft", "[meeting][refinalize]") {
    for (const bool guard : {false, true}) {
        DYNAMIC_SECTION("guard " << guard) {
            SqlRepo sql;
            make_meeting(sql, seeded_state("m_retry", default_steps()));
            add_transcript(sql);
            RerunRequest request = request_for(sql, "redecode");
            request.transcribe = [](const std::filesystem::path&, const std::vector<Json>&,
                                    const OfflineProgress&) {
                return std::vector<TranscriptSegment>{segment("sg_sparse", "mic", 0.0, 1.0, "Hi")};
            };
            request.options.redecode_coverage_guard = guard;

            const Json result = rerun_finalization("m_retry", request);

            const std::set<std::string> ids = segment_ids(sql);
            CHECK(ids.contains("sg_1") == guard);
            CHECK(ids.contains("sg_sparse") == !guard);
            const Json& redecode = step_of(result, "redecode");
            CHECK(redecode.at("status") == (guard ? "failed" : "completed"));
            if (guard) {
                CHECK_THAT(redecode.at("detail").get<std::string>(),
                           ContainsSubstring("80% coverage threshold"));
                CHECK_THAT(redecode.at("detail").get<std::string>(),
                           ContainsSubstring("Kept the live transcript"));
                CHECK(result.at("ok") == false);
            }
        }
    }
}

TEST_CASE("TestRedeocdeGuard::test_successful_redecode_replaces_draft", "[meeting][refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    add_transcript(sql);
    RerunRequest request = request_for(sql, "redecode");
    request.transcribe = rich_decode;

    const Json result = rerun_finalization("m_retry", request);

    const std::set<std::string> ids = segment_ids(sql);
    CHECK(ids.contains("sg_new_1"));
    CHECK_FALSE(ids.contains("sg_1"));
    CHECK(step_of(result, "redecode").at("status") == "completed");
}

TEST_CASE("TestRedeocdeGuard::test_redecode_rebuilds_repairs_from_the_final_transcript",
          "[meeting][refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    add_transcript(sql);
    std::vector<std::vector<std::string>> repaired;
    RedecodeRequest request = redecode_request(rich_decode);
    request.repair = [&repaired](MeetingStateStore&, const std::vector<Json>& rows) {
        std::vector<std::string> ids;
        for (const Json& row : rows) {
            ids.push_back(row.at("id").get<std::string>());
        }
        repaired.push_back(std::move(ids));
        return 0;
    };

    const Json result = rerun_redecode(sql.repo, "m_retry", request);

    CHECK(result.at("ok") == true);
    CHECK(repaired == std::vector<std::vector<std::string>>{{"sg_new_1", "sg_new_2"}});
}

// ---- TestOneRunPerMeeting ----

TEST_CASE("TestOneRunPerMeeting::test_second_concurrent_retry_is_refused",
          "[meeting][refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    add_transcript(sql);
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool released = false;

    RerunRequest slow = request_for(sql, "redecode");
    slow.transcribe = [&](const std::filesystem::path& spool, const std::vector<Json>& chunks,
                          const OfflineProgress& progress) {
        {
            std::unique_lock lock(mutex);
            started = true;
            changed.notify_all();
            changed.wait_for(lock, std::chrono::seconds(10), [&] { return released; });
        }
        return rich_decode(spool, chunks, progress);
    };
    std::thread first([&] { rerun_finalization("m_retry", slow); });
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return started; }));
    }
    CHECK(is_running("m_retry"));
    CHECK_THROWS_AS(rerun_finalization("m_retry", request_for(sql, "polish")),
                    FinalizationBusyError);
    {
        const std::lock_guard lock(mutex);
        released = true;
    }
    changed.notify_all();
    first.join();

    const Json result = rerun_finalization("m_retry", request_for(sql, "finalize"));
    const std::string status = result.at("finalization").at("status").get<std::string>();
    CHECK((status == "completed" || status == "failed"));
}

// ---- TestCardDeferred ----

TEST_CASE("TestCardDeferred::test_rerun_preserves_card_deferred", "[meeting][refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps(), true, true));
    add_transcript(sql);
    RerunRequest request = request_for(sql, "redecode");
    request.transcribe = rich_decode;

    const Json result = rerun_finalization("m_retry", request);

    CHECK(result.at("finalization").at("card_deferred") == true);
    CHECK(result.at("state").at("finalization").at("card_deferred") == true);
    const Json stored =
        fv::py::loads(sql.repo->get_meeting("m_retry")->at("state_json").get<std::string>());
    CHECK(stored.at("finalization").at("card_deferred") == true);
}

// ---- TestRedecodeReleasesModel ----
// The engine branch of rerun_redecode must free its model: every other test injects a decoder,
// which skips it. The engine is destroyed before the lease's release runs.

TEST_CASE("TestRedecodeReleasesModel::test_model_is_released_after_a_successful_redecode",
          "[meeting][refinalize]") {
    SqlRepo sql;
    const std::filesystem::path spool = session_spool(sql);
    make_meeting(sql, seeded_state("m_retry", default_steps()), true, fv::path_to_utf8(spool));
    SpeechRig rig;
    rig.script->set_fallback(
        {{fv::Segment{"Re-decoded transcript text for the meeting.", 0.0, 1.0}}, std::nullopt});
    rig.select();
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    std::vector<std::string> order;
    FunctionModelLease lease(
        [&order] {
            order.emplace_back("acquire");
            return true;
        },
        [&order] { order.emplace_back("release"); });
    RedecodeRequest request = redecode_request();
    request.asr_model_name = "base";
    request.asr = recording_factory(rig, sql.repo, calls);
    request.model_lease = &lease;

    const Json result = rerun_redecode(sql.repo, "m_retry", request);

    CHECK(result.at("ok") == true);
    REQUIRE(calls->size() == 1);
    CHECK(calls->front().model == "base");
    CHECK(calls->front().language == "auto");
    const auto decodes = rig.script->calls();
    REQUIRE_FALSE(decodes.empty());
    CHECK(decodes.front().first == fv::speech::Lane::Background);
    CHECK(order == std::vector<std::string>{"acquire", "release"});
}

TEST_CASE("TestRedecodeReleasesModel::test_lease_brackets_the_model_load",
          "[meeting][refinalize]") {
    SqlRepo sql;
    const std::filesystem::path spool = session_spool(sql);
    make_meeting(sql, seeded_state("m_retry", default_steps()), true, fv::path_to_utf8(spool));
    SpeechRig rig;
    rig.script->set_fallback(
        {{fv::Segment{"Re-decoded transcript text for the meeting.", 0.0, 1.0}}, std::nullopt});
    rig.select();
    std::vector<std::string> order;
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    FunctionModelLease lease(
        [&order] {
            order.emplace_back("acquire");
            return true;
        },
        [&order] { order.emplace_back("release"); });
    RedecodeRequest request = redecode_request();
    request.asr_model_name = "base";
    request.asr = recording_factory(rig, sql.repo, calls, [&order] { order.emplace_back("load"); });
    request.model_lease = &lease;

    const Json result = rerun_redecode(sql.repo, "m_retry", request);

    CHECK(result.at("ok") == true);
    CHECK(order == std::vector<std::string>{"acquire", "load", "release"});
    REQUIRE(calls->size() == 1);
    CHECK(calls->front().model == "base");
}

TEST_CASE("TestRedecodeReleasesModel::test_lease_release_is_skipped_when_no_model_loads",
          "[meeting][refinalize]") {
    // An injected decoder needs no speech model, so no lease is taken.
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    std::vector<std::string> order;
    FunctionModelLease lease(
        [&order] {
            order.emplace_back("acquire");
            return true;
        },
        [&order] { order.emplace_back("release"); });
    RedecodeRequest request = redecode_request(
        [](const std::filesystem::path&, const std::vector<Json>&, const OfflineProgress&) {
            return std::vector<TranscriptSegment>{
                segment("sg_1", "mic", 0.0, 2.0, "Injected transcript text for the meeting.")};
        });
    request.model_lease = &lease;

    rerun_redecode(sql.repo, "m_retry", request);

    CHECK(order.empty());
}

TEST_CASE("TestRedecodeReleasesModel::test_model_is_released_when_it_fails_to_load",
          "[meeting][refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    SpeechRig rig;
    rig.script->set_ready({fv::speech::ReadyState::ModelMissing,
                           "Download Parakeet TDT 0.6B v3 in Downloads.",
                           "Download Parakeet TDT 0.6B v3 in Downloads."});
    rig.select();
    std::vector<std::string> order;
    auto calls = std::make_shared<std::vector<FactoryCall>>();
    FunctionModelLease lease(
        [&order] {
            order.emplace_back("acquire");
            return true;
        },
        [&order] { order.emplace_back("release"); });
    RedecodeRequest request = redecode_request();
    request.asr_model_name = "base";
    request.asr = recording_factory(rig, sql.repo, calls, [&order] { order.emplace_back("load"); });
    request.model_lease = &lease;

    const Json result = rerun_redecode(sql.repo, "m_retry", request);

    CHECK(result.at("ok") == false);
    // The speech service's own sentence for a missing model (OpenWhisper: "The Whisper model is
    // not available yet. Approve the download and retry.").
    CHECK(result.at("error") == "Download Parakeet TDT 0.6B v3 in Downloads.");
    CHECK(order == std::vector<std::string>{"acquire", "load", "release"});
    CHECK(rig.script->calls().empty());
}

// ---- TestSpeakerGate ----

TEST_CASE("TestSpeakerGate::test_refused_pass_is_skipped_without_uploading",
          "[meeting][refinalize]") {
    struct Case {
        std::string backend;
        bool consent;
        std::string reason;
    };
    for (const Case& c :
         {Case{"openai", false, "consent"}, Case{"local", true, "not set to OpenAI"},
          Case{"off", true, "not set to OpenAI"}}) {
        DYNAMIC_SECTION(c.backend << " consent " << c.consent) {
            SqlRepo sql;
            make_meeting(sql, seeded_state("m_retry", speaker_steps(), false), false);
            add_transcript(sql);
            std::vector<Json> snapshots;
            RerunRequest request = request_for(sql, "speaker_id");
            request.options.speaker_id_backend = c.backend;
            request.options.speaker_audio_consent = c.consent;
            request.progress = [&snapshots](const Json& snapshot) {
                snapshots.push_back(snapshot);
            };

            const Json result = rerun_finalization("m_retry", request);

            const Json& speaker = step_of(result, "speaker_id");
            CHECK(speaker.at("status") == "completed");
            CHECK_THAT(speaker.at("detail").get<std::string>(), ContainsSubstring(c.reason));
            CHECK(result.at("ok") == true);
            for (const Json& snapshot : snapshots) {
                CHECK_THAT(snapshot.value("step_details", ""), !ContainsSubstring("Uploading"));
            }
        }
    }
}

TEST_CASE("TestSpeakerGate::test_missing_key_is_skipped_like_live_end", "[meeting][refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", speaker_steps(), false), false);
    add_transcript(sql);
    RerunRequest request = request_for(sql, "speaker_id");
    request.options.speaker_id_backend = "openai";
    request.options.speaker_audio_consent = true;

    const Json result = rerun_finalization("m_retry", request);

    const Json& speaker = step_of(result, "speaker_id");
    CHECK(speaker.at("status") == "completed");
    CHECK(speaker.at("detail") == "No OpenAI API key is configured.");
    CHECK(result.at("ok") == true);
}

// ---- TestRedecodeSpeakerLabels ----

TEST_CASE("TestRedecodeSpeakerLabels::test_off_backend_does_not_rediarize",
          "[meeting][refinalize]") {
    SqlRepo sql;
    make_meeting(sql, seeded_state("m_retry", default_steps()));
    add_transcript(sql);
    int created = 0;
    RedecodeRequest request = redecode_request(rich_decode);
    request.speaker_id_backend = "off";
    request.diarizer_factory = [&created](fv::diarize::DiarizerHost&, const std::string&) {
        ++created;
        return std::unique_ptr<fv::diarize::Diarizer>();
    };

    const Json result = rerun_redecode(sql.repo, "m_retry", request);

    CHECK(result.at("ok") == true);
    CHECK(created == 0);
}

TEST_CASE("every speaker backend but off re-diarizes a redecoded transcript",
          "[meeting][refinalize]") {
    // Lead amendment 26: the test is != "off", so "openai" keeps the on-device diarizer.
    for (const std::string backend : {"local", "openai"}) {
        DYNAMIC_SECTION(backend) {
            SqlRepo sql;
            make_meeting(sql, seeded_state("m_retry", default_steps()));
            add_transcript(sql);
            int created = 0;
            RedecodeRequest request = redecode_request(rich_decode);
            request.speaker_id_backend = backend;
            request.diarizer_factory = [&created](fv::diarize::DiarizerHost&, const std::string&) {
                ++created;
                return std::unique_ptr<fv::diarize::Diarizer>();
            };
            CHECK(rerun_redecode(sql.repo, "m_retry", request).at("ok") == true);
            CHECK(created == 1);
        }
    }
}

namespace {

// Labels every slice it hears with one participant.
class FixedDiarizer final : public fv::diarize::Diarizer {
public:
    FixedDiarizer(std::string participant, std::vector<std::pair<std::string, std::size_t>>& heard)
        : participant_(std::move(participant)), heard_(heard) {}
    std::optional<std::string> assign(fv::diarize::Segment& segment, std::span<const float> audio,
                                      int /*rate*/) override {
        heard_.emplace_back(segment.id, audio.size());
        return participant_;
    }
    void pin(const std::string&, const std::string&) override {}
    bool is_available() const override { return true; }

private:
    std::string participant_;
    std::vector<std::pair<std::string, std::size_t>>& heard_;
};

} // namespace

TEST_CASE("a redecode labels system audio from the whole session through a stored speaker",
          "[meeting][refinalize]") {
    SqlRepo sql;
    const std::filesystem::path spool = sql.folder("spool");
    write_wav(session_wav_path(spool, "loopback"), std::vector<std::int16_t>(5 * 16000, 1000));
    make_meeting(sql, seeded_state("m_retry", default_steps()), true, fv::path_to_utf8(spool));
    add_transcript(sql);
    std::vector<std::pair<std::string, std::size_t>> heard;
    std::optional<std::string> speaker;
    RedecodeRequest request = redecode_request(rich_decode);
    request.diarizer_factory = [&](fv::diarize::DiarizerHost& host, const std::string& meeting_id) {
        CHECK(meeting_id == "m_retry");
        if (auto created = host.create_participant("Speaker 1")) {
            speaker = std::move(*created);
        }
        return std::unique_ptr<fv::diarize::Diarizer>(
            std::make_unique<FixedDiarizer>(speaker.value_or(""), heard));
    };

    const Json result = rerun_redecode(sql.repo, "m_retry", request);

    REQUIRE(result.at("ok") == true);
    REQUIRE(speaker);
    // Only system audio, sliced from the session recording: 2.2 s to 4.4 s at 16 kHz.
    CHECK(heard == std::vector<std::pair<std::string, std::size_t>>{{"sg_new_2", 35200}});
    const auto row = sql.repo->get_segment("m_retry", "sg_new_2");
    REQUIRE(row);
    CHECK(row->at("speaker_participant_id") == *speaker);
    CHECK(row->at("speaker_source") == "diarizer");
    CHECK(sql.repo->get_segment("m_retry", "sg_new_1")->at("speaker_participant_id").is_null());
}

// ---- borrowed claims ----

TEST_CASE("test_borrowed_claim_remains_exclusive_until_caller_releases_it",
          "[meeting][refinalize]") {
    const auto pipeline = [](const std::string& meeting_id, FinalizationClaim* claim) {
        return run_claimed(meeting_id, claim, [&] {
            CHECK(is_running(meeting_id));
            return Json{{"ok", true}};
        });
    };
    auto claim = std::make_unique<FinalizationClaim>("m_claim_success");
    CHECK(pipeline(claim->meeting_id(), claim.get()).at("ok") == true);
    CHECK(is_running(claim->meeting_id()));
    claim->release();
    CHECK_FALSE(is_running(claim->meeting_id()));
    try {
        pipeline(claim->meeting_id(), claim.get());
        FAIL("no ValueError");
    } catch (const fv::py::ValueError& error) {
        CHECK_THAT(error.what(), ContainsSubstring("inactive or mismatched"));
    }
}

TEST_CASE("test_borrowed_claim_survives_pipeline_failure_and_releases_idempotently",
          "[meeting][refinalize]") {
    const auto pipeline = [](const std::string& meeting_id, FinalizationClaim* claim) {
        return run_claimed(meeting_id, claim, [&meeting_id]() -> Json {
            // Conditional, so the compiler does not see a call that never returns.
            if (!meeting_id.empty()) {
                throw std::runtime_error("synthetic pipeline failure");
            }
            return Json();
        });
    };
    FinalizationClaim claim("m_claim_failure");
    try {
        pipeline(claim.meeting_id(), &claim);
        FAIL("no failure");
    } catch (const std::runtime_error& error) {
        CHECK_THAT(error.what(), ContainsSubstring("synthetic pipeline failure"));
    }
    CHECK(is_running(claim.meeting_id()));
    try {
        pipeline("m_other", &claim);
        FAIL("no ValueError");
    } catch (const fv::py::ValueError& error) {
        CHECK_THAT(error.what(), ContainsSubstring("inactive or mismatched"));
    }
    claim.release();
    {
        FinalizationClaim next_claim(claim.meeting_id());
        claim.release();
        CHECK(is_running(claim.meeting_id()));
        CHECK_FALSE(claim.is_current());
        CHECK(next_claim.is_current());
        next_claim.release();
    }
    CHECK_FALSE(is_running(claim.meeting_id()));
}

// ---- test_meeting_cloud_speakers.py TestRespeaker ----

namespace {

std::string seed_speaker_meeting(SqlRepo& sql, const std::string& meeting_id = "m_spk") {
    sql.repo->create_meeting({{"id", meeting_id},
                              {"title", "Sync"},
                              {"status", "ended"},
                              {"started_at", "2026-01-01T11:00:00"},
                              {"host_token", "h"},
                              {"guest_token", "g"},
                              {"cloud_enabled", false},
                              {"spool_dir", "/tmp/spool"},
                              {"state_json", nullptr},
                              {"state_seq", 0}});
    TranscriptSegment pinned = segment("sg_pin", "loopback", 2.0, 4.0, "pinned", meeting_id);
    pinned.speaker_source = "human";
    pinned.speaker_pinned = true;
    sql.repo->add_segments({segment("sg_lb", "loopback", 0.0, 2.0, "hello", meeting_id), pinned});
    return meeting_id;
}

} // namespace

TEST_CASE("TestRespeaker::test_rerun_speakers_uses_headless_store", "[meeting][refinalize]") {
    // The headless store answers with the meeting's state. The upload pass is not in this build,
    // so an eligible gate reports it unavailable and labels nothing.
    SqlRepo sql;
    const std::string meeting_id = seed_speaker_meeting(sql);
    SpeakerPassGate gate;
    gate.ok = true;
    gate.offered = true;
    RerunRequest request = request_for(sql, "speaker_id");

    const Json result = rerun_speakers(meeting_id, request, gate);

    CHECK(result.at("ok") == false);
    CHECK(result.at("skipped") == false);
    CHECK(result.at("error") == std::string(kSpeakerPassUnavailable));
    CHECK(result.at("state").at("meeting_id") == meeting_id);
    CHECK(sql.repo->get_segment(meeting_id, "sg_lb")->at("speaker_participant_id").is_null());
    CHECK_FALSE(is_running(meeting_id));
}

TEST_CASE("TestRespeaker::test_unknown_meeting_raises", "[meeting][refinalize]") {
    SqlRepo sql;
    SpeakerPassGate gate;
    gate.ok = true;
    try {
        rerun_speakers("m_missing", request_for(sql, "speaker_id"), gate);
        FAIL("no ValueError");
    } catch (const fv::py::ValueError& error) {
        CHECK_THAT(error.what(), ContainsSubstring("unknown meeting"));
    }
}

TEST_CASE("TestRespeaker::test_refused_gate_uploads_nothing", "[meeting][refinalize]") {
    SqlRepo sql;
    const std::string meeting_id = seed_speaker_meeting(sql);
    SpeakerPassGate gate;
    gate.reason = "Audio-upload consent has not been given.";

    const Json result = rerun_speakers(meeting_id, request_for(sql, "speaker_id"), gate);

    CHECK(result.at("skipped") == true);
    CHECK(result.at("error") == "Audio-upload consent has not been given.");
}

// ---- test_meeting_notes_agent.py TestRedecodeStrip ----

TEST_CASE("TestRedecodeStrip::test_redecode_strip_keeps_live_notes", "[meeting][refinalize]") {
    MeetingState state;
    state.meeting_id = "m_strip";
    state.cards["live_notes"].push_back(card_item("it_note_prop", "live_notes"));
    state.cards["live_notes"].push_back(card_item("it_note_edit", "live_notes", "edited"));
    state.cards["live_notes"].push_back(card_item("it_note_pin", "live_notes", "proposed", true));
    state.cards["key_points"].push_back(card_item("it_key_prop", "key_points"));
    state.cards["key_points"].push_back(card_item("it_key_edit", "key_points", "confirmed"));
    MeetingStateStore store(state);

    strip_unevidenced_proposed(store);

    // The notes page survives the re-decode strip entirely...
    const Json snapshot = store.snapshot();
    CHECK(statuses(snapshot, "live_notes") ==
          std::map<std::string, std::string>{{"it_note_prop", "proposed"},
                                             {"it_note_edit", "edited"},
                                             {"it_note_pin", "proposed"}});
    // ...while proposed items on other cards are removed.
    CHECK(statuses(snapshot, "key_points") ==
          std::map<std::string, std::string>{{"it_key_prop", "removed"},
                                             {"it_key_edit", "confirmed"}});
}

TEST_CASE("TestRedecodeStrip::test_redecode_strip_keeps_evidenced_proposed_items",
          "[meeting][refinalize]") {
    MeetingState state;
    state.meeting_id = "m_redecode";
    CardItem alive = card_item("it_key_alive", "key_points");
    alive.text = "anchored";
    alive.evidence = {"sg_new1"};
    CardItem ghost = card_item("it_key_ghost", "key_points");
    ghost.text = "ghost";
    CardItem edited = card_item("it_key_edit", "key_points", "edited");
    edited.text = "edited";
    state.cards["key_points"] = {alive, ghost, edited};
    CardItem note = card_item("it_note_ghost", "live_notes");
    note.text = "note";
    state.cards["live_notes"].push_back(note);
    MeetingStateStore store(state);

    strip_unevidenced_proposed(store);

    CHECK(statuses(store.snapshot(), "key_points") ==
          std::map<std::string, std::string>{{"it_key_alive", "proposed"},
                                             {"it_key_ghost", "removed"},
                                             {"it_key_edit", "edited"}});
    CHECK(statuses(store.snapshot(), "live_notes") ==
          std::map<std::string, std::string>{{"it_note_ghost", "proposed"}});
}

// ---- test_meeting_state.py ----

TEST_CASE("test_redecode_cleanup_preserves_spoken_notes", "[meeting][refinalize]") {
    MeetingState state;
    state.meeting_id = "spoken-finalization";
    CardItem spoken = card_item("spoken", "key_points");
    spoken.text = "Budget A needs $1,000.";
    spoken.author_type = "system";
    spoken.author_id = "voice_command";
    CardItem automatic = card_item("automatic", "key_points");
    automatic.text = "An obsolete draft.";
    state.cards["key_points"] = {spoken, automatic};
    MeetingStateStore store(state);

    strip_unevidenced_proposed(store);

    CHECK(statuses(store.snapshot(), "key_points") ==
          std::map<std::string, std::string>{{"spoken", "proposed"}, {"automatic", "removed"}});
}

// The Python tests assert the backend's cleanup() and stop() before the lease's release: here the
// redecode engine is stopped and destroyed (its worker joined) when the lease gives the dictation
// model back, so no meeting decode overlaps the dictation engine's reload.
TEST_CASE("a redecode's engine is stopped and destroyed before the model lease is released",
          "[meeting][refinalize]") {
    SqlRepo sql;
    const std::filesystem::path spool = session_spool(sql);
    make_meeting(sql, seeded_state("m_retry", default_steps()), true, fv::path_to_utf8(spool));
    SpeechRig rig;
    rig.script->set_fallback(
        {{fv::Segment{"Re-decoded transcript text for the meeting.", 0.0, 1.0}}, std::nullopt});
    rig.select();
    auto alive = std::make_shared<std::weak_ptr<void>>();
    std::vector<std::string> order;
    FunctionModelLease lease(
        [&order] {
            order.emplace_back("acquire");
            return true;
        },
        [&order, alive] {
            order.emplace_back(alive->expired() ? "release after the engine"
                                                : "release with the engine alive");
        });
    RedecodeRequest request = redecode_request();
    request.asr_model_name = "base";
    request.asr = tracking_factory(rig, sql.repo, alive);
    request.model_lease = &lease;

    const Json result = rerun_redecode(sql.repo, "m_retry", request);

    CHECK(result.at("ok") == true);
    CHECK(order == std::vector<std::string>{"acquire", "release after the engine"});
}
