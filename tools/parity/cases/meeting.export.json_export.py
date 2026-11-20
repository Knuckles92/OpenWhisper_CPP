"""Curated inputs for meeting/export/json_export.py (M7.11 meeting-state-extras).

    export_json(meeting, state, segments)     the document text, byte for byte
"""

MEETING = {
    "id": "m_export", "title": "Planning Sync — Q3", "status": "ended",
    "started_at": "2026-03-15T14:30:00Z", "ended_at": "2026-03-15T15:00:00Z",
    "paused_total_s": 12.5, "cloud_enabled": False, "asr_model": "parakeet",
    "asr_remote_json": None, "agent_provider": "ollama", "agent_model": "qwen",
    "agent_endpoint_json": '{"base_url":"http://localhost:11434/v1"}',
    "host_token": "SECRET_HOST_TOKEN", "guest_token": "SECRET_GUEST_TOKEN",
    "app_pid": 12345, "app_heartbeat_at": "2026-03-15T15:00:00Z",
    "state_json": '{"seq": 9}', "state_seq": 9, "spool_dir": "C:\\meetings\\m_export",
    "origin_device_id": None, "origin_device_name": None,
}

STATE = {
    "meeting_id": "m_export", "seq": 9, "title": "Planning Sync — Q3",
    "cards": {"key_points": [{"id": "it_1", "text": "Ship béta 😀", "evidence": ["sg_1"],
                              "data": {"n": 1.0, "big": 10**15, "neg": -0.5, "e": 1e-07}}]},
    "participants": {"p_me": {"id": "p_me", "display_name": "Ålex"}},
    "questions": [], "custom_reports": [], "live_highlights": [],
    "insight_review": {}, "intent": {"text": "Line one\nLine \"two\"\t"},
}

SEGMENTS = [
    {"id": "sg_1", "meeting_id": "m_export", "start_s": 5.0, "end_s": 8.0,
     "text": "Welcome — everyone", "original_text": "Welcome everyone", "channel": "mic",
     "speaker_participant_id": "p_me", "speaker_pinned": False},
    {"id": "sg_2", "start_s": 10, "end_s": 14.25, "text": "", "channel": "loopback",
     "speaker_participant_id": None, "speaker_pinned": True},
]

CASES = {
    "export_json": [
        [MEETING, STATE, SEGMENTS],
        [{}, {}, []],
        [None, {"title": "Only state"}, []],
        [{"host_token": "x", "guest_token": "y", "app_pid": 1, "app_heartbeat_at": "z",
          "state_json": "{}"}, {}, [{}]],
        [{"nested": {"host_token": "kept inside"}, "list": [1, [2, {"a": None}]]}, {"empty": {}},
         [{"empty_list": []}]],
    ],
}
