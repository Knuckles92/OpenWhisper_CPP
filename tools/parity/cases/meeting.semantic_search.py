"""Curated inputs for meeting/semantic_search.py (M8.3 recall-and-knowledge-folder).

    constants()                  the module's messages, scope texts, question and limits
    search_history(setup)        over a stub repository and a scripted judge. `setup`:
        query, semantic, exclude, limit   search_history's arguments
        feature                           what resolve_typesafe_feature_enabled answers, or a
                                          list of answers, one per call, the last repeated
        route                             "typesafe" or "openrouter": route_from_settings
        judge                             null (no key), or {"is_local", "answers": [one per
                                          batch: {key: score} or null]}
        keyword, candidates               rows of search_transcripts and search_candidates
        cloud                             {meeting id: cloud_enabled} for get_meeting
      The answer is {result, asks, candidate_calls, meeting_reads}: what the judge was asked
      (state and questions), with which arguments the candidates were read, and which meetings
      were read for their consent.

The C++ replay is tests/meeting_intel/test_golden.cpp.
"""

import itertools


def _constants(module):
    return {
        "MAX_CANDIDATES": module.MAX_CANDIDATES,
        "SEARCH_BUDGET_S": module.SEARCH_BUDGET_S,
        "SCOPE": module.SCOPE,
        "LOCAL_SCOPE": module.LOCAL_SCOPE,
        "BATCH_SIZE": module.BATCH_SIZE,
        "RELEVANT_SCORE": module.RELEVANT_SCORE,
        "DISABLED_MESSAGE": module.DISABLED_MESSAGE,
        "UNAVAILABLE_MESSAGE": module.UNAVAILABLE_MESSAGE,
        "RELEVANCE_QUESTION": module.RELEVANCE_QUESTION,
    }


class _Repo:
    def __init__(self, setup, record):
        self.setup = setup
        self.record = record

    def search_transcripts(self, query, *, exclude_meeting_id=None, limit=200):
        return [dict(row) for row in self.setup["keyword"]]

    def search_candidates(self, query, *, exclude_meeting_id=None, limit=48, shared_only=True):
        self.record["candidate_calls"].append({"query": query, "exclude": exclude_meeting_id,
                                               "limit": limit, "shared_only": shared_only})
        return [dict(row) for row in self.setup["candidates"]]

    def get_meeting(self, meeting_id):
        self.record["meeting_reads"].append(meeting_id)
        cloud = self.setup["cloud"]
        if meeting_id not in cloud:
            return None
        return {"id": meeting_id, "cloud_enabled": cloud[meeting_id]}


class _Judge:
    def __init__(self, spec, record):
        self.is_local = spec["is_local"]
        self.answers = list(spec["answers"])
        self.record = record

    def ask(self, state, questions, timeout_s):
        self.record["asks"].append({"state": state, "questions": questions})
        if not self.answers:
            return None
        scores = self.answers.pop(0)
        if scores is None:
            return None
        return {key: {"type": "noul", "noul": value} for key, value in scores.items()}


def _search_history(module, setup):
    from services import typesafe

    record = {"asks": [], "candidate_calls": [], "meeting_reads": []}
    feature = setup["feature"]
    answers = feature if isinstance(feature, list) else [feature]
    feeds = itertools.chain(answers, itertools.repeat(answers[-1]))
    route = typesafe.OPENROUTER_ROUTE if setup["route"] == "openrouter" else typesafe.TYPESAFE_ROUTE
    judge = None if setup["judge"] is None else _Judge(setup["judge"], record)
    saved = (module.resolve_typesafe_feature_enabled, module.judge_from_settings,
             module.route_from_settings)
    module.resolve_typesafe_feature_enabled = lambda _name: next(feeds)
    module.judge_from_settings = lambda: None
    module.route_from_settings = lambda: route
    try:
        result = module.search_history(_Repo(setup, record), setup["query"],
                                       semantic=setup["semantic"],
                                       exclude_meeting_id=setup["exclude"],
                                       limit=setup["limit"], judge=judge)
    finally:
        (module.resolve_typesafe_feature_enabled, module.judge_from_settings,
         module.route_from_settings) = saved
    return {"result": result, **record}


def _row(segment_id, meeting_id, text, title="Launch", **extra):
    row = {"segment_id": segment_id, "meeting_id": meeting_id, "text": text, "title": title,
           "start_s": 10.0, "started_at": "2026-09-18T10:00:00"}
    row.update(extra)
    return row


KEYWORD = [{"segment_id": "sg_k", "meeting_id": "m_a", "text": "keyword", "snippet": "[keyword]"}]
TWO = [_row("sg_a", "m_a", "Launch postponed"), _row("sg_b", "m_b", "Coffee break")]
TWENTY = [_row(f"sg_{i:02d}", f"m_{i % 3}", f"Passage {i} " + "word " * 300,
               title="T" * 150 if i == 0 else f"Meeting {i}") for i in range(20)]


def _setup(**changes):
    setup = {"query": "shipping delay", "semantic": True, "exclude": None, "limit": 50,
             "feature": True, "route": "typesafe",
             "judge": {"is_local": False, "answers": [{"p0": 0.96, "p1": 0.08}]},
             "keyword": KEYWORD, "candidates": TWO, "cloud": {"m_a": True, "m_b": True}}
    setup.update(changes)
    return setup


CASES = {
    "constants": [[]],
    "search_history": [[s] for s in (
        _setup(),
        _setup(semantic=False),
        _setup(query="   "),
        _setup(query="  " + "q" * 600 + "  "),
        _setup(limit=0),
        _setup(limit=1, judge={"is_local": True, "answers": [{"p0": 0.6, "p1": 0.9}]}),
        _setup(feature=False),
        _setup(judge=None),
        _setup(judge=None, route="openrouter"),
        _setup(judge={"is_local": False, "answers": [None]}),
        _setup(judge={"is_local": False, "answers": [{"p0": 0.5, "p1": 0.4999}]}),
        _setup(judge={"is_local": False, "answers": [{"p0": 0.9}]}),
        _setup(judge={"is_local": True, "answers": [{"p0": 0.9, "p1": 0.9}]},
               cloud={"m_a": False, "m_b": False}),
        _setup(cloud={"m_a": False, "m_b": True},
               judge={"is_local": False, "answers": [{"p0": 0.9}]}),
        _setup(cloud={}, judge={"is_local": False, "answers": []}),
        _setup(candidates=[], judge={"is_local": False, "answers": []}),
        _setup(candidates=TWENTY, cloud={"m_0": True, "m_1": False, "m_2": True},
               judge={"is_local": False, "answers": [
                   {f"p{i}": 0.1 * (i + 1) for i in range(6)},
                   {f"p{i}": 0.95 - 0.1 * i for i in range(6)},
                   {f"p{i}": 0.7 for i in range(3)}]}),
        _setup(candidates=TWENTY, judge={"is_local": True, "answers": [
            {f"p{i}": 0.9 for i in range(8)}, {f"p{i}": 0.9 for i in range(8)}, None]}),
        _setup(candidates=TWENTY, feature=[True, True, False],
               judge={"is_local": True, "answers": [{f"p{i}": 0.9 for i in range(8)}] * 3}),
        _setup(exclude="m_b", limit=7),
        _setup(candidates=[_row("sg_n", "m_a", "No title", title=None)]),
        _setup(candidates=[{"segment_id": "sg_m", "meeting_id": "m_a", "text": "Bare row"}],
               judge={"is_local": False, "answers": [{"p0": 0.8}]}),
    )],
}

WRAPPERS = {
    "constants": _constants,
    "search_history": _search_history,
}

PINS = {
    "judge": "a scripted judge: one answer per batch, in order; the timeout it is given is "
             "not recorded (it is what is left of the 20 s budget)",
}
