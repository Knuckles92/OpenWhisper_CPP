"""Scenarios for meeting/end_run.py EndRun (M7.9 meeting-core).

    run(scenario) -> {"events": [...], "state": {...}}

One End as the engine drives it: plan, announce, redecode, identify_speakers, insights. Every
effect is a fake the scenario scripts:

    complete, unfinished, scheduler (bool), will_offline, will_speaker_id
    cloud_enabled, want_polish, want_report        plan's arguments
    core_healthy                                   core_is_healthy()
    redecode: {"result": bool | "raise", "progress": [[detail, current, total], ...]}
    redecode_failure                               redecode_failure()
    speakers: {"result": {...} | "raise", "on_start": bool, "progress": [...]}
    polish: absent (the scheduler has no run_final_polish) or
            {"result": {status?, message?} | "raise", "progress": [...]}
    consolidate: {"result": {status?, message?} | "raise", "progress": [detail, ...]}
    allow_raises                                   allow_agent_writes() raises
    offline_message, summary_stats

Events, in order: ["set_finalization", {status, message, stage, current_step, total_steps,
step_details, steps?, summary_stats?, emit}] with the steps copied at the call (EndRun mutates
its list afterwards), ["allow"], ["revoke"]. A raising fake raises RuntimeError(<name> failed).
"""

import copy
import types


def _outcome(result):
    return types.SimpleNamespace(**result)


def _run(module, scenario):
    events = []
    s = scenario

    def set_finalization(status, message="", *, stage="", current_step=0, total_steps=0,
                         step_details="", steps=None, summary_stats=None, emit=True):
        payload = {"status": status, "message": message, "stage": stage,
                   "current_step": current_step, "total_steps": total_steps,
                   "step_details": step_details}
        if steps is not None:
            payload["steps"] = copy.deepcopy(steps)
        if summary_stats is not None:
            payload["summary_stats"] = copy.deepcopy(summary_stats)
        payload["emit"] = emit
        events.append(["set_finalization", payload])
        return True

    def redecode(progress):
        spec = s.get("redecode", {"result": True})
        for detail, current, total in spec.get("progress", []):
            progress(detail, current, total)
        if spec["result"] == "raise":
            raise RuntimeError("redecode failed")
        return spec["result"]

    def identify_speakers(*, progress_cb, on_start):
        spec = s.get("speakers", {"result": {"ok": True, "applied": 1}})
        if spec.get("on_start"):
            on_start()
        for detail, current, total in spec.get("progress", []):
            progress_cb(detail, current, total)
        if spec["result"] == "raise":
            raise RuntimeError("speakers failed")
        return dict(spec["result"])

    def allow():
        events.append(["allow"])
        if s.get("allow_raises"):
            raise RuntimeError("allow failed")

    def revoke():
        events.append(["revoke"])

    scheduler = None
    if s.get("scheduler"):
        scheduler = types.SimpleNamespace()
        if "polish" in s:
            def run_final_polish(*, timeout_s, progress_cb):
                spec = s["polish"]
                for detail, current, total in spec.get("progress", []):
                    progress_cb(detail, current, total)
                if spec["result"] == "raise":
                    raise RuntimeError("polish failed")
                return _outcome(spec["result"])
            scheduler.run_final_polish = run_final_polish

        def run_consolidation(*, progress_cb):
            spec = s.get("consolidate", {"result": {"status": "completed"}})
            for detail in spec.get("progress", []):
                progress_cb(detail)
            if spec["result"] == "raise":
                raise RuntimeError("consolidate failed")
            return _outcome(spec["result"])
        scheduler.run_consolidation = run_consolidation

    end = module.EndRun(
        "m_test",
        complete=s.get("complete", True),
        unfinished=s.get("unfinished", 0),
        scheduler=scheduler,
        will_offline=s.get("will_offline", False),
        will_speaker_id=s.get("will_speaker_id", False),
        set_finalization=set_finalization,
        redecode=redecode,
        redecode_failure=lambda: s.get("redecode_failure", ""),
        identify_speakers=identify_speakers,
        allow_agent_writes=allow,
        revoke_agent_writes=revoke,
        core_is_healthy=lambda: s.get("core_healthy", True),
        offline_message=lambda: s.get("offline_message", "Meeting intelligence is offline."),
        summary_stats=lambda: dict(s.get("summary_stats", {"segments": 3, "words": 12})),
    )
    end.plan(cloud_enabled=s.get("cloud_enabled", False), want_polish=s.get("want_polish", True),
             want_report=s.get("want_report", True))
    end.announce()
    end.redecode()
    end.identify_speakers()
    end.insights()
    return {
        "events": events,
        "state": {
            "run_cloud": end.run_cloud, "total_steps": end.total_steps,
            "offline_ok": end.offline_ok, "speaker_ok": end.speaker_ok,
            "speaker_skipped": end.speaker_skipped, "speaker_error": end.speaker_error,
        },
    }


_STATS = {"segments": 32, "words": 410, "key_points": 4, "action_items": 0, "decisions": 1,
          "risks": 0, "questions": 2, "duration_s": 61.5}
_CLOUD = {"cloud_enabled": True, "scheduler": True, "summary_stats": _STATS}
_POLISH_OK = {"result": {"status": "completed", "message": "Cleaned 3 blocks."},
              "progress": [["Cleaning transcript formatting and grammar (block 1/2, 80 segments)...",
                            1, 2],
                           ["Cleaning transcript formatting and grammar (block 2/2, 12 segments)...",
                            2, 2]]}
_REPORT_OK = {"result": {"status": "completed", "message": "Report ready."},
              "progress": ["Reading the transcript...", "Writing the summary..."]}

_SCENARIOS = [
    # Cloud off, nothing else: disabled from the start.
    {},
    # test_cloud_off_replaces_transcript_when_redecode_enabled: disabled without steps after a
    # completed re-decode.
    {"will_offline": True,
     "redecode": {"result": True, "progress": [["window 1", 1, 3], ["window 2", 2, 3]]}},
    {"will_offline": True, "redecode": {"result": False}},
    {"will_offline": True, "redecode": {"result": False},
     "redecode_failure": "Re-transcription produced 2 words versus 90."},
    {"will_offline": True, "redecode": {"result": "raise"}},
    # Waiting for recovery.
    {"cloud_enabled": True, "complete": False, "unfinished": 3, "scheduler": True},
    {"cloud_enabled": True, "complete": False, "unfinished": 2, "scheduler": True,
     "will_offline": True},
    {"cloud_enabled": True, "complete": False, "unfinished": 2, "scheduler": True,
     "will_offline": True, "redecode": {"result": False}},
    # Intelligence offline or unwanted.
    {"cloud_enabled": True, "scheduler": False, "offline_message": "The agent did not start."},
    {"cloud_enabled": True, "scheduler": True, "core_healthy": False},
    {"cloud_enabled": True, "scheduler": True, "want_polish": False, "want_report": False},
    {"cloud_enabled": True, "scheduler": False, "want_polish": False, "want_report": False},
    # The cloud passes.
    dict(_CLOUD, polish=_POLISH_OK, consolidate=_REPORT_OK),
    dict(_CLOUD, polish=_POLISH_OK, consolidate=_REPORT_OK, will_offline=True),
    dict(_CLOUD, polish={"result": {"status": "failed", "message": "Timed out on block 2/3."}},
         consolidate=_REPORT_OK),
    dict(_CLOUD, polish={"result": {"status": "failed"}}, consolidate=_REPORT_OK),
    dict(_CLOUD, polish={"result": "raise"}, consolidate=_REPORT_OK),
    dict(_CLOUD, polish={"result": {}}, consolidate=_REPORT_OK),
    dict(_CLOUD, consolidate=_REPORT_OK),
    dict(_CLOUD, polish=_POLISH_OK, want_report=False),
    dict(_CLOUD, polish=_POLISH_OK, want_polish=False, consolidate=_REPORT_OK),
    dict(_CLOUD, polish=_POLISH_OK, consolidate={"result": "raise"}),
    dict(_CLOUD, polish=_POLISH_OK,
         consolidate={"result": {"status": "unavailable", "message": "No model answered."}}),
    dict(_CLOUD, polish=_POLISH_OK, consolidate={"result": {"status": "failed"}}),
    dict(_CLOUD, polish=_POLISH_OK, consolidate={"result": {}}),
    dict(_CLOUD, polish=_POLISH_OK, consolidate=_REPORT_OK, allow_raises=True),
    dict(_CLOUD, polish=_POLISH_OK, consolidate=_REPORT_OK,
         summary_stats={"segments": 1, "words": 2}),
    # Speaker identification.
    {"will_speaker_id": True,
     "speakers": {"result": {"ok": True, "applied": 3}, "on_start": True,
                  "progress": [["window 1", 1, 2]]}},
    {"will_speaker_id": True, "speakers": {"result": {"ok": True, "applied": 1}}},
    {"will_speaker_id": True,
     "speakers": {"result": {"ok": False, "skipped": True,
                             "error": "Audio-upload consent has not been given."}}},
    {"will_speaker_id": True, "speakers": {"result": {"ok": False, "skipped": True}}},
    {"will_speaker_id": True, "speakers": {"result": {"ok": False, "error": "Upload failed."}}},
    {"will_speaker_id": True, "speakers": {"result": "raise", "on_start": True}},
    {"will_speaker_id": True, "cloud_enabled": True, "scheduler": False,
     "speakers": {"result": {"ok": True, "applied": 2}}},
    {"will_speaker_id": True, "will_offline": True, "cloud_enabled": True, "complete": False,
     "scheduler": True, "speakers": {"result": {"ok": True, "applied": 2}}},
    dict(_CLOUD, will_speaker_id=True, will_offline=True, polish=_POLISH_OK,
         consolidate=_REPORT_OK,
         speakers={"result": {"ok": True, "applied": 2}, "on_start": True}),
    dict(_CLOUD, will_speaker_id=True, polish=_POLISH_OK, consolidate=_REPORT_OK,
         speakers={"result": {"ok": False, "error": "Upload failed."}}),
]

CASES = {"run": [[scenario] for scenario in _SCENARIOS]}

WRAPPERS = {"run": _run}
