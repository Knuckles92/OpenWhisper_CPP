"""Curated inputs for meeting/agent/scheduler.py (M8.4 checkpoint-scheduler).

    _content_words(text)                    sorted(the set)
    jaccard_shift(older, newer)             _detect_topic_shift() of a scheduler whose engine
                                            holds the `older` texts in the window before
                                            now - 60 s and the `newer` ones after it (now 200 s,
                                            no semantic judge, no spacing)
    _interval_for(pending, intervals)       on CheckpointScheduler(engine, core, **intervals)
    ConsolidationOutcome(status, message)   .to_dict()
    _consolidation_outcome(result, timeout) .to_dict(); result null (timed out), ["ok", n]
                                            (n applied ops) or ["fail", error]
    trace(spec)                             one scripted run of the scheduler (below)

A trace drives a CheckpointScheduler without its worker: a fake engine (with the durable
delivery ledger when spec["ledger"]), a fake monotonic clock starting at 1000 s, a meeting clock
at 200 s, and a scripted agent core. spec["intervals"] are the constructor's intervals; each
step of spec["steps"] is
    ["add", [[id, start_s, text], ...]]   rows committed (end_s = start_s + 2)
    ["notify", n] / ["guidance"]          notify_segments(n) / notify_guidance()
    ["advance", s] / ["clock", s]         monotonic seconds / the meeting clock
    ["script", [result, ...]]             the next passes' results, in order: "ok" (one
                                          set_topic applied), "revise" (one
                                          revise_segment_text applied), "fail:<error>"; "ok"
                                          when none is left
    ["healthy", bool] / ["no_store"]      the core's health / the engine loses its store
    ["run_due"] / ["seed"]                one tick / seed_sent_segments(get_transcript())
    ["consolidate", timeout_s]            run_consolidation; records ["outcome", status, message]
    ["pending"] / ["online"]              records the pending notifications / the online state
and the trace records, in order, every pass ["pass", kind, segment ids, recent context ids or
null, notes_review_requested, note_adjustment_request or null], every cursor acknowledgement
["ack", consumer, seq], every health call ["health", online, error] and every write
revocation ["revoke"]. Pass request ids are random and left out.
"""

from meeting.interfaces import AgentResult, OpResult


class _Clock:
    def __init__(self, now_s):
        self.value = now_s

    def now_s(self):
        return self.value


class _Mono:
    def __init__(self, value):
        self.value = value

    def __call__(self):
        return self.value


class _Store:
    def snapshot(self):
        return {"meeting_id": "m_test", "cards": {}}


class _Engine:
    def __init__(self, trace):
        self.trace = trace
        self.store = _Store()
        self.clock = _Clock(200.0)
        self.rows = []

    def add(self, row):
        self.rows.append(row)

    def get_transcript(self, after_start_s=-1.0, limit=None):
        return [
            {k: v for k, v in row.items() if k != "delivery_seq"}
            for row in self.rows if float(row.get("start_s") or 0.0) > float(after_start_s)
        ]

    def revoke_agent_writes(self):
        self.trace.append(["revoke"])


class _LedgerEngine(_Engine):
    def __init__(self, trace):
        super().__init__(trace)
        self.seq = 0
        self.cursors = {}

    def add(self, row):
        self.seq += 1
        super().add(dict(row, delivery_seq=self.seq))

    def get_transcript_delivery_page(self, after_seq=0, limit=300):
        return [dict(row) for row in self.rows if row["delivery_seq"] > after_seq][:limit]

    def get_transcript_delivery_cursor(self, consumer):
        return self.cursors.get(consumer, 0)

    def advance_transcript_delivery_cursor(self, consumer, seq):
        self.trace.append(["ack", consumer, seq])
        self.cursors[consumer] = max(self.cursors.get(consumer, 0), seq)

    def get_transcript_delivery_high_water(self):
        return self.seq


class _Agent:
    def __init__(self, trace):
        self.trace = trace
        self.results = []
        self.healthy = True

    def _run(self, payload):
        if payload.is_consolidation:
            kind = "consolidation"
        elif payload.is_polish:
            kind = "polish"
        elif payload.is_notes:
            kind = "notes"
        else:
            kind = "cards"
        snap = payload.state_snapshot
        context = snap.get("recent_transcript_context")
        self.trace.append([
            "pass", kind, [s.get("id") for s in payload.new_segments],
            None if context is None else [s.get("id") for s in context],
            bool(snap.get("notes_review_requested")), snap.get("note_adjustment_request"),
        ])
        result = self.results.pop(0) if self.results else "ok"
        if result == "ok":
            return AgentResult(ok=True, op_results=[OpResult(ok=True, op={"op": "set_topic"})])
        if result == "revise":
            return AgentResult(ok=True, op_results=[
                OpResult(ok=True, op={"op": "revise_segment_text"})])
        return AgentResult(ok=False, error=result[len("fail:"):])

    def checkpoint(self, payload):
        return self._run(payload)

    def consolidate(self, payload):
        return self._run(payload)

    def cancel(self):
        self.trace.append(["cancel"])

    def is_healthy(self):
        return self.healthy


def _trace(module, spec):
    trace = []
    engine = _LedgerEngine(trace) if spec.get("ledger") else _Engine(trace)
    agent = _Agent(trace)
    mono = _Mono(1000.0)
    sched = module.CheckpointScheduler(
        engine, agent, monotonic=mono,
        on_health=lambda online, error: trace.append(["health", online, error]),
        **spec.get("intervals", {}),
    )
    for step in spec["steps"]:
        op = step[0]
        if op == "add":
            for seg_id, start_s, text in step[1]:
                engine.add({"id": seg_id, "start_s": start_s, "end_s": start_s + 2.0,
                            "text": text})
        elif op == "notify":
            sched.notify_segments(step[1])
        elif op == "guidance":
            sched.notify_guidance()
        elif op == "advance":
            mono.value += step[1]
        elif op == "clock":
            engine.clock.value = step[1]
        elif op == "script":
            agent.results.extend(step[1])
        elif op == "healthy":
            agent.healthy = step[1]
        elif op == "no_store":
            engine.store = None
        elif op == "run_due":
            sched.run_due()
        elif op == "seed":
            sched.seed_sent_segments(engine.get_transcript())
        elif op == "consolidate":
            outcome = sched.run_consolidation(timeout_s=step[1])
            trace.append(["outcome", outcome.status, outcome.message])
        elif op == "pending":
            trace.append(["pending", sched._pending_segments])
        elif op == "online":
            trace.append(["online", sched._online])
        else:
            raise ValueError(op)
    return trace


class _ShiftEngine:
    def __init__(self, older, newer):
        self.clock = _Clock(200.0)
        self.store = None
        self.rows = (
            [{"start_s": 85.0 + i, "text": text} for i, text in enumerate(older)]
            + [{"start_s": 145.0 + i, "text": text} for i, text in enumerate(newer)]
        )

    def get_transcript(self, after_start_s=-1.0, limit=None):
        return [row for row in self.rows if row["start_s"] > after_start_s]


def _jaccard_shift(module, older, newer):
    saved = module._SHIFT_CHECK_SPACING_S
    module._SHIFT_CHECK_SPACING_S = 0.0
    try:
        sched = module.CheckpointScheduler(_ShiftEngine(older, newer), object())
        return sched._detect_topic_shift()
    finally:
        module._SHIFT_CHECK_SPACING_S = saved


def _consolidation_outcome(module, result, timeout_s):
    if result is None:
        value = None
    elif result[0] == "ok":
        value = AgentResult(ok=True, op_results=[
            OpResult(ok=True, op={"op": "set_topic"}) for _ in range(result[1])])
    else:
        value = AgentResult(ok=False, error=result[1])
    return module._consolidation_outcome(value, timeout_s).to_dict()


_FINANCE = ["budget forecast revenue pipeline margin earnings",
            "quarterly finance spreadsheet ledger accounting"]
_HIRING = ["hiring onboarding interview candidates staffing",
           "recruitment culture retention engineers designers"]


def _rows(prefix, count, start=0.0, step=1.0, text="hello there"):
    return [[f"{prefix}{i}", start + i * step, f"{text} {i}"] for i in range(count)]


_FAST = {"base_interval_s": 0.05, "min_interval_s": 0.05, "max_interval_s": 0.05}
_SHIFT = {"base_interval_s": 45.0, "min_interval_s": 0.05, "max_interval_s": 60.0}

_TRACES = [
    # The warm-up keeps everything, then one fire carries all of it; notes follow the first
    # checkpoint, polish 15 s after it.
    {"steps": [
        ["add", _rows("sg_", 12)], ["clock", 119.9], ["notify", 12], ["run_due"], ["pending"],
        ["clock", 120.0], ["run_due"], ["pending"], ["advance", 5], ["run_due"],
        ["advance", 10], ["run_due"], ["run_due"],
    ]},
    # Guidance during the warm-up waits too, then fires over the whole meeting.
    {"steps": [
        ["add", _rows("sg_", 3)], ["clock", 60.0], ["guidance"], ["run_due"], ["clock", 130.0],
        ["run_due"], ["pending"],
    ]},
    # Failures back off 30, 60 and 120 s; the third goes offline, a success comes back online.
    {"steps": [
        ["add", [["sg_1", 1.0, "one"]]], ["script", ["fail:boom", "fail:boom", "fail:later"]],
        ["notify", 1], ["run_due"], ["run_due"], ["advance", 29], ["run_due"], ["advance", 1],
        ["run_due"], ["advance", 60], ["run_due"], ["online"], ["pending"], ["advance", 119],
        ["run_due"], ["advance", 1], ["run_due"], ["online"], ["pending"],
    ]},
    # New speech: the interval follows the pressure; sent rows are never resent, earlier lines
    # travel as context.
    {"intervals": {}, "steps": [
        ["add", _rows("a_", 2)], ["notify", 2], ["run_due"],
        ["add", [["b_0", 10.0, "late one"]]], ["notify", 1], ["advance", 10], ["run_due"],
        ["advance", 10], ["run_due"],
        ["add", _rows("c_", 9, start=20.0)], ["notify", 9], ["advance", 4], ["run_due"],
        ["advance", 1], ["run_due"],
        ["add", _rows("d_", 4, start=40.0)], ["notify", 4], ["advance", 14], ["run_due"],
        ["advance", 1], ["run_due"],
    ]},
    # Out-of-order arrival without a ledger: the time window re-reads what arrived late.
    {"intervals": _FAST, "steps": [
        ["add", [["mic_1", 0.0, "mic one"]]], ["notify", 1], ["run_due"],
        ["add", [["loop_1", 0.0, "loop one"], ["loop_2", 8.0, "loop two"]]], ["notify", 2],
        ["advance", 1], ["run_due"],
        ["add", [["late_1", 400.0, "far"]]], ["notify", 1], ["advance", 1], ["run_due"],
        ["add", [["early_1", 5.0, "too old"]]], ["notify", 1], ["advance", 1], ["run_due"],
    ]},
    # The ledger pages 300 rows in commit order, acknowledges each page, and continues.
    {"ledger": True, "steps": [
        ["add", _rows("sg_", 305)], ["notify", 1], ["run_due"], ["pending"], ["run_due"],
        ["advance", 20], ["run_due"], ["advance", 45], ["run_due"], ["advance", 45],
        ["run_due"],
    ]},
    # A late segment that starts early is delivered in commit order and shown in timeline order.
    {"ledger": True, "steps": [
        ["add", [["newer", 600.0, "newer"]]], ["notify", 1], ["run_due"],
        ["add", [["late", 1.0, "late"], ["mid", 300.0, "mid"]]], ["notify", 2],
        ["advance", 20], ["run_due"], ["advance", 45], ["run_due"],
    ]},
    # A failed pass on the ledger leaves the cursor; the retry gets the same page.
    {"ledger": True, "steps": [
        ["add", _rows("sg_", 3)], ["script", ["fail:busy"]], ["notify", 3], ["run_due"],
        ["advance", 30], ["run_due"], ["advance", 45], ["run_due"],
    ]},
    # A polish that revises text asks for a guidance pass and a notes review.
    {"steps": [
        ["add", _rows("sg_", 3)], ["notify", 3], ["run_due"], ["script", ["revise"]],
        ["advance", 15], ["run_due"], ["pending"], ["run_due"], ["run_due"],
    ]},
    # A failed polish is retried at the next cadence, not at once.
    {"steps": [
        ["add", _rows("sg_", 2)], ["notify", 2], ["run_due"], ["script", ["fail:polish"]],
        ["advance", 15], ["run_due"], ["run_due"], ["advance", 45], ["run_due"],
    ]},
    # Failed notes wait their interval; guidance clears the wait.
    {"steps": [
        ["add", _rows("sg_", 2)], ["script", ["ok", "fail:notes"]], ["notify", 2], ["run_due"],
        ["advance", 20], ["run_due"], ["guidance"], ["run_due"], ["advance", 45], ["run_due"],
    ]},
    # A topic shift fires early; similar windows do not.
    {"intervals": _SHIFT, "steps": [
        ["add", [["f_0", 85.0, _FINANCE[0]], ["f_1", 100.0, _FINANCE[1]]]], ["notify", 2],
        ["run_due"],
        ["add", [["h_0", 145.0, _HIRING[0]], ["h_1", 165.0, _HIRING[1]]]], ["notify", 2],
        ["advance", 5], ["run_due"],
        ["add", [["g_0", 170.0, _FINANCE[0]], ["g_1", 175.0, _FINANCE[1]]]], ["notify", 2],
        ["advance", 5], ["run_due"], ["advance", 10], ["run_due"], ["advance", 50],
        ["run_due"],
    ]},
    # Seeding marks the meeting so far as delivered, on the ledger too.
    {"steps": [
        ["add", _rows("sg_", 3)], ["seed"], ["add", [["sg_new", 10.0, "new"]]], ["notify", 1],
        ["run_due"], ["advance", 20], ["run_due"],
    ]},
    {"ledger": True, "steps": [
        ["add", _rows("sg_", 3)], ["seed"], ["add", [["sg_new", 10.0, "new"]]], ["notify", 1],
        ["run_due"], ["advance", 20], ["run_due"],
    ]},
    # No store: the claimed work and the guidance are put back.
    {"steps": [
        ["add", _rows("sg_", 2)], ["no_store"], ["guidance"], ["notify", 2], ["run_due"],
        ["pending"], ["consolidate", 5.0],
    ]},
    # An unhealthy core skips notes and polish and the final pass.
    {"steps": [
        ["add", _rows("sg_", 2)], ["notify", 2], ["run_due"], ["healthy", False],
        ["advance", 60], ["run_due"], ["consolidate", 5.0], ["run_due"],
    ]},
    # The final pass, its failure, and nothing after it.
    {"steps": [
        ["add", _rows("sg_", 2)], ["consolidate", 5.0], ["notify", 1], ["run_due"],
    ]},
    {"steps": [
        ["add", _rows("sg_", 2)], ["script", ["fail:model down"]], ["consolidate", 7.9],
    ]},
]

CASES = {
    "_content_words": [
        ["Yeah okay we should discuss the quarterly budget review process"],
        ["budget forecast revenue pipeline margin earnings"],
        [""],
        ["It's the team's don't-stop ROADMAP, roadmap; ROADMAPS"],
        ["café naïve résumé über Straße KELVIN K"],
        ["THINGS things Something somethings actually Actually basically"],
        ["a ab abc abcd abcde ''''  'tis o'clock"],
        ["numbers 1234 and words2words mixed"],
    ],
    "jaccard_shift": [
        [_FINANCE, _HIRING],
        [_FINANCE, _FINANCE],
        [["okay"], ["sure"]],
        [_FINANCE + _HIRING, _HIRING],
        [_FINANCE, [_FINANCE[0], _HIRING[1]]],
        [_FINANCE + ["budget planning offsite"], _HIRING + ["budget planning offsite"]],
        [[], _HIRING],
    ],
    "_interval_for": [
        [pending, intervals]
        for intervals in ({}, {"base_interval_s": 45.0, "min_interval_s": 30.0,
                               "max_interval_s": 60.0})
        for pending in (0, 1, 2, 3, 7, 8, 20)
    ],
    "ConsolidationOutcome": [
        ["completed", "Final insights are ready."],
        ["unavailable", "AI insights are offline; final insights could not run."],
        ["failed", "Final insights timed out after 900s."],
    ],
    "_consolidation_outcome": [
        [None, 900.0], [None, 0.05], [None, 7.9], [["ok", 0], 900.0], [["ok", 3], 5.0],
        [["fail", "model down"], 900.0], [["fail", ""], 900.0],
    ],
    "trace": [[spec] for spec in _TRACES],
}

WRAPPERS = {
    "_content_words": lambda module, text: sorted(module._content_words(text)),
    "jaccard_shift": _jaccard_shift,
    "_interval_for": lambda module, pending, intervals: module.CheckpointScheduler(
        _Engine([]), object(), **intervals)._interval_for(pending),
    "ConsolidationOutcome": lambda module, status, message: module.ConsolidationOutcome(
        status, message).to_dict(),
    "_consolidation_outcome": _consolidation_outcome,
    "trace": _trace,
}

TRACE = {
    "tests": ["tests/test_meeting_scheduler.py", "tests/test_scheduler_semantic_topic_shift.py"],
    "functions": ["_content_words"],
}
