"""Curated inputs for ui_qt/utils/loudness.py (fv_uimath level_to_height)."""

FLOAT_TOLERANCE = 1e-12

CASES = {
    # 10 ** (dB / 20) for the floor (-60) and ceiling (-12), and either side of them.
    "level_to_height": [
        [0.0], [-0.5], [1e-9], [0.0005], [0.001], [0.00100001], [0.003], [0.01], [0.0316],
        [0.1], [0.251188643150958], [0.2511886], [0.26], [0.5], [1.0], [2.0], [1e300],
        [float("inf")], [float("nan")], [-float("inf")], [5e-324],
    ],
}
