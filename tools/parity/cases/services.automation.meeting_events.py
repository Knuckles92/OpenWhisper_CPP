"""Meeting engine events as public meeting.* events (services/automation/meeting_events.py,
M5.8 voice-routing).

    forward(steps)   a fresh EventBus and no meeting announced yet, then forward(kind, payload,
                     meeting_id=meeting_id) for each [kind, payload, meeting_id] step; the
                     [type, data] of every event published, oldest first

The module's own record of announced meetings is set aside for the run and put back after it.
"""


def _forward(module, steps):
    from services.automation import events

    bus = events.EventBus()
    previous = events.set_bus(bus)
    saved = module._announced.copy()
    module._announced.clear()
    try:
        for kind, payload, meeting_id in steps:
            module.forward(kind, payload, meeting_id=meeting_id)
        return [[event.type, event.data] for event in bus.recent(500)]
    finally:
        events.set_bus(previous)
        bus.close()
        module._announced.clear()
        module._announced.update(saved)


WRAPPERS = {"forward": _forward}

_PRIVATE = {"_retry_generation": 3, "_finalization_claim": {"owner": "x"}}
_ROW = {"id": "sg_1", "meeting_id": "m_1", "chunk_id": 4, "channel": "mic", "start_s": 1.23456,
        "end_s": 3.5, "text": "Ship it Friday.", "original_text": "ship it friday",
        "speaker_participant_id": "p_me", "speaker_source": "mic", "speaker_pinned": False,
        "created_at": "x"}

CASES = {
    "forward": [
        # The first running status announces a meeting once; a pause and resume do not.
        [[["server_started", {"url": "http://127.0.0.1:9/m?token=guest-secret"}, "m_1"],
          ["status", {"status": "starting", **_PRIVATE}, "m_1"],
          ["status", {"status": "active", "note": "Mic only", **_PRIVATE}, "m_1"],
          ["status", {"status": "paused"}, "m_1"], ["status", {"status": "active"}, "m_1"],
          ["status", {"status": "active", "title": " Standup "}, "m_2"],
          ["status", {"status": "paused", "title": 5}, "m_3"],
          ["status", {"status": "Active"}, "m_4"], ["status", {"status": ["active"]}, "m_5"],
          ["status", {"status": "active", "title": " Café 東京 "}, "m_6"]]],
        [[["status", {"status": "paused"}, "m_1"], ["status", {"status": "active"}, "m_1"]]],
        # Ended carries the status, "ended" without one.
        [[["ended", {"meeting_id": "m_1", "canceled": False, "status": "needs_recovery",
                     "unfinished_chunks": 2, "capture_error": "device lost", **_PRIVATE}, ""],
          ["ended", {"status": ""}, "m_2"], ["ended", {"status": 7}, "m_3"],
          ["ended", {}, "m_4"], ["ended", {"meeting_id": 12, "status": "ended"}, ""],
          ["ended", {"meeting_id": "m_6", "status": "ended"}, "m_7"]]],
        # Committed segments, blank and malformed rows dropped.
        [[["segments", {"items": [
            _ROW, {"id": "sg_2", "text": "   ", "start_s": 4, "end_s": 5},
            {"id": "sg_3", "text": "No speaker yet.", "start_s": 6, "end_s": 7.25,
             "speaker_participant_id": None},
            "not a row",
            {"segment_id": "sg_4", "speaker": "Ana", "text": "Hi.", "start_s": True,
             "end_s": "9"},
            {"id": 17, "text": 42, "speaker": 3, "speaker_participant_id": 9, "start_s": 2.0005,
             "end_s": 1e20},
            {"id": "", "segment_id": "sg_6", "text": "x", "speaker": "",
             "speaker_participant_id": "p_2", "start_s": -0.0004},
            {"text": "été à 東京"}], **_PRIVATE}, "m_1"]]],
        # A re-decode's replacement transcript, and batches with nothing to say.
        [[["segments", {"items": [{"id": "sg_9", "text": "Re-decoded."}],
                        "removed_ids": ["sg_1"]}, "m_1"],
          ["segments", {"items": [], "removed_ids": None}, "m_1"],
          ["segments", {"items": []}, "m_1"],
          ["segments", {"items": [{"id": "sg_1", "text": ""}]}, "m_1"],
          ["segments", {"items": 5}, "m_1"], ["segments", {"items": {"id": "x"}}, "m_1"]]],
        # Voice commands by name.
        [[["voice_command", {"command": "recap", "applied": 1, "rejected": 0}, "m_1"],
          ["voice_command", {"command": ""}, "m_1"], ["voice_command", {"command": "  "}, "m_1"],
          ["voice_command", {"command": " next topic "}, "m_1"],
          ["voice_command", {"command": 3}, "m_1"]]],
        # Other engine events stay private.
        [[[kind, {"message": "guest link http://x/?t=secret", "online": True,
                  "guest_url": "http://x/?t=secret", "phase": "saved"}, "m_1"]
          for kind in ("server_started", "error", "intelligence", "background_ready",
                       "asr_released", "voice_feedback", "retry_status", "unknown")]],
        # Nothing private reaches any event.
        [[["server_started", {"guest_url": "http://x/?t=guest-secret-token"}, "m"],
          ["status", {"status": "active", "capture": {"message": "guest-secret-token"},
                      "finalization": {"message": "guest-secret-token"},
                      "speech": {"x": "guest-secret-token"}, "_retry_generation": 1}, "m"],
          ["segments", {"items": [{"id": "sg", "text": "hello",
                                   "original_text": "guest-secret-token",
                                   "channel": "guest-secret-token",
                                   "_private": "guest-secret-token"}]}, "m"],
          ["ended", {"status": "ended", "error": "guest-secret-token",
                     "capture_error": "guest-secret-token"}, "m"]]],
        # Payloads that are not mappings, and events without a meeting.
        [[[kind, payload, "m_1"] for payload in (None, [], "status", {"items": 5})
          for kind in ("status", "segments", "ended", "voice_command")]],
        [[["status", {"status": "active"}, ""], ["ended", {"status": "ended"}, ""],
          ["status", {"status": "active", "meeting_id": ""}, ""],
          ["status", {"status": "active", "meeting_id": 0}, ""],
          ["status", {"status": "active", "meeting_id": "m_9"}, ""]]],
        # The meeting runtime's seam: one meeting's events in turn.
        [[["status", {"status": "active"}, "m_7"],
          ["segments", {"items": [{"id": "sg_1", "meeting_id": "m_7", "text": "Hi.",
                                   "start_s": 0.0, "end_s": 1.0}]}, "m_7"],
          ["ended", {"meeting_id": "m_7", "status": "ended"}, "m_7"]]],
        # Only the last 32 meetings are remembered as announced.
        [[["status", {"status": "active"}, f"m_{n}"] for n in range(33)]
         + [["status", {"status": "active"}, "m_1"], ["status", {"status": "active"}, "m_0"],
            ["status", {"status": "active"}, "m_2"]]],
    ],
}
