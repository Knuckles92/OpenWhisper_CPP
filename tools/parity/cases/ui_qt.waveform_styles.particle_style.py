"""Curated inputs for ui_qt/waveform_styles/particle_style.py (fv_uimath particles).

Only what does not depend on Python's random stream is a vector: Particle.update trajectories,
the colour functions, and the processing vortex (its positions come from the clock and the arm
counter; only each dot's size is random). The recording drift and the transcribing converge draw
their launch points from the random stream, so the C++ tests check them statistically.

Particle.update runs steps [dt, gravity, damping] on a particle made with (x, y, vx, vy) and the
given life and reports [alive, x, y, vx, vy, life] after each. The colour functions take the
theme name first and return [r, g, b, a]. ParticleStyle.vortex runs ticks [state, dt] on a style
and reports the clock and the particle count after each tick, then every particle's [x, y, vx, vy,
life, hue].
"""

FLOAT_TOLERANCE = 1e-9


def _particle(module, x, y, vx, vy, life, hue):
    particle = module.Particle(x, y, vx, vy, hue=hue)
    particle.life = life
    return particle


def _update(module, x, y, vx, vy, life, steps):
    particle = _particle(module, x, y, vx, vy, life, 0.0)
    out = []
    for dt, gravity, damping in steps:
        alive = particle.update(dt, gravity, damping)
        out.append([alive, particle.x, particle.y, particle.vx, particle.vy, particle.life])
    return out


def _theme(theme):
    from ui_qt.utils.palette import palette_for, set_current_palette

    set_current_palette(palette_for(theme))


def _qcolor(module, theme, hue, life, base_hue=None):
    _theme(theme)
    return list(_particle(module, 0.0, 0.0, 0.0, 0.0, life, hue).get_qcolor(base_hue).getRgb())


def _fading(module, theme, hue, life):
    _theme(theme)
    return list(_particle(module, 0.0, 0.0, 0.0, 0.0, life, hue).get_fading_color().getRgb())


def _vortex(module, width, height, config, ticks):
    style = module.ParticleStyle(width, height, config)
    clock = []
    for state, dt in ticks:
        style.advance(state, dt)
        clock.append([style.animation_time, len(style.particles)])
    return {
        "ticks": clock,
        "particles": [[p.x, p.y, p.vx, p.vy, p.life, p.color_hue] for p in style.particles],
    }


WRAPPERS = {
    "Particle.update": _update,
    "Particle.get_qcolor": _qcolor,
    "Particle.get_fading_color": _fading,
    "ParticleStyle.vortex": _vortex,
}

_FRAME = [1 / 30, 0.0, 0.99]
_OVERLAY = {"max_particles": 150, "emission_rate": 30, "gravity": 20, "damping": 0.98,
            "wind_strength": 5, "audio_response": 1.5, "glow_effect": True,
            "turbulence_strength": 10, "color_shift_speed": 50}

CASES = {
    "Particle.update": [
        [10.0, 20.0, 30.0, -40.0, 1.0, [_FRAME] * 90],
        [0.0, 0.0, 0.0, 0.0, 1.0, [[1 / 60, 20.0, 0.98]] * 150],
        [100.0, 50.0, -120.5, 80.25, 1.0, [[1 / 120, 0.0, 0.95]] * 300],
        [5.0, 5.0, 100.0, 0.0, 0.3, [[0.0, 0.0, 0.5], [0.01, 0.0, 1.0], [0.5, 100.0, 0.9],
                                      [0.5, 0.0, 0.92], [0.5, 0.0, 0.92]]],
        [1.0, 1.0, 10.0, 10.0, 1.0, [[0.1, 0.0, 0.93]] * 25],
        [1.0, 1.0, 10.0, 10.0, 1.0, [[1 / 30, -5.0, 0.0], [1 / 30, 5.0, 2.0], [0.0, 0.0, 0.5]]],
        [1.0, 2.0, 3.0, 4.0, 0.0, [[1 / 30, 0.0, 0.99]]],
        [1.0, 2.0, 3.0, 4.0, 0.01, [[0.02, 0.0, 0.99], [0.0, 0.0, 0.99]]],
    ],
    "Particle.get_qcolor": (
        [[theme, float(hue), life] for theme in ("dark", "light") for hue in range(0, 360, 11)
         for life in (0.02, 0.3, 0.5, 0.77, 1.0)]
        + [[theme, 12.0, 0.6, 250.5] for theme in ("dark", "light")]
        + [["dark", 359.99, 0.5], ["dark", -10.0, 0.5], ["light", 725.0, 0.5], ["dark", 0.0, 0.004],
           ["light", 0.0, 0.004], ["light", 100.0, 0.001], ["dark", 100.0, 1.0]]
    ),
    "Particle.get_fading_color": (
        [[theme, float(hue), life] for theme in ("dark", "light") for hue in range(0, 360, 17)
         for life in (0.01, 0.33, 0.5, 0.9, 1.0)]
        + [["dark", -5.0, 0.5], ["light", 400.5, 0.25]]
    ),
    "ParticleStyle.vortex": [
        [320, 100, _OVERLAY, [["processing", 1 / 30]] * 90],
        [320, 100, _OVERLAY, [["processing", 1 / 60]] * 200],
        [300, 80, {}, [["processing", 1 / 30]] * 45],
        [300, 80, {"max_particles": 40}, [["processing", 0.0331]] * 80],
        [300, 80, _OVERLAY, [["processing", 5.0], ["processing", 0.5], ["processing", -1.0],
                             ["processing", 1 / 30]]],
        [300, 80, _OVERLAY, [["processing", 0.2]] + [["idle", 1 / 30]] * 4 + [["processing", 1 / 30]] * 4
                            + [["cleaning", 1 / 30]] + [["processing", 1 / 30]] * 6],
        [321, 101, _OVERLAY, [["processing", 0.0083]] * 400],
        [300, 80, {"max_particles": 0}, [["processing", 1 / 30]] * 30],
        [300, 80, _OVERLAY, [["idle", 1 / 30]] * 10],
    ],
}
