"""Writes tests/fixtures/workflow_smoke/fixture.wav: the WAV OpenWhisper's workflow smoke
(services/release_checks.py run_workflow_smoke) writes and decodes, made the same way, with
numpy and the standard wave module of OpenWhisper's interpreter.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tools/parity/check_isolation.py -- `
        $env:FIORIVOCE_OW_PYTHON -I -B tests/fixtures/workflow_smoke/make_fixture.py

No OpenWhisper module is imported; only fixture.wav in this folder is written. fiorivoce-cli's
smoke writes its own copy at run time (an installed build has no tests folder), and the cli
tests compare its bytes with this file.
"""
from __future__ import annotations

import wave
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent


def main() -> None:
    # release_checks.py:117-122, unchanged.
    samples = np.full(16000, 1000, dtype=np.int16)
    with wave.open(str(HERE / "fixture.wav"), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(samples.tobytes())


if __name__ == "__main__":
    main()
