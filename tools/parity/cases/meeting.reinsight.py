"""Curated inputs for meeting/reinsight.py's StoreToolHost (M8.5 custom-reports).

Every case builds a StoreToolHost over the store of an ended meeting "m_rerun" (no repository
behind the store) or over no store (get_store returns None), with writes allowed or revoked:

    apply_agent_ops(has_store, writes_allowed, ops)
        {"results": [{"ok", "reason", "op" (refusals only)}], "applied"}
    question(has_store, writes_allowed, tool, args)
        ask_question(*args) or resolve_question(*args): {"ok", "reason", "op" (refusals only)}
        and "applied"
    segment_exists(has_store, known, segment_id, fails)
        over a stub repository answering segment_exists(meeting_id, id) from `known` (or
        raising RuntimeError when `fails`); {"exists", "asked": [meeting_id, segment_id] | null}
    search_past_meetings(has_store, enabled, rows, query, meeting_id, limit)
        recall switched by `enabled`; the stub repository answers search_transcripts with
        `rows` (semantic search is off in the scratch settings)
    search_context_files(enabled, query, relative_path, limit)
        the knowledge folder switched by `enabled`, with no folder chosen

The C++ replay is tests/meeting_intel/test_store_tool_host.cpp.
"""

from unittest.mock import patch

MEETING_ID = "m_rerun"


class _Repo:
    def __init__(self, known=(), fails=False, rows=()):
        self.known = set(known)
        self.fails = fails
        self.rows = list(rows)
        self.asked = None

    def segment_exists(self, meeting_id, segment_id):
        self.asked = [meeting_id, segment_id]
        if self.fails:
            raise RuntimeError("database is locked")
        return segment_id in self.known

    def search_transcripts(self, query, *, exclude_meeting_id=None, limit=200):
        return list(self.rows)


def _store():
    from meeting.state.schema import FinalizationState, MeetingState
    from meeting.state.store import MeetingStateStore

    state = MeetingState(meeting_id=MEETING_ID, title="Re-run", status="ended",
                         cloud_enabled=True)
    state.finalization = FinalizationState.coerce(
        {"status": "completed"}, cloud_enabled=True, meeting_status="ended")
    return MeetingStateStore(state)


def _host(module, has_store, writes_allowed=True, repository=None):
    store = _store() if has_store else None
    return module.StoreToolHost(repository=repository if repository is not None else _Repo(),
                                get_store=lambda: store,
                                writes_allowed=lambda: writes_allowed)


def _result(result):
    out = {"ok": result.ok, "reason": result.reason}
    if not result.ok and result.reason in ("inactive", "agent_writes_revoked"):
        out["op"] = result.op
    return out


def _apply_agent_ops(module, has_store, writes_allowed, ops):
    host = _host(module, has_store, writes_allowed)
    results = host.apply_agent_ops(ops)
    return {"results": [_result(r) for r in results], "applied": host.applied}


def _question(module, has_store, writes_allowed, tool, args):
    host = _host(module, has_store, writes_allowed)
    out = _result(getattr(host, tool)(*args))
    out["applied"] = host.applied
    return out


def _segment_exists(module, has_store, known, segment_id, fails):
    repo = _Repo(known=known, fails=fails)
    host = _host(module, has_store, repository=repo)
    return {"exists": host.segment_exists(segment_id), "asked": repo.asked}


def _search_past_meetings(module, has_store, enabled, rows, query, meeting_id, limit):
    host = _host(module, has_store, repository=_Repo(rows=rows))
    with patch("services.settings.resolve_meeting_past_recall_enabled", return_value=enabled):
        return host.search_past_meetings(query=query, meeting_id=meeting_id, limit=limit)


def _search_context_files(module, enabled, query, relative_path, limit):
    host = _host(module, True)
    with patch("services.settings.resolve_meeting_context_folder_enabled", return_value=enabled):
        return host.search_context_files(query=query, relative_path=relative_path, limit=limit)


def _hit(meeting_id, text, start_s=12.0):
    return {"segment_id": "sg_abc123def456", "meeting_id": meeting_id, "text": text,
            "snippet": text, "title": "Q1 review", "started_at": "2026-03-01T10:00:00",
            "start_s": start_s}


ROWS = [_hit(MEETING_ID, "this meeting's budget"), _hit("m_past", "we adopted the budget"),
        _hit("m_other", "budget again", 30.5)]

TOPIC = {"op": "set_topic", "text": "Beta launch"}
UNKNOWN = {"op": "no_such_op"}
FORBIDDEN = {"op": "reassign_segment_speaker", "segment_id": "sg_1", "participant_id": "p_1"}

CASES = {
    "apply_agent_ops": [
        [False, True, [TOPIC, UNKNOWN, "bare", 7]],
        [True, False, [TOPIC, ["listed"], None]],
        [False, False, [TOPIC]],
        [True, True, []],
        [True, True, [UNKNOWN, FORBIDDEN]],
    ],
    "question": [
        [False, True, "ask_question", ["What is the budget?", ["sg_1"]]],
        [True, False, "ask_question", ["What is the budget?", None]],
        [True, False, "ask_question", ["Split", "ab"]],
        [False, True, "resolve_question", ["q_1", "Twelve", 0.75, ["sg_2"]]],
        [True, False, "resolve_question", ["q_1", "Twelve", 1, []]],
        [True, True, "ask_question", ["What is the budget?", ["sg_1"]]],
        [True, True, "ask_question", ["What is the budget?", []]],
        [True, True, "resolve_question", ["q_missing", "Twelve", 0.5, ["sg_1"]]],
    ],
    "segment_exists": [
        [True, ["sg_1"], "sg_1", False],
        [True, ["sg_1"], "sg_missing", False],
        [False, ["sg_1"], "sg_1", False],
        [True, ["sg_1"], "sg_1", True],
    ],
    "search_past_meetings": [
        [True, False, ROWS, "budget", None, 10],
        [True, True, ROWS, "budget", None, 10],
        [False, True, ROWS, "budget", None, 10],
        [True, True, ROWS, "   ", None, 10],
        [True, True, ROWS, "budget", None, "1"],
        [True, True, ROWS, "", MEETING_ID, 10],
    ],
    "search_context_files": [
        [False, "budget", None, 10],
        [True, "budget", None, 10],
        [True, "budget", "notes.md", 10],
    ],
}

WRAPPERS = {
    "apply_agent_ops": _apply_agent_ops,
    "question": _question,
    "segment_exists": _segment_exists,
    "search_past_meetings": _search_past_meetings,
    "search_context_files": _search_context_files,
}

PINS = {
    "store": "an ended meeting m_rerun with finalization completed, no repository behind the "
             "store",
    "settings": "the consent switches are patched per case; semantic search and the knowledge "
                "folder path stay at the scratch settings' defaults",
}
