// SpeechModelLease when the separate-model setting changes between two overlapping passes, and
// the finalization retry's polish and consolidation steps catching only what OpenWhisper's
// _retry_polish (rerun_polish) and _retry_consolidation catch.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "meeting/model_lease.h"
#include "meeting/refinalize.h"
#include "meeting/testing/fake_intelligence.h"
#include "pyport/errors.h"
#include "recovery_support.h"

using fv::Json;
using fv::speech::ReadyState;
using fv::speech::Slot;
using namespace fv::meeting;
using namespace fv::meeting::test;
using Catch::Matchers::ContainsSubstring;

namespace {

fv::speech::Readiness settled(SpeechRig& rig, Slot slot) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (rig.service.readiness(slot).state == ReadyState::Loading &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return rig.service.readiness(slot);
}

// An ended meeting with AI insights on and one transcript row; get_segments throws while
// `segments_fail` is set.
std::shared_ptr<ScriptedRepository> ended_meeting(bool segments_fail = false) {
    auto repository = std::make_shared<ScriptedRepository>();
    repository->create_meeting(Json{{"id", "m_retry"},
                                    {"title", "Budget sync"},
                                    {"status", "ended"},
                                    {"started_at", "2026-01-01T11:00:00"},
                                    {"ended_at", "2026-01-01T11:30:00"},
                                    {"host_token", "host-token"},
                                    {"guest_token", "guest-token"},
                                    {"cloud_enabled", true},
                                    {"spool_dir", "/tmp/spool"},
                                    {"agent_provider", "openrouter"},
                                    {"agent_model", "test/model"},
                                    {"state_json", Json()},
                                    {"state_seq", 0}});
    repository->set_segments(std::vector<Json>{Json{{"segment_id", "sg_1"},
                                                    {"meeting_id", "m_retry"},
                                                    {"channel", "mic"},
                                                    {"start_s", 0.0},
                                                    {"end_s", 2.0},
                                                    {"text", "Hold the price."}}},
                             segments_fail);
    return repository;
}

RerunRequest request(const std::shared_ptr<ScriptedRepository>& repository,
                     const std::shared_ptr<FakeIntelligence>& intelligence,
                     const std::string& from_step) {
    RerunRequest out;
    out.repository = repository;
    out.from_step = from_step;
    out.intelligence = intelligence;
    return out;
}

const Json* step(const Json& result, const std::string& id) {
    for (const Json& entry : result.at("finalization").at("steps")) {
        if (entry.value("id", std::string()) == id) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("a pass that overlaps one holding the dictation model keeps that kind of hold",
          "[meeting][refinalize]") {
    // The setting turns "separate" between the two acquires: the second pass shares the first
    // one's engine lease, and dictation is never released under it.
    SpeechRig rig;
    rig.select();
    REQUIRE(settled(rig, Slot::Dictation).state == ReadyState::Ready);
    bool separate = false;
    SpeechModelLease lease(rig.service, [&] { return separate; });

    REQUIRE(lease.acquire());
    separate = true;
    REQUIRE(lease.acquire());
    CHECK(rig.service.lease_holders().size() == 2);
    CHECK(rig.service.readiness(Slot::Dictation).state == ReadyState::Ready);
    lease.release();
    CHECK(rig.service.lease_holders().size() == 1);
    lease.release();
    CHECK(rig.service.lease_holders().empty());
    CHECK(rig.service.readiness(Slot::Dictation).state == ReadyState::Ready);

    // The next independent pass reads the setting again.
    REQUIRE(lease.acquire());
    CHECK(rig.service.lease_holders().empty());
    CHECK(rig.service.readiness(Slot::Dictation).state == ReadyState::Offline);
    lease.release();
    CHECK(settled(rig, Slot::Dictation).state == ReadyState::Ready);
}

TEST_CASE("a polish retry whose transcript cannot be read ends the retry",
          "[meeting][refinalize]") {
    // rerun_polish reads the segments outside its try, so the error leaves _retry_polish.
    auto repository = ended_meeting(true);
    auto intelligence = std::make_shared<FakeIntelligence>();
    CHECK_THROWS_WITH(rerun_finalization("m_retry", request(repository, intelligence, "polish")),
                      ContainsSubstring("database is locked"));
    CHECK(intelligence->polished().empty());
}

TEST_CASE("a consolidation retry records a ValueError and lets anything else out",
          "[meeting][refinalize]") {
    auto repository = ended_meeting();
    auto intelligence = std::make_shared<FakeIntelligence>();
    intelligence->script_consolidate({{"value_error", "meeting has no transcript"}});
    const Json result =
        rerun_finalization("m_retry", request(repository, intelligence, "consolidation"));
    const Json* consolidation = step(result, "consolidation");
    REQUIRE(consolidation != nullptr);
    CHECK(consolidation->at("status") == "failed");
    CHECK(consolidation->at("detail") == "meeting has no transcript");

    intelligence->script_consolidate({{"throw", "provider down"}});
    CHECK_THROWS_WITH(
        rerun_finalization("m_retry", request(repository, intelligence, "consolidation")),
        ContainsSubstring("provider down"));
}
