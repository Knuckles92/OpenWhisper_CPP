"""Curated inputs for services/runtime/hotkeys.py (M1.5 hotkeys): traces of the push-and-hold
and hands-free latch decisions (record_key_pressed .. _reset_hold).

A trace runs a real HotkeyRuntime against a fake controller and recorder, as OpenWhisper's tests
do (tests/test_push_hold_recording.py), with its OrderedDispatcher replaced by a recorder of
deadlines (a submitted call runs at once) and its clock by a fake one that sleep() advances, so
the 0.5 s wait for the stream ends at once. A trace is [latch setting, steps]; each step is one of
    {"op": "press", "at"}, {"op": "release", "at"}, {"op": "deadline"} (the armed deadline
    fires), {"op": "ended"} (the recording ends for any reason), {"op": "reset"} (a trigger mode
    change), {"op": "recording", "value"}, {"op": "canceled", "value"},
    {"op": "accept", "value"} (whether the next start is accepted)
and the result has one entry per step: what the runtime did, in order ("start", "stop",
"cancel", "latch", "unlatch", "arm@<when>", "cancel_deadline"), and the hold state after it.
The C++ replay drives HoldMachine (tests/hotkeys/test_golden.cpp).
"""

import types


class _Recorder:
    def __init__(self):
        self.is_recording = False
        self.capture_canceled = False

    def has_recording_data(self):
        return self.is_recording


class _Signal:
    def __init__(self):
        self.emitted = []

    def emit(self, *args):
        self.emitted.append(args)


class _Controller:
    def __init__(self, done):
        self.recorder = _Recorder()
        self.hotkey_manager = None
        self.status_update = _Signal()
        self.accept = True
        self.done = done

    def start_recording(self):
        self.done.append("start")
        if self.accept:
            self.recorder.is_recording = True
        return self.accept

    def stop_recording(self):
        self.done.append("stop")

    def cancel(self):
        self.done.append("cancel")


class _Dispatcher:
    def __init__(self, done):
        self.done = done
        self.pending = None

    def submit(self, fn, *args):
        fn(*args)

    def call_at(self, when, fn, *args):
        self.done.append(f"arm@{when!r}")
        self.pending = (fn, args)

    def cancel_deadline(self):
        self.done.append("cancel_deadline")
        self.pending = None

    def stop(self):
        pass


def _trace(module, latch_on, steps):
    done = []
    controller = _Controller(done)
    runtime = module.HotkeyRuntime(controller)
    dispatcher = _Dispatcher(done)
    runtime._dispatcher = dispatcher
    runtime.hands_free_changed.connect(lambda on: done.append("latch" if on else "unlatch"))
    clock = [1000.0]

    def sleep(seconds):
        clock[0] += seconds

    original_time = module.time
    original_latch = module.resolve_recording_hands_free_latch
    module.time = types.SimpleNamespace(monotonic=lambda: clock[0], sleep=sleep)
    module.resolve_recording_hands_free_latch = lambda *args, **kwargs: latch_on
    results = []
    try:
        for step in steps:
            done.clear()
            op = step["op"]
            if op == "press":
                runtime.record_key_pressed(step["at"])
            elif op == "release":
                runtime.record_key_released(step["at"])
            elif op == "deadline":
                if dispatcher.pending is not None:
                    fn, args = dispatcher.pending
                    dispatcher.pending = None
                    fn(*args)
            elif op == "ended":
                controller.recorder.is_recording = False
                runtime._on_recording_state_changed(False)
            elif op == "reset":
                runtime._reset_hold()
            elif op == "recording":
                controller.recorder.is_recording = step["value"]
            elif op == "canceled":
                controller.recorder.capture_canceled = step["value"]
            elif op == "accept":
                controller.accept = step["value"]
            else:
                raise ValueError(op)
            results.append([list(done), runtime._hold_state])
    finally:
        module.time = original_time
        module.resolve_recording_hands_free_latch = original_latch
        runtime._dispatcher = dispatcher
    return results


WRAPPERS = {"hold_trace": _trace}


def press(at):
    return {"op": "press", "at": at}


def release(at):
    return {"op": "release", "at": at}


DEADLINE = {"op": "deadline"}
ENDED = {"op": "ended"}
RESET = {"op": "reset"}

CASES = {
    "hold_trace": [
        # A long hold stops; exactly the minimum hold stops too.
        [True, [press(100.0), release(100.5)]],
        [True, [press(100.0), release(100.25)]],
        [True, [press(100.0), release(100.2499)]],
        # A short tap cancels without the latch.
        [False, [press(100.0), release(100.1)]],
        # A tap, then a second press inside the window latches; its release does nothing; the
        # next press stops.
        [True, [press(100.0), release(100.1), press(100.25), release(100.33), press(103.0),
                release(103.1)]],
        [True, [press(100.0), release(100.1), press(100.5)]],
        [True, [press(100.0), release(100.1), press(100.50000001), release(100.6)]],
        # The window closes on the deadline: the tap cancels.
        [True, [press(100.0), release(100.1), DEADLINE, press(101.0)]],
        # A deadline from an older generation does nothing.
        [True, [press(100.0), release(100.1), RESET, DEADLINE]],
        # A refused start makes the release a no-op.
        [True, [{"op": "accept", "value": False}, press(100.0), release(100.5)]],
        # A press during a previous stop's post-roll is ignored.
        [True, [{"op": "recording", "value": True}, press(100.0), release(100.4)]],
        # A start whose stream never opens: the release gives up quietly.
        [True, [press(100.0), {"op": "recording", "value": False}, release(100.5)]],
        # The cancel hotkey already ended the capture.
        [True, [press(100.0), {"op": "canceled", "value": True}, release(101.0)]],
        [True, [press(100.0), release(100.1), {"op": "canceled", "value": True}, DEADLINE]],
        # A lost release: the next press continues the hold.
        [True, [press(100.0), press(100.4), release(100.9)]],
        # A recording that ended another way drops the latch.
        [True, [press(100.0), release(100.1), ENDED, press(100.2)]],
        [True, [press(100.0), release(100.1), press(100.2), ENDED, press(101.0)]],
        [True, [press(100.0), ENDED, release(100.1)]],
        [True, [press(100.0), {"op": "recording", "value": False},
                {"op": "recording", "value": True}, release(100.1), ENDED, DEADLINE]],
        # A mode change resets a latch.
        [True, [press(100.0), release(100.1), press(100.2), RESET, press(101.0)]],
        # A press at time zero: Python's `if press_time` counts it as no press.
        [True, [press(0.0), release(5.0)]],
    ],
}
