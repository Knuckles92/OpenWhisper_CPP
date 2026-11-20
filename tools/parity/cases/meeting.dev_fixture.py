"""Curated inputs for meeting/dev_fixture.py (M7.17 meeting-engine).

    constants()              {"DEMO_TITLE", "DEMO_NOTE"}
    turns()                  _TURNS as [speaker, start_s, end_s, text] lists
    speakers()               _SPEAKERS as [key, name, kind] lists
    seed_demo_meeting(title)
        seed_demo_meeting over a store with no repository whose host participant "Me" was
        applied first (as the engine does), a recording repository for the segments, and no
        clock. The result is {"result": the function's dict, "segments": the stored segments as
        [segment_id, channel, start_s, end_s, text, speaker_participant_id, speaker_source],
        "state": the store's snapshot}.

Ids and time are pinned as in meeting.state.patches.py: new_id counts "<prefix>_000000000001",
"..._000000000002" (one counter per call, testing/fixed_ids.h), and now is 2026-01-01T00:00:00Z
(FakeClock's default), through meeting.state.schema.uuid and meeting.time_utils.datetime.
"""

from datetime import datetime, timezone

NOW_UTC = datetime(2026, 1, 1, tzinfo=timezone.utc)


class _Pinned(datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW_UTC if tz is not None else NOW_UTC.replace(tzinfo=None)


class _Uuid:
    def __init__(self, n):
        self.hex = f"{n:012x}" + "0" * 20


class _FixedUuid:
    def __init__(self):
        self.next = 1

    def uuid4(self):
        value = _Uuid(self.next)
        self.next += 1
        return value


class _Segments:
    def __init__(self):
        self.segments = []

    def add_segments(self, segments):
        self.segments.extend(segments)

    def segment_exists(self, segment_id):
        return any(segment.segment_id == segment_id for segment in self.segments)


def _constants(module):
    return {"DEMO_TITLE": module.DEMO_TITLE, "DEMO_NOTE": module.DEMO_NOTE}


def _turns(module):
    return [list(turn) for turn in module._TURNS]


def _speakers(module):
    return [list(speaker) for speaker in module._SPEAKERS]


def _seed(module, title):
    import meeting.state.schema as schema
    import meeting.time_utils as time_utils
    from meeting.state.store import MeetingStateStore

    saved_uuid, saved_datetime = schema.uuid, time_utils.datetime
    schema.uuid, time_utils.datetime = _FixedUuid(), _Pinned
    try:
        repository = _Segments()
        store = MeetingStateStore(
            schema.MeetingState(meeting_id="m_demo", title=""),
            segment_exists=repository.segment_exists,
        )
        me = store.apply("system", None, [{
            "op": "upsert_participant", "display_name": "Me",
            "kind": "me", "is_provisional": False,
        }])[0]
        result = module.seed_demo_meeting(
            meeting_id="m_demo",
            me_participant_id=me.target_id,
            store=store,
            repository=repository,
            title=title,
        )
        segments = [
            [s.segment_id, s.channel, s.start_s, s.end_s, s.text, s.speaker_participant_id,
             s.speaker_source]
            for s in repository.segments
        ]
        return {"result": result, "segments": segments, "state": store.snapshot()}
    finally:
        schema.uuid, time_utils.datetime = saved_uuid, saved_datetime


CASES = {
    "constants": [[]],
    "turns": [[]],
    "speakers": [[]],
    "seed_demo_meeting": [[None], ["Quarterly sync"], ["   "]],
}

WRAPPERS = {
    "constants": _constants,
    "turns": _turns,
    "speakers": _speakers,
    "seed_demo_meeting": _seed,
}
