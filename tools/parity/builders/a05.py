"""Reference builders of the a05 recording-and-audio widgets (fv_ui_record, M2.14).

The microphone check is built over a recorder that opens at once and never delivers audio, so the
render shows what the widget does with a level and nothing else: the level is applied through the
same slot the audio thread's queued delivery reaches. Nothing here runs an event loop.
"""

#: The level that fills the bar to 60 %: 0.004 + 0.6 ** 2 * (0.2 - 0.004).
HEARING_LEVEL = 0.004 + 0.36 * 0.196


class _FakeRecorder:
    last_start_error = ""

    def set_audio_level_callback(self, callback):
        self.callback = callback

    def start_recording(self):
        return True

    def cancel_recording(self):
        pass

    def cleanup(self):
        pass


def microphone_check_hearing(variant):
    from ui_qt.widgets.microphone_check import MicrophoneCheck

    check = MicrophoneCheck(_FakeRecorder)
    check.start()
    check._on_level(HEARING_LEVEL)
    return check, (360, 56), None


# render_reference.py writes one folder per builder name: tests/reference/microphone_check/hearing/.
BUILDERS = {
    "microphone_check/hearing": microphone_check_hearing,
}
