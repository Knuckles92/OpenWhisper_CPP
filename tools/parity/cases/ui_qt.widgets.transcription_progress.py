"""The pure rules of ui_qt/widgets/transcription_progress.py (M2.3 upload-core, fv_dictation
upload_progress.h); the panel itself is M2.14's.

    format_elapsed(seconds)
    stage_for_overlay_state(state value) -> stage value or None
    stage_title(stage value) -> _STAGE_TITLES[stage]
    apply_steps(stage value, [three chip states before]) -> the three chip states after
    set_batch_position(total_files, position, total) -> [position, total, bar fraction]
    set_stage_bar(total_files, position, total, stage value) -> the bar after set_stage:
        a fraction, "indeterminate" or "reset"

The panel methods run on a stub that has the panel's batch fields, three stand-in chips and a
recording bar; set_style_property is swapped for a no-op for the call, so no QApplication is
made.
"""

import math


class _Chip:
    def __init__(self, state):
        self.state = state

    def set_state(self, state):
        self.state = state


class _Bar:
    def __init__(self):
        self.value = "indeterminate"
        self.is_indeterminate = True

    def set_fraction(self, fraction, animate=True):
        self.value = fraction
        self.is_indeterminate = False

    def set_indeterminate(self):
        self.value = "indeterminate"
        self.is_indeterminate = True

    def reset(self):
        self.value = "reset"
        self.is_indeterminate = False


class _Ignore:
    def __getattr__(self, _name):
        return lambda *args, **kwargs: None


def _panel(module, total_files=1, states=("pending", "pending", "pending")):
    panel_class = module.TranscriptionProgressPanel

    class Stub:
        is_batch = panel_class.is_batch
        set_stage = panel_class.set_stage
        set_batch_position = panel_class.set_batch_position
        _apply_steps = panel_class._apply_steps
        _tick = panel_class._tick

    stub = Stub()
    stub._batch_total = max(1, total_files)
    stub._batch_position = 1
    stub._started_at = None
    stub._stage = None
    stub.steps = [_Chip(s) for s in states]
    stub.bar = _Bar()
    if stub.is_batch:
        stub.bar.set_fraction(0.0, animate=False)
    for name in ("title_label", "detail_label", "batch_label", "stop_btn", "_clock",
                 "elapsed_label"):
        setattr(stub, name, _Ignore())
    return stub


def _without_restyle(module, call):
    original = module.set_style_property
    module.set_style_property = lambda *args, **kwargs: None
    try:
        return call()
    finally:
        module.set_style_property = original


def _stage_for_overlay_state(module, value):
    from ui_qt.overlay_state import OverlayState

    stage = module.stage_for_overlay_state(OverlayState(value))
    return None if stage is None else stage.value


def _stage_title(module, value):
    return module._STAGE_TITLES[module.ProgressStage(value)]


def _apply_steps(module, value, states):
    stub = _panel(module, states=states)
    stub._apply_steps(module.ProgressStage(value))
    return [chip.state for chip in stub.steps]


def _set_batch_position(module, total_files, position, total):
    stub = _panel(module, total_files)
    stub.set_batch_position(position, total, "file.wav")
    return [stub._batch_position, stub._batch_total, stub.bar.value]


def _set_stage_bar(module, total_files, position, total, value):
    stub = _panel(module, total_files)

    def run():
        stub.set_batch_position(position, total, "file.wav")
        stub.set_stage(module.ProgressStage(value))
        return stub.bar.value

    return _without_restyle(module, run)


WRAPPERS = {
    "stage_for_overlay_state": _stage_for_overlay_state,
    "stage_title": _stage_title,
    "apply_steps": _apply_steps,
    "set_batch_position": _set_batch_position,
    "set_stage_bar": _set_stage_bar,
}

_OVERLAY_STATES = ["none", "recording", "processing", "transcribing", "cleaning", "canceling",
                   "stt_enabled", "stt_disabled", "command_listening", "rewriting"]
_STAGES = ["preparing", "splitting", "transcribing", "cleaning", "done", "failed", "canceled"]
_CHIP_STATES = [
    ["pending", "pending", "pending"],
    ["active", "pending", "pending"],
    ["done", "active", "pending"],
    ["done", "done", "active"],
    ["done", "done", "done"],
    ["failed", "pending", "pending"],
    ["done", "failed", "pending"],
    ["active", "active", "pending"],
]

CASES = {
    "format_elapsed": [
        [s] for s in [0, 0.0, 0.4, 0.999, 1, 59, 59.99, 60, 61.5, 119.9, 599, 600, 3599, 3600,
                      3661.7, 86399.9, 359999, 360000, -0.5, -1, -61, 1e10, 1e17, 1.5e19,
                      2**63 + 0.0, 9.3e18, 1e300, math.nan, math.inf, -math.inf]
    ],
    "stage_for_overlay_state": [[s] for s in _OVERLAY_STATES],
    "stage_title": [[s] for s in _STAGES],
    "apply_steps": [[stage, states] for stage in _STAGES for states in _CHIP_STATES],
    "set_batch_position": [
        [files, position, total]
        for files in (1, 3)
        for position, total in [(1, 1), (1, 3), (2, 3), (3, 3), (4, 3), (0, 3), (-2, 3), (1, 0),
                                (2, 0), (5, 7), (7, 7), (1, 2)]
    ],
    "set_stage_bar": [
        [files, position, total, stage]
        for files in (1, 4)
        for position, total in [(1, 1), (1, 4), (3, 4), (4, 4), (9, 4), (2, 3), (1, 0)]
        for stage in _STAGES
    ],
}
