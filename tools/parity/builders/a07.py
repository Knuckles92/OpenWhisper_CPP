"""Reference builders of the a07 recording overlay (fv_ui_overlay, M1.8, M2.12 and M6.4).

Each builder returns (overlay, (width, height), None): a standalone WaveformOverlay opened by
the pointer in one state. The overlay paints from time.monotonic(), so its module's clock is
pinned to NOW for the whole render, and each frame's moments are set to fixed values: when
listening, work and the stage label started, the state's animation time, and the particles'
clock (their hue). The voice ribbon is fed a fixed 2.2 s sequence of levels. Every timer is
stopped, so nothing advances between show and grab.

The widget names are "waveform_overlay/<scene>", so each scene's variants share a folder under
tests/reference/waveform_overlay/. The tests/ui_overlay screenshot tests build the same scenes.
Scenes marked TREE_ONLY play Python's random confetti, bubbles or particles: their pixels cannot
be matched, and only their .json (the tree) is kept.
"""

import types

#: The pinned time.monotonic(), in seconds.
NOW = 1000.0
#: The particles' clock: their hue is animation_time * 50 degrees.
HUE_TIME = 3.0
#: A short phrase as raw RMS levels, one per frame of 1/30 s; played three times it is 2.2 s,
#: the ribbon's whole history.
VOICE = (0.002, 0.004, 0.02, 0.06, 0.09, 0.05, 0.03, 0.008, 0.002, 0.03, 0.07,
         0.12, 0.08, 0.04, 0.015, 0.003, 0.002, 0.05, 0.1, 0.06, 0.02, 0.004)

TREE_ONLY = ("waveform_overlay/recording-classic-particles", "waveform_overlay/copied-confetti",
             "waveform_overlay/canceling-bubbles")


def _overlay(look="bars"):
    from ui_qt.overlays import waveform_overlay

    waveform_overlay.time = types.SimpleNamespace(monotonic=lambda: NOW)
    overlay = waveform_overlay.WaveformOverlay()
    overlay._refresh_language = lambda: None
    overlay.set_recording_look(look)
    overlay.style.animation_time = HUE_TIME
    return overlay


def _voice(overlay):
    for level in VOICE * 3:
        overlay._ribbon.hear(level)
        overlay._ribbon.advance(1 / 30)


def _still(overlay):
    overlay.timer.stop()
    overlay.hidden_timer.stop()
    overlay._caption_timer.stop()
    return overlay, (overlay.width(), overlay.height()), None


def _recording(look="bars"):
    overlay = _overlay(look)
    overlay.show_at_cursor(overlay.STATE_RECORDING)
    _voice(overlay)
    overlay._listen_started = NOW - 4.4
    return overlay


def recording_bars(variant):
    return _still(_recording())


def recording_bars_handsfree(variant):
    overlay = _recording()
    overlay.set_hands_free(True)
    return _still(overlay)


def recording_bars_caption(variant):
    overlay = _recording()
    overlay.show_caption("Switched to Laptop mic")
    overlay._caption_started = NOW - 1.0
    return _still(overlay)


def processing_bars(variant):
    overlay = _recording()
    overlay.set_state(overlay.STATE_PROCESSING)
    overlay._work_started = NOW - 0.6
    overlay._work_clock = 0.6
    overlay.animation_time = 0.6
    return _still(overlay)


def transcribing_bars(variant):
    overlay = _recording()
    overlay.set_state(overlay.STATE_PROCESSING)
    overlay.set_state(overlay.STATE_TRANSCRIBING)
    overlay._work_started = NOW - 1.4
    overlay._label_changed = NOW - 0.8
    overlay._work_clock = 1.4
    overlay.animation_time = 0.8
    return _still(overlay)


def _shown(state, at, look="bars"):
    overlay = _overlay(look)
    overlay.show_at_cursor(state)
    overlay.animation_time = at
    return overlay


def cleaning(variant):
    return _still(_shown("cleaning", 1.0))


def canceling(variant):
    return _still(_shown("canceling", 0.10))


def stt_enable(variant):
    return _still(_shown("stt_enable", 0.12))


def copied(variant):
    return _still(_shown("copied", 0.12))


def notice_problem(variant):
    overlay = _overlay()
    overlay.show_notice("Transcription failed. Retry the recording from History.")
    overlay.animation_time = 0.6
    return _still(overlay)


def recording_classic(variant):
    """Classic before its first tick: no particle yet, so its pixels are fixed."""
    overlay = _overlay("legacy")
    overlay.show_at_cursor(overlay.STATE_RECORDING)
    return _still(overlay)


def recording_classic_particles(variant):
    overlay = _overlay("legacy")
    overlay.show_at_cursor(overlay.STATE_RECORDING)
    overlay.update_audio_levels([0.06] * 20)
    for _ in range(15):
        overlay.last_frame_time = NOW - 1 / 30
        overlay._update_animation()
    return _still(overlay)


def _burst(state, at):
    overlay = _shown(state, 0.0)
    overlay.animation_time = at
    overlay._update_animation()
    return _still(overlay)


def copied_confetti(variant):
    return _burst("copied", 0.6)


def canceling_bubbles(variant):
    return _burst("canceling", 0.4)


#: The live preview's words in the streaming scenes, at the default 16 pt.
WORDS = "Hello from FioriVoce, this is the live preview"
#: The other looks are drawn LOOK_T seconds into the recording, each frame of the fixed voice
#: run through the look's own motion as the overlay's frame timer would run it.
LOOK_T = 1.2


def _recording_look(look):
    overlay = _overlay(look)
    overlay.show_at_cursor(overlay.STATE_RECORDING)
    frames = round(LOOK_T * 30)
    for level in (VOICE * 2)[:frames]:
        overlay._ribbon.hear(level)
        overlay._ribbon.advance(1 / 30)
        overlay._visual.advance(1 / 30)
    overlay._listen_started = NOW - LOOK_T
    return overlay


def _look_builder(look):
    return lambda variant: _still(_recording_look(look))


def _streaming(layout, chip=False):
    overlay = _overlay()
    overlay._streaming_layout = layout
    overlay._streaming_font_size = 16
    overlay.show_at_cursor(overlay.STATE_STREAMING)
    _voice(overlay)
    overlay._listen_started = NOW - 4.4
    if chip:
        overlay.set_language("en", ["en", "es"])
    overlay.update_streaming_text(WORDS)
    return overlay


def streaming_header(variant):
    return _still(_streaming("header"))


def streaming_footer(variant):
    return _still(_streaming("footer"))


def streaming_header_chip(variant):
    return _still(_streaming("header", chip=True))


def language_notice(variant):
    overlay = _overlay()
    overlay.set_language("en", ["en", "es"])
    overlay.show_language_notice()
    overlay.animation_time = 0.4
    return _still(overlay)


def large_file(variant):
    overlay = _overlay()
    overlay.set_large_file_info(25.4)
    overlay.show_at_cursor(overlay.STATE_LARGE_FILE_SPLITTING)
    overlay.animation_time = 0.7
    return _still(overlay)


#: A walkthrough's recording (M6.4): its badge, Snap and Done, the clock at 0:12.
WALKTHROUGH_LABEL = "Walkthrough · 2 shots"
WALKTHROUGH_T = 12.4


def _walkthrough(chip=False):
    """The overlay as a walkthrough records with it: the destination badge, then Snap and Done.

    The offscreen platform takes no overlay clicks, so the platform's answer is pinned to
    Windows' for the render, as the overlay's own tests pin it.
    """
    from ui_qt.overlays import waveform_overlay

    waveform_overlay._platform_takes_overlay_clicks = lambda: True
    overlay = _overlay()
    overlay._takes_clicks_quietly = True
    overlay.set_recording_destination(WALKTHROUGH_LABEL)
    overlay.set_walkthrough_controls(True)
    overlay.show_at_cursor(overlay.STATE_RECORDING)
    _voice(overlay)
    overlay._listen_started = NOW - WALKTHROUGH_T
    if chip:
        overlay.set_language("en", ["en", "es"])
    return overlay


def walkthrough_recording(variant):
    return _still(_walkthrough())


def walkthrough_chip(variant):
    """Beside the language chip there is no room for the words: Snap and Done are icons."""
    return _still(_walkthrough(chip=True))


#: The Settings preview: its own width, the sample voice PREVIEW_T seconds into the loop.
PREVIEW_WIDTH = 560
PREVIEW_T = 2.0


def recording_look_preview_bars(variant):
    """The preview with no microphone (the toggle hidden), shown and then held still.

    Shown here, before the harness shows it, so the entry the preview replays on show happens
    first; its own timer and the overlay's are then stopped, and the sample voice is fed frame
    by frame up to PREVIEW_T.
    """
    from ui_qt.overlays import waveform_overlay
    from ui_qt.widgets.recording_look_preview import RecordingLookPreview, sample_level

    waveform_overlay.time = types.SimpleNamespace(monotonic=lambda: NOW)
    preview = RecordingLookPreview()
    overlay = preview.overlay
    overlay._refresh_language = lambda: None
    overlay.set_recording_look("bars")
    overlay.style.animation_time = HUE_TIME
    size = (PREVIEW_WIDTH, preview.sizeHint().height())
    preview.setFixedSize(*size)
    preview.show()
    preview._timer.stop()
    _still(overlay)
    for frame in range(round(PREVIEW_T * 30)):
        overlay._ribbon.hear(sample_level(frame / 30))
        overlay._ribbon.advance(1 / 30)
    overlay._listen_started = NOW - PREVIEW_T
    return preview, size, None


BUILDERS = {
    "waveform_overlay/recording-bars": recording_bars,
    "waveform_overlay/recording-bars-handsfree": recording_bars_handsfree,
    "waveform_overlay/recording-bars-caption": recording_bars_caption,
    "waveform_overlay/processing-bars": processing_bars,
    "waveform_overlay/transcribing-bars": transcribing_bars,
    "waveform_overlay/cleaning": cleaning,
    "waveform_overlay/canceling": canceling,
    "waveform_overlay/stt-enable": stt_enable,
    "waveform_overlay/copied": copied,
    "waveform_overlay/notice-problem": notice_problem,
    "waveform_overlay/recording-classic": recording_classic,
    "waveform_overlay/recording-classic-particles": recording_classic_particles,
    "waveform_overlay/copied-confetti": copied_confetti,
    "waveform_overlay/canceling-bubbles": canceling_bubbles,
    "waveform_overlay/recording-ribbon": _look_builder("ribbon"),
    "waveform_overlay/recording-wave": _look_builder("wave"),
    "waveform_overlay/recording-aurora": _look_builder("aurora"),
    "waveform_overlay/recording-pulse": _look_builder("pulse"),
    "waveform_overlay/recording-dots": _look_builder("dots"),
    "waveform_overlay/streaming-header": streaming_header,
    "waveform_overlay/streaming-footer": streaming_footer,
    "waveform_overlay/streaming-header-chip": streaming_header_chip,
    "waveform_overlay/language-notice": language_notice,
    "waveform_overlay/large-file": large_file,
    "waveform_overlay/walkthrough-recording": walkthrough_recording,
    "waveform_overlay/walkthrough-chip": walkthrough_chip,
    "recording_look_preview/bars": recording_look_preview_bars,
}
