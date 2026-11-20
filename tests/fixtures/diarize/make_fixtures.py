"""Writes the speaker-embedding fixtures of tests/fixtures/diarize from OpenWhisper's own
tests/test_meeting_diarize.py::_synthetic_speakers, which seeds numpy's default_rng (PCG64), so
C++ cannot draw the same vectors itself.

    $env:FIORIVOCE_OW_ROOT = 'D:\\coding\\whisper_local'
    & $env:FIORIVOCE_OW_PYTHON -I -B tests/fixtures/diarize/make_fixtures.py

Each set is little-endian float32, one row of `dim` values per embedding, in the order the
function returns them. A call with fewer vectors and the same seed and speaker count returns a
prefix of a longer call, so one file serves every length the tests ask for. speakers.json lists
the sets. Writes nothing outside this folder (and the isolation scratch folder).
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools" / "parity"))
import ow_isolation  # noqa: E402

# name: (count, speakers, seed); dim 192 and sigma 0.035 are the function's defaults.
SETS = {
    "s4_seed7": (1200, 4, 7),
    "s3_seed7": (60, 3, 7),
    "s2_seed7": (40, 2, 7),
    "s2_seed11": (12, 2, 11),
    "s1_seed7": (20, 1, 7),
    "s1_seed99": (2, 1, 99),
}
DIM = 192
SIGMA = 0.035


def main() -> int:
    checkout = ow_isolation.checkout()
    ow_isolation.isolate()
    path = checkout / "tests" / "test_meeting_diarize.py"
    spec = importlib.util.spec_from_file_location("ow_test_meeting_diarize", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    import numpy as np

    manifest = {"source": "tests/test_meeting_diarize.py::_synthetic_speakers",
                "source_commit": ow_isolation.head_commit(checkout),
                "numpy": np.__version__, "dim": DIM, "sigma": SIGMA, "sets": {}}
    for name, (count, speakers, seed) in SETS.items():
        vectors = module._synthetic_speakers(count, dim=DIM, speakers=speakers, sigma=SIGMA,
                                             seed=seed)
        data = np.stack(vectors).astype("<f4")
        assert data.shape == (count, DIM)
        (HERE / f"{name}.f32").write_bytes(data.tobytes())
        manifest["sets"][name] = {"file": f"{name}.f32", "count": count, "speakers": speakers,
                                  "seed": seed}
    (HERE / "speakers.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
