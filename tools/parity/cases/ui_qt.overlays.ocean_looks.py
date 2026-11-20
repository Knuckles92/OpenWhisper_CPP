"""Curated inputs for ui_qt/overlays/ocean_looks.py (fv_uimath SchoolSim and FizzSim).

The sims draw from random.Random(5) (school) and Random(11) (Fizz), and C++ replays those
streams (python_random_engine), so a whole run is a vector: "python_random" pins the engine
against the first draws of random(), uniform and randrange, "School.reset" the starting fish,
"School.run" and "Fizz.run" a scripted run frame by frame. Every case drives a stub overlay
whose ribbon says what shown and history it has; the sim's own clock and level are the only
other inputs. A frame is [shown, dt] while recording and [dt, rate] while working.

"School._roll_springs" is the deterministic part (no fish): the springs of five layers stepped
from a given displacement and velocity. "School.colors" gives ocean() and fish_color() for both
themes at a given clock and particle hue. "Fizz.work_levels" is draw_work's mutation of the
glasses' levels, drawn through a painter that ignores everything.
"""
import math
from types import SimpleNamespace

# Floats pass through the CRT's sin, cos, exp and pow on both sides; hypot differs in the last
# bit between CPython's and the C library's, and a flock settles but does not forget.
FLOAT_TOLERANCE = 1e-7

_KINDS = {"baitball": "BaitBall", "fish": "RealFish", "glow": "DeepGlow"}


class _NullPainter:
    def __getattr__(self, name):
        return lambda *args, **kwargs: None


def _overlay(width=300, shown=0.0):
    history = [0.0] * 100
    return SimpleNamespace(
        width=lambda: width,
        _ribbon=SimpleNamespace(shown=shown, history=history),
        style=SimpleNamespace(animation_time=0.0, color_shift_speed=50.0),
    )


def _school(module, kind, width):
    return getattr(module, _KINDS[kind])(_overlay(width))


def _fish(school):
    return [[f["x"], f["y"], f["vx"], f["vy"], f["r"], f["hue"], f["phase"]] for f in school.fish]


def _sample(school):
    ys = sorted(f["y"] for f in school.fish)
    return [school.level, sum(f["y"] for f in school.fish) / len(school.fish), max(school.u[0]),
            min(school.u[0]), school._t, school.morph(), school.voice(), ys[0], ys[-1]]


def _state(school):
    first = school.fish[0]
    return {
        "fish": [[f["x"], f["y"], f["vx"], f["vy"]] for f in school.fish],
        "trail": [[list(p) for p in f["trail"]] for f in school.fish[:3]],
        "trail_length": [len(f["trail"]) for f in school.fish],
        "u": [list(row) for row in school.u],
        "uv": [list(row) for row in school.uv],
        "t": school._t,
        "level": school.level,
        "working": school.working,
        "work_t": school._work_t,
        "rate": school.rate,
        "y_of": [[school.y_of(layer, p) for p in (0, 1, 11, 24, 36, 47)]
                 for layer in range(school.LAYERS)],
        "first": [first["x"], first["y"]],
    }


def _python_random(module, seed, count, bound):
    import random

    unit = random.Random(seed)
    uniform = random.Random(seed)
    below = random.Random(seed)
    return {
        "unit": [unit.random() for _ in range(count)],
        "uniform": [uniform.uniform(-25, 25) for _ in range(count)],
        "below": [below.randrange(bound) for _ in range(count)],
    }


def _school_reset(module, kind, width):
    school = _school(module, kind, width)
    return {"count": len(school.fish), "fish": _fish(school)}


def _school_run(module, kind, width, frames, work):
    school = _school(module, kind, width)
    ribbon = school.overlay._ribbon
    samples = []
    for shown, dt in frames:
        ribbon.shown = shown
        school.advance(dt)
        samples.append(_sample(school))
    work_samples = []
    if work:
        school.begin_work()
        for dt, rate in work:
            school.advance_work(dt, rate)
            work_samples.append(_sample(school))
    return {"samples": samples, "work_samples": work_samples, "state": _state(school)}


def _roll_springs(module, u, uv, h):
    stub = SimpleNamespace(POINTS=48, LAYERS=5, u=[list(row) for row in u],
                           uv=[list(row) for row in uv])
    module._School._roll_springs(stub, h)
    return {"u": stub.u, "uv": stub.uv}


def _colors(module, kind, theme, t, animation_time, alpha, light):
    from ui_qt.utils.palette import palette_for, set_current_palette

    set_current_palette(palette_for(theme))
    school = _school(module, kind, 300)
    school._t = t
    school.overlay.style.animation_time = animation_time
    return {
        "ocean": [list(school.ocean(layer, alpha, light).getRgb()) for layer in range(5)],
        "fish": [list(school.fish_color(f, alpha).getRgb()) for f in school.fish[:8]],
    }


def _fizz(module, width=300):
    return module.Fizz(_overlay(width))


def _fizz_state(fizz):
    return {
        "h": list(fizz.h),
        "v": list(fizz.v),
        "bubbles": [[b["lane"], b["x"], b["y"], b["vy"], b["r"]] for b in fizz.bubbles],
        "splashes": [list(s) for s in fizz.splashes],
        "t": fizz._t,
        "carry": fizz._carry,
    }


def _fizz_run(module, width, frames, work, draws):
    fizz = _fizz(module, width)
    ribbon = fizz.overlay._ribbon
    samples = []
    for shown, dt in frames:
        ribbon.shown = shown
        fizz.advance(dt)
        samples.append([sum(fizz.h) / len(fizz.h), max(fizz.h), len(fizz.bubbles),
                        len(fizz.splashes)])
    work_samples = []
    if work:
        fizz.begin_work()
        for (dt, rate), (morph, clock) in zip(work, draws, strict=True):
            fizz.advance_work(dt, rate)
            fizz.draw_work(_NullPainter(), morph, clock)
            work_samples.append(list(fizz.h) + [len(fizz.bubbles), len(fizz.splashes)])
    return {"samples": samples, "work_samples": work_samples, "state": _fizz_state(fizz)}


def _tables(module):
    return {
        "school": {name: getattr(module._School, name) for name in (
            "FISH", "BASE", "TOP", "LAYERS", "POINTS", "BOTTOM", "GAP", "GAP_MIN", "SWELL", "RANGE",
            "COHESION", "ALIGN", "SEPARATE", "ATTACK_S", "RELEASE_S", "VOICE_FLOOR", "STEER",
            "WATER_HUE")},
        "kinds": {"baitball": [module.BaitBall.FISH, module.BaitBall.COHESION, module.BaitBall.SQUASH,
                               module.BaitBall.DEPTH],
                  "fish": [module.RealFish.FISH], "glow": [module.DeepGlow.FISH, module.DeepGlow.TRAIL,
                                                           module.DeepGlow.PING_S]},
        "fizz": {name: getattr(module.Fizz, name) for name in (
            "LANES", "BASE", "TOP", "KICK", "GRAVITY", "LEAK", "SLOSH_S", "MAX_H")},
    }


WRAPPERS = {
    "python_random": _python_random,
    "School.reset": _school_reset,
    "School.run": _school_run,
    "School._roll_springs": _roll_springs,
    "School.colors": _colors,
    "Fizz.run": _fizz_run,
    "tables": _tables,
}


def _voice(speech_every, count, speech=0.55, quiet=0.05):
    """Frames of 1/30 s: words of speech_every // 2 frames with gaps, as a ribbon's shown."""
    return [[speech if i % speech_every < speech_every // 2 else quiet, 1 / 30]
            for i in range(count)]


def _wobble(layers, points, phase):
    return [[math.sin(0.31 * i + 0.9 * layer + phase) * (1.0 + layer) for i in range(points)]
            for layer in range(layers)]


_QUIET = [[0.02, 1 / 30]] * 45
_TALK = _voice(8, 90)
_LOUD = [[0.85, 1 / 30]] * 60
_WORK = [[1 / 30, 1.0]] * 15 + [[1 / 30, 1.0 + 0.7 * min(1.0, k / 9)] for k in range(1, 30)]
_RESIZED = [[0.3, 0.05], [0.0, 0.0], [0.7, 0.1], [0.4, -0.01], [0.1, 1 / 60]] * 6
_DRAWS = [[min(1.0, (k + 1) / 15), (k + 1) / 30 * 1.4] for k in range(len(_WORK))]

CASES = {
    "python_random": [[5, 40, 9], [11, 40, 9], [3, 40, 9], [0, 12, 1], [1, 12, 2], [7, 12, 3],
                      [5, 12, 16], [5, 12, 17], [11, 80, 10], [4294967295, 12, 9]],
    "School.reset": [[kind, width] for kind in _KINDS for width in (300, 150, 200, 260.5, 420)],
    "School.run": (
        [[kind, 300, _QUIET + _TALK + _LOUD, _WORK] for kind in _KINDS]
        + [[kind, 300, _TALK, []] for kind in _KINDS]
        + [[kind, 260, _RESIZED, _WORK[:10]] for kind in _KINDS]
        + [[kind, 180, _TALK[:40], _WORK[:20]] for kind in _KINDS]
    ),
    "School._roll_springs": [
        [_wobble(5, 48, 0.0), _wobble(5, 48, 1.3), 1 / 60],
        [_wobble(5, 48, 2.0), _wobble(5, 48, 0.4), 1 / 120],
        [[[0.0] * 48] * 5, _wobble(5, 48, 0.2), 0.05],
        [_wobble(5, 48, 0.7), [[0.0] * 48] * 5, 0.0],
    ],
    "School.colors": [
        [kind, theme, t, anim, alpha, light]
        for kind in _KINDS for theme in ("dark", "light")
        for t, anim, alpha, light in ((0.0, 0.0, 255, 0.0), (1.7, 3.0, 128.5, 0.5),
                                      (12.3, 7.77, 300, 1.0), (4.0, 2.2, -4, 0.3))
    ],
    "Fizz.run": [
        [300, _QUIET + _TALK + _LOUD, _WORK, _DRAWS],
        [300, _TALK, [], []],
        [300, [[0.015, 1 / 30]] * 45, [], []],
        [300, [[0.5, 1 / 30]] * 45, [], []],
        [200, _RESIZED, _WORK[:10], _DRAWS[:10]],
    ],
    "tables": [[]],
}
