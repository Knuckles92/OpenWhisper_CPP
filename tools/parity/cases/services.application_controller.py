"""Curated inputs for services/application_controller.py (M1.1 appcore).

The controller's decisions are methods of a QObject that owns every runtime, so each vector runs
the real method bound onto a minimal stand-in, as OpenWhisper's own tests do
(test_engine_warmup._controller, test_application_controller._create_controller):

- refusal gates (_refuse_dictation_during_meeting, _refuse_engine_load_during_meeting,
  _acquire_backup, _acquire_walkthrough_library) over flag combinations;
- transcription_readiness_message, Parakeet branch, over the engine's states, and (M2.2) the
  branch of an engine that is not a speech engine (the API backend) over the selected model;
- PreviewEvents.offer sequences with an injected monotonic clock and timer;
- reload_trace: the engine reload state machine (reload_whisper_model, _do_reload_whisper_model,
  _start_initial_whisper_load, _reload_worker, _finish_speech_reload, release_local_engine,
  restore_local_engine) driven through scripted scenarios, one per controller test of
  tests/test_engine_warmup.py plus the debounce edges, recorded as the ordered list of inputs
  and the commands they produced. Event names are FioriVoce's EngineReloadMachine names:
  busy_changed (engine_busy_changed), status (status_update), device_info (device_info_update),
  missing_runtime (models.runtime_consent_requested), streaming_setup_due
  (streaming_runtime.streaming_setup_requested), ensure_model_available
  (ensure_local_model_available), start_load (the engine's reload_model called by the worker),
  start_warmup (warmup called), discard_load (the engine cleaned up after a switch), timer
  (the debounce timer started, with its milliseconds). Inputs are ["in", name, ...].
"""

from __future__ import annotations

import itertools
import threading
from types import SimpleNamespace

# ---------------------------------------------------------------- refusal gates

_DICTATION_FLAGS = ("backup_in_progress", "meeting_setup_open", "meeting_active")

# _acquire_backup reads these; the C++ snapshot folds job_active and backend_transcribing into
# `transcribing`, and the four meeting_* sub-states into `meeting_finishing`.
_BACKUP_FLAGS = (
    "shutting_down", "startup_ready", "recovery_running", "library_change", "write_up_running",
    "backup_in_progress", "recording", "job_active", "backend_transcribing", "meeting_active",
    "meeting_finalizing", "meeting_background_engines", "meeting_archive_starting",
    "meeting_retry", "rule_dictation_busy",
)

_LIBRARY_FLAGS = (
    "shutting_down", "startup_ready", "recovery_running", "backup_in_progress",
    "library_change", "write_up_running", "job_active", "job_is_walkthrough",
)


def _every_combination(names):
    return [[dict(zip(names, values))]
            for values in itertools.product((False, True), repeat=len(names))]


def _single_and_pair_flips(names, base):
    """The base, every single flag flipped, and every pair flipped."""
    cases = [[dict(base)]]
    for count in (1, 2):
        for flipped in itertools.combinations(names, count):
            flags = dict(base)
            for name in flipped:
                flags[name] = not flags[name]
            cases.append([flags])
    return cases


class _Recorder:
    def __init__(self, events, name):
        self.events, self.name = events, name

    def emit(self, *args):
        self.events.append([self.name, *args])


def _refuse_dictation(module, flags):
    events = []
    stub = SimpleNamespace(
        _backup_in_progress=flags["backup_in_progress"],
        ui_controller=SimpleNamespace(_meeting_setup_open=flags["meeting_setup_open"]),
        is_meeting_active=lambda: flags["meeting_active"],
        status_update=_Recorder(events, "status"),
    )
    refused = module.ApplicationController._refuse_dictation_during_meeting(stub)
    return {"refused": refused, "statuses": [event[1] for event in events]}


def _refuse_engine_load(module, flags):
    events = []
    stub = SimpleNamespace(
        is_meeting_active=lambda: flags["meeting_active"],
        status_update=_Recorder(events, "status"),
    )
    refused = module.ApplicationController._refuse_engine_load_during_meeting(stub)
    return {"refused": refused, "statuses": [event[1] for event in events]}


def _recovery_future(running):
    return SimpleNamespace(done=lambda: not running) if running else None


def _acquire_backup(module, flags):
    runtime = SimpleNamespace(
        _capture_lock=threading.Lock(), _job_lock=threading.Lock(),
        writing_up=flags["write_up_running"], _job_active=flags["job_active"],
    )
    meeting = SimpleNamespace(
        _lock=threading.Lock(),
        is_claimed=flags["meeting_active"],
        is_finalizing=flags["meeting_finalizing"],
        _background_engines={"parakeet": object()} if flags["meeting_background_engines"] else {},
        _archive_starting=flags["meeting_archive_starting"],
        _retry_meeting_id="m_1" if flags["meeting_retry"] else None,
    )
    rules = SimpleNamespace(dictation_state="listening" if flags["rule_dictation_busy"] else "idle")
    stub = SimpleNamespace(
        _shutting_down=flags["shutting_down"],
        _startup_database_ready=flags["startup_ready"],
        _startup_recovery_future=_recovery_future(flags["recovery_running"]),
        transcription_runtime=runtime,
        meeting_runtime=meeting,
        _walkthrough_library_busy=flags["library_change"],
        _backup_in_progress=flags["backup_in_progress"],
        recorder=SimpleNamespace(is_recording=flags["recording"]),
        current_backend=SimpleNamespace(is_transcribing=flags["backend_transcribing"]),
        ui_controller=SimpleNamespace(
            _settings_dialog=SimpleNamespace(cleanup_rules=rules),
            main_window=SimpleNamespace(centralWidget=lambda: None),
        ),
    )
    return module.ApplicationController._acquire_backup(stub)


def _acquire_walkthrough_library(module, flags):
    from services.dictation_pipeline import JobMode

    mode = JobMode.WALKTHROUGH if flags["job_is_walkthrough"] else JobMode.DICTATION
    runtime = SimpleNamespace(
        _job_lock=threading.Lock(),
        writing_up=flags["write_up_running"],
        _job_active=flags["job_active"],
        _active_job=SimpleNamespace(mode=mode),
    )
    stub = SimpleNamespace(
        _shutting_down=flags["shutting_down"],
        _startup_database_ready=flags["startup_ready"],
        _startup_recovery_future=_recovery_future(flags["recovery_running"]),
        transcription_runtime=runtime,
        _backup_in_progress=flags["backup_in_progress"],
        _walkthrough_library_busy=flags["library_change"],
    )
    return module.ApplicationController._acquire_walkthrough_library(stub)


# ---------------------------------------------------------------- readiness

_READINESS_FLAGS = ("has_backend", "available", "reload_in_flight", "model_missing",
                    "closed_by_cancel", "has_last_error")


def _speech_engine_class():
    from transcriber.optional_backend import LocalSpeechBackend

    class Engine(LocalSpeechBackend):
        """A Parakeet engine whose state is set, not loaded."""

        is_model_missing = False
        device_info = ""

        def __init__(self, available, model_missing, should_cancel, last_error, device_info):
            self.backend_id = "parakeet"
            self.model_name = "parakeet-v3"
            self._available = available
            self.is_model_missing = model_missing
            self.should_cancel = should_cancel
            self.last_error = last_error
            self.device_info = device_info

        def is_available(self):
            return self._available

    return Engine


def _readiness(module, flags):
    calls = []
    engine = None
    if flags["has_backend"]:
        last_error = "Install Parakeet's GPU runtime in Downloads." if flags["has_last_error"] else ""
        device_info = last_error or "Parakeet is not loaded"
        engine = _speech_engine_class()(flags["available"], flags["model_missing"],
                                         flags["closed_by_cancel"], last_error, device_info)
    stub = SimpleNamespace(
        current_backend=engine,
        _reload_in_flight=flags["reload_in_flight"],
        _current_model_name="parakeet",
        ensure_local_model_available=lambda: calls.append("consent"),
        reload_whisper_model=lambda: calls.append("reload"),
    )
    message = module.ApplicationController.transcription_readiness_message(stub)
    return {
        "device_info": engine.device_info if engine is not None else "",
        "message": message,
        "request_reload": "reload" in calls,
        "request_model_consent": "consent" in calls,
    }


def _readiness_other_engine(module, available, model_name):
    """The branch for an engine that is not a speech engine (M2.2): the API backend, or any
    other kind the controller cannot name. The stand-in backend is neither a LocalSpeechBackend
    nor a LocalWhisperBackend, as OpenAIBackend is not."""
    calls = []
    stub = SimpleNamespace(
        current_backend=SimpleNamespace(is_available=lambda: available),
        _reload_in_flight=False,
        _current_model_name=model_name,
        ensure_local_model_available=lambda: calls.append("consent"),
        reload_whisper_model=lambda: calls.append("reload"),
    )
    stub.local_whisper_loading_message = (
        lambda: module.ApplicationController.local_whisper_loading_message(stub))
    message = module.ApplicationController.transcription_readiness_message(stub)
    return {
        "message": message,
        "request_reload": "reload" in calls,
        "request_model_consent": "consent" in calls,
    }


# ---------------------------------------------------------------- PreviewEvents

class _FakeTimer:
    """QTimer as PreviewEvents and the reload debounce use it, on an injected clock."""

    def __init__(self, clock, events, *, record_start_time=True):
        self.clock, self.events = clock, events
        self.record_start_time = record_start_time
        self.deadline = None
        self.timeout = SimpleNamespace(connect=self._connect)
        self.callback = None

    def _connect(self, callback):
        self.callback = callback

    def setSingleShot(self, _single):
        pass

    def start(self, ms):
        self.deadline = self.clock[0] + ms
        if self.record_start_time:
            self.events.append(["timer", self.clock[0], ms])
        else:
            self.events.append(["timer", ms])

    def stop(self):
        self.deadline = None

    def isActive(self):
        return self.deadline is not None

    def fire(self):
        self.deadline = None
        self.callback()


def _preview_events(module, offers):
    """offers: [[t_ms, job, text, final], ...] in time order. Returns the publish and timer events
    with their times in milliseconds; a timer fires at its deadline, before an offer at that time.
    """
    clock = [0]
    events = []
    saved_time = module.time
    module.time = SimpleNamespace(monotonic=lambda: clock[0] / 1000)
    try:
        preview = module.PreviewEvents.__new__(module.PreviewEvents)
        preview._publish = lambda job, text, final: events.append(
            ["publish", clock[0], job, text, final])
        preview._timer = _FakeTimer(clock, events)
        preview._timer.timeout.connect(preview._flush)
        preview._sent_at = None
        preview._held = None

        def run_timer_until(t):
            timer = preview._timer
            while timer.isActive() and timer.deadline <= t:
                clock[0] = timer.deadline
                timer.fire()

        for t, job, text, final in offers:
            run_timer_until(t)
            clock[0] = t
            preview.offer(job, text, final)
        if preview._timer.isActive():
            run_timer_until(preview._timer.deadline)
    finally:
        module.time = saved_time
    return events


# ---------------------------------------------------------------- reload_trace

_LOAD_OUTCOMES = {
    # outcome: (ReadyState, device_info, message), as the C++ load reports it
    "ready": ("Ready", "Parakeet TDT 0.6B v3 | cuda", ""),
    "failed": ("Failed", "", "native load failed"),
    "model_missing": ("ModelMissing", "Download Parakeet TDT 0.6B v3 in Downloads.", ""),
    "runtime_missing": ("RuntimeMissing", "Install Parakeet's GPU runtime in Downloads.", ""),
}


def _reload_controller(module, events, activity):
    from transcriber.optional_backend import LocalSpeechBackend

    clock = [0]
    queue = []

    class Engine(LocalSpeechBackend):
        """The selected Parakeet engine; its loads and warm-ups follow the scenario's script."""

        is_model_missing = False
        device_info = ""

        def __init__(self):
            self.backend_id = "parakeet"
            self.model_name = "parakeet-v3"
            self.should_cancel = False
            self.is_transcribing = False
            self.last_error = ""
            self.cpu_fallback_note = ""
            self.loaded = False
            self.quiet = False
            self.loads = []        # per load: {"outcome": ..., "switch": bool}
            self.warmups = []      # per warm-up: steps run while it is in progress

        def is_available(self):
            return self.loaded

        @property
        def model(self):
            # release_local_engine frees every engine whose model is set.
            return "decoder" if self.loaded else None

        def reload_model(self, *_args, **_kwargs):
            events.append(["start_load"])
            script = self.loads.pop(0) if self.loads else {"outcome": "ready"}
            state, info, message = _LOAD_OUTCOMES[script["outcome"]]
            if script.get("switch"):
                controller.current_backend = object()
                events.append(["in", "select", "other"])
            events.append(["in", "load_finished", state, info, message])
            self.loaded = state == "Ready"
            self.is_model_missing = state == "ModelMissing"
            self.device_info = info
            if state == "Failed":
                raise RuntimeError(message)

        def warmup(self):
            events.append(["start_warmup"])
            for step in (self.warmups.pop(0) if self.warmups else []):
                apply(step)
            events.append(["in", "warmup_finished"])
            return False

        def cleanup(self):
            self.loaded = False
            if not self.quiet:
                events.append(["discard_load"])

    engine = Engine()
    timer = _FakeTimer(clock, events, record_start_time=False)

    controller = SimpleNamespace(
        _shutting_down=False,
        _engine_lock=threading.RLock(),
        _reload_in_flight=False,
        _reload_pending=False,
        _reload_note="",
        _pending_streaming_setup=False,
        current_backend=engine,
        _current_model_name="parakeet",
        transcription_backends={"parakeet": engine},
        recorder=SimpleNamespace(is_recording=False),
        is_meeting_active=lambda: activity["meeting_active"],
        is_transcribing=lambda: activity["transcribing"],
        host_update_active=lambda: activity["host_update_active"],
        ensure_local_model_available=lambda: events.append(["ensure_model_available"]),
        _engine_released_for_lease=False,
        _restore_after_reload=False,
        _reload_handoff_lock=threading.Lock(),
        _streaming_backend=None,
        _reload_timer=timer,
        executor=SimpleNamespace(submit=lambda fn, *a: queue.append((fn, a))),
    )
    controller.models = SimpleNamespace(
        runtime_consent_requested=_Recorder(events, "missing_runtime"))
    controller.device_info_update = _Recorder(events, "device_info")
    controller.status_update = _Recorder(events, "status")
    controller.engine_busy_changed = _Recorder(events, "busy_changed")
    controller.streaming_runtime = SimpleNamespace(
        streaming_setup_requested=_Recorder(events, "streaming_setup_due"),
        cleanup=lambda: events.append(["stop_preview"]))
    for name in ("_reload_worker", "_reload_selected_engine", "_finish_speech_reload",
                 "_flush_pending_streaming_setup", "release_local_engine",
                 "restore_local_engine", "_submit_restore_reload", "reload_whisper_model",
                 "_arm_reload_timer", "_do_reload_whisper_model",
                 "_start_initial_whisper_load"):
        setattr(controller, name,
                getattr(module.ApplicationController, name).__get__(controller))
    # On the Qt thread the debounce request arms the timer at once.
    controller.reload_debounce_requested = SimpleNamespace(emit=controller._arm_reload_timer)
    timer.timeout.connect(controller._do_reload_whisper_model)

    def drain():
        while queue:
            fn, args = queue.pop(0)
            fn(*args)

    def apply(step):
        name, *args = step
        if name == "activity":
            flags = args[0]
            activity.update(flags)
            controller.recorder.is_recording = activity["recording"]
            events.append(["in", "activity", dict(activity)])
        elif name == "request":
            note = args[0] if args else ""
            events.append(["in", "request", note])
            if note:
                controller._reload_note = note
            controller.reload_whisper_model()
        elif name == "timer":
            events.append(["in", "timer"])
            if timer.isActive():
                timer.fire()
        elif name == "initial_load":
            events.append(["in", "initial_load"])
            controller._pending_streaming_setup = True
            controller._start_initial_whisper_load()
        elif name == "release":
            # The input comes before what it emits; its result is filled in after.
            event = ["in", "release"]
            events.append(event)
            engine.quiet = True
            try:
                event.append(controller.release_local_engine())
            finally:
                engine.quiet = False
        elif name == "restore":
            events.append(["in", "restore"])
            controller.restore_local_engine()
        elif name == "engine_closed":
            events.append(["in", "engine_closed"])
            engine.quiet = True
            try:
                with controller._engine_lock:
                    engine.cleanup()
            finally:
                engine.quiet = False
        else:
            raise ValueError(f"unknown step {name}")

    return controller, engine, apply, drain


_IDLE = {"recording": False, "transcribing": False, "meeting_active": False,
         "host_update_active": False}

# name: {"loads": [...], "warmups": [[steps]...], "steps": [...]}
_SCENARIOS = {
    # test_reload_warms_after_load_then_clears_busy_and_sets_up_preview
    "warms_after_load": {"loads": [{"outcome": "ready"}], "steps": [["initial_load"]]},
    # test_failed_warmup_still_finishes_the_reload (the warm-up's own failure is the engine's)
    "failed_warmup_finishes": {"loads": [{"outcome": "ready"}], "steps": [["initial_load"]]},
    # test_failed_load_is_not_warmed
    "failed_load": {"loads": [{"outcome": "failed"}], "steps": [["initial_load"]]},
    # test_missing_model_is_not_warmed
    "missing_model": {"loads": [{"outcome": "model_missing"}], "steps": [["initial_load"]]},
    # test_missing_runtime_is_not_warmed
    "missing_runtime": {"loads": [{"outcome": "runtime_missing"}], "steps": [["initial_load"]]},
    # test_engine_switched_during_load_is_released_not_warmed
    "switched_during_load": {"loads": [{"outcome": "ready", "switch": True}],
                             "steps": [["initial_load"]]},
    # test_meeting_lease_during_warmup_ends_it_and_finishes_the_reload
    "lease_ends_warmup": {"loads": [{"outcome": "ready"}], "warmups": [[["engine_closed"]]],
                          "steps": [["initial_load"]]},
    # test_lease_returned_during_warmup_reloads_the_engine_it_closed
    "lease_returned_during_warmup": {
        "loads": [{"outcome": "ready"}, {"outcome": "ready"}],
        "warmups": [[["release"], ["restore"]]],
        "steps": [["initial_load"]],
    },
    # test_lease_still_held_after_warmup_asks_for_no_runtime
    "lease_held_after_warmup": {"loads": [{"outcome": "ready"}], "warmups": [[["release"]]],
                                "steps": [["initial_load"]]},
    # Debounce: a recording that starts after the request re-arms the timer until it ends.
    "debounce_rearms_while_recording": {
        "loads": [{"outcome": "ready"}],
        "steps": [["request"], ["activity", {"recording": True}], ["timer"], ["timer"],
                  ["activity", {"recording": False}], ["timer"]],
    },
    # test_reload_whisper_model_refused_while_recording
    "refused_while_recording": {
        "steps": [["activity", {"recording": True}], ["request"], ["timer"]],
    },
    "refused_while_transcribing": {
        "steps": [["activity", {"transcribing": True}], ["request"], ["timer"]],
    },
    # test_whisper_reload_is_refused_during_a_meeting
    "refused_during_meeting": {
        "steps": [["activity", {"meeting_active": True}], ["request"], ["timer"]],
    },
    "meeting_cancels_pending_reload": {
        "steps": [["request", "Switching to Parakeet TDT 0.6B v3..."],
                  ["activity", {"meeting_active": True}], ["timer"],
                  ["activity", {"meeting_active": False}], ["request"], ["timer"]],
        "loads": [{"outcome": "ready"}],
    },
    "host_update_rearms": {
        "loads": [{"outcome": "ready"}],
        "steps": [["activity", {"host_update_active": True}], ["request"], ["timer"],
                  ["activity", {"host_update_active": False}], ["timer"]],
    },
    "note_replaces_reloading_status": {
        "loads": [{"outcome": "ready"}],
        "steps": [["request", "Switching to Parakeet TDT 0.6B v3 for laptop..."], ["timer"]],
    },
    "requests_coalesce": {
        "loads": [{"outcome": "ready"}],
        "steps": [["request"], ["request"], ["request"], ["timer"]],
    },
    # A request during a reload waits for it, then reloads with the newest settings.
    "rearms_while_in_flight": {
        "loads": [{"outcome": "ready"}, {"outcome": "ready"}],
        "warmups": [[["request"], ["timer"]]],
        "steps": [["initial_load"], ["timer"]],
    },
    # The initial load while a recording runs gives up at the worker.
    "initial_load_while_recording": {
        "steps": [["activity", {"recording": True}], ["initial_load"]],
    },
    "initial_load_twice": {
        "loads": [{"outcome": "ready"}],
        "warmups": [[["initial_load"]]],
        "steps": [["initial_load"]],
    },
    # test_release_is_a_noop_when_no_model_is_loaded / test_restore_reloads_only_after_a_release
    "release_without_engine": {"steps": [["release"], ["restore"]]},
    # test_release_frees_the_local_model_and_owes_a_reload / test_restore_is_idempotent
    "release_then_restore": {
        "loads": [{"outcome": "ready"}, {"outcome": "ready"}],
        "steps": [["initial_load"], ["release"], ["restore"], ["restore"]],
    },
    # test_repeated_lease_release_keeps_restore_obligation
    "repeated_release_keeps_restore": {
        "loads": [{"outcome": "ready"}, {"outcome": "ready"}],
        "steps": [["initial_load"], ["release"], ["release"], ["restore"]],
    },
    "restore_during_meeting_waits_for_worker": {
        "loads": [{"outcome": "ready"}],
        "steps": [["initial_load"], ["release"], ["activity", {"meeting_active": True}],
                  ["restore"]],
    },
}


def _reload_trace(module, name):
    scenario = _SCENARIOS[name]
    events = []
    activity = dict(_IDLE)
    saved_preload = module._preload_recording_reader
    module._preload_recording_reader = lambda _backend: None
    try:
        controller, engine, apply, drain = _reload_controller(module, events, activity)
        engine.loads = [dict(load) for load in scenario.get("loads", [])]
        engine.warmups = [list(steps) for steps in scenario.get("warmups", [])]
        for step in scenario["steps"]:
            apply(step)
            drain()
    finally:
        module._preload_recording_reader = saved_preload
    return events


WRAPPERS = {
    "_refuse_dictation_during_meeting": _refuse_dictation,
    "_refuse_engine_load_during_meeting": _refuse_engine_load,
    "_acquire_backup": _acquire_backup,
    "_acquire_walkthrough_library": _acquire_walkthrough_library,
    "transcription_readiness_message": _readiness,
    "transcription_readiness_message.other_engine": _readiness_other_engine,
    "PreviewEvents.offer": _preview_events,
    "reload_trace": _reload_trace,
}

_BACKUP_BASE = {name: False for name in _BACKUP_FLAGS}
_BACKUP_BASE["startup_ready"] = True

_LIBRARY_BASE = {name: False for name in _LIBRARY_FLAGS}
_LIBRARY_BASE["startup_ready"] = True

CASES = {
    "_refuse_dictation_during_meeting": _every_combination(_DICTATION_FLAGS),
    "_refuse_engine_load_during_meeting": _every_combination(("meeting_active",)),
    "_acquire_backup": _single_and_pair_flips(_BACKUP_FLAGS, _BACKUP_BASE),
    "_acquire_walkthrough_library": _every_combination(_LIBRARY_FLAGS),
    "transcription_readiness_message": _every_combination(_READINESS_FLAGS),
    "PreviewEvents.offer": [
        # The first offer goes out at once; one 100 ms later is held for 151 ms.
        [[[0, "j1", "a", False], [100, "j1", "ab", False]]],
        # Only the newest held text is sent when the quarter second is up.
        [[[0, "j1", "a", False], [50, "j1", "ab", False], [120, "j1", "abc", False],
          [200, "j1", "abcd", False]]],
        # A final preview goes out at once and drops what was held.
        [[[0, "j1", "a", False], [100, "j1", "ab", False], [150, "j1", "abc", True]]],
        # Offers a quarter second apart or more are never held.
        [[[0, "j1", "a", False], [250, "j1", "ab", False], [600, "j1", "abc", False]]],
        # After the flush the next offer waits from the flushed send.
        [[[0, "j1", "a", False], [10, "j1", "ab", False], [300, "j1", "abc", False],
          [400, "j1", "abcd", False], [420, "j1", "abcde", False]]],
        # Times whose float subtraction lands just under a whole millisecond.
        [[[300, "j1", "a", False], [350, "j1", "ab", False]]],
        [[[100, "j1", "a", False], [200, "j1", "ab", False], [700, "j1", "abc", False],
          [810, "j1", "abcd", False]]],
        [[[1000, "j1", "a", False], [1249, "j1", "ab", False], [1250, "j1", "abc", False]]],
        [[[0, "j1", "a", False], [249, "j1", "ab", False]]],
        # A new job's text replaces the held text of the old one.
        [[[0, "j1", "a", False], [100, "j1", "ab", False], [120, "j2", "x", False]]],
        # A final first offer, and a final while nothing is held.
        [[[0, "j1", "done", True], [10, "j2", "a", False], [20, "j2", "ab", True]]],
        [[[0, "j1", "", False], [1, "j1", "", False], [2, "j1", "", False]]],
    ],
    "reload_trace": [[name] for name in _SCENARIOS],
    "transcription_readiness_message.other_engine": [
        [available, model_name]
        for available in (False, True)
        for model_name in ("api", "custom", "local_whisper", "")
    ],
}

PINS = {
    "reload_debounce_ms": 400,
    "note": "reload_trace scenarios bind the controller's reload methods onto a stand-in, as "
            "tests/test_engine_warmup.py does; the engine is scripted, not a speech worker",
}
