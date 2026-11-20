#pragma once

// tests/test_meeting_export.py _fixture(): one meeting row, its state document and two
// segments, shared by the export tests. Dates are legacy naive stamps, so they read the same in
// every zone; the clock is in America/New_York, the zone the export vectors pin.

#include <vector>

#include "base/json.h"
#include "base/testing/fake_clock.h"

namespace fv::meeting_state::test_support {

inline const fv::FakeClock& export_clock() {
    static const fv::FakeClock clock(fv::FakeClock::default_utc(), "America/New_York");
    return clock;
}

struct ExportFixture {
    const fv::FakeClock& clock = export_clock();
    Json meeting = Json::parse(R"({
        "id": "m_export", "title": "Planning Sync", "status": "ended",
        "started_at": "2026-03-15T14:30:00", "ended_at": "2026-03-15T15:00:00",
        "paused_total_s": 0.0, "cloud_enabled": false, "asr_model": "base",
        "host_token": "SECRET_HOST_TOKEN_XXXX", "guest_token": "SECRET_GUEST_TOKEN_YYYY",
        "app_pid": 12345, "app_heartbeat_at": "2026-03-15T15:00:00",
        "state_json": "{\"seq\": 9}", "spool_dir": "/tmp/spool"})");
    Json state = Json::parse(R"({
        "meeting_id": "m_export", "seq": 9, "title": "Planning Sync", "status": "ended",
        "topic": {"current": "Q3 roadmap", "history": [
            {"text": "Kickoff", "ts": "2026-03-15T14:30:00"},
            {"text": "Q3 roadmap", "ts": "2026-03-15T14:45:00"}]},
        "rolling_summary": "Team aligned on milestones.",
        "cards": {
            "key_points": [{"id": "it_1", "card": "key_points", "text": "Ship beta in June",
                            "status": "proposed", "data": {}, "pinned": false,
                            "evidence": ["sg_1"]}],
            "decisions": [{"id": "it_2", "card": "decisions", "text": "Use FastAPI",
                           "status": "confirmed", "data": {}, "pinned": true, "evidence": []}],
            "action_items": [{"id": "it_3", "card": "action_items", "text": "Draft RFC",
                              "status": "proposed", "data": {"owner_participant_id": "p_me"},
                              "pinned": false, "evidence": []}],
            "risks": [{"id": "it_4", "card": "risks", "text": "Vendor delay",
                       "status": "proposed", "data": {"severity": "high"}, "pinned": false,
                       "evidence": []}],
            "timeline": [{"id": "it_5", "card": "timeline", "text": "Kickoff done",
                          "status": "proposed", "data": {"start_s": 90}, "pinned": false,
                          "evidence": []}],
            "user_notes": [{"id": "it_gone", "card": "user_notes", "text": "scratch",
                            "status": "removed", "data": {}, "pinned": false, "evidence": []}]},
        "participants": {
            "p_me": {"id": "p_me", "display_name": "Alex", "kind": "me"},
            "p_2": {"id": "p_2", "display_name": "Sam", "kind": "guest"}},
        "questions": [
            {"id": "q_1", "text": "Deadline firm?", "status": "open",
             "suggested_answer": "Yes, June 30", "suggested_confidence": 0.6},
            {"id": "q_2", "text": "Who owns RFC?", "status": "resolved", "answer": "Alex",
             "answer_source": "audio", "resolved_by": "p_me"},
            {"id": "q_3", "text": "Ignored", "status": "dismissed"}],
        "cloud_enabled": false, "intelligence_online": true})");
    std::vector<Json> segments{
        Json::parse(R"({"id": "sg_1", "start_s": 5.0, "end_s": 8.0, "text": "Welcome everyone",
                        "channel": "mic", "speaker_participant_id": "p_me"})"),
        Json::parse(R"({"id": "sg_2", "start_s": 10.0, "end_s": 14.0, "text": "Let's plan Q3",
                        "channel": "loopback", "speaker_participant_id": "p_2"})")};
};

} // namespace fv::meeting_state::test_support
