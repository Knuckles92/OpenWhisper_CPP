"""Curated inputs for services/format_utils.py (M1.13 history-core, seeded by M0.4)."""

CASES = {
    # round() is half to even: 62.5 -> 62, 63.5 -> 64, 125.5 -> 126, 124.5 -> 124; :.1f rounds
    # the binary value.
    "format_audio_duration": [
        [None], [0], [0.04], [0.05], [0.15], [0.25], [3.7], [59.94], [59.95], [60],
        [62.5], [63.5], [125], [3599.5], [3600], [3660], [7322], [-5], ["12.5"], ["x"],
        [125.5], [59.99], [3659.5], [86400], [90061], [-0.0], [float("-inf")],
    ],
    # The same function on inputs the plain list above must not hold: tests/support replays that
    # list through a minimal port (numbers and float() text only, no raises) and expects 62.5 to
    # be the only case that half away from zero gets wrong.
    "format_audio_duration_edge_cases": [
        [124.5], [True], [False], [" 61 "], ["1e2"], [float("nan")], [float("inf")],
        ["nan"], ["-inf"],
    ],
    "format_short_duration": [[0], [0.0005], [0.0315], [0.9994], [0.9996], [1], [61], [-1],
                              ["bad"], [None], [0.0025], [0.0015], [float("nan")], [125.5]],
    # :.0f on the binary quotient: 1500 -> "2 KB", 2500 -> "2 KB".
    "format_size_bytes": [[0], [999], [1000], [1499], [1500], [2500], [999_999],
                          [1_000_000], [1_500_000], [2_500_000], [999_999_999],
                          [1_000_000_000], [1_234_567_890], [-5], [1_005_000_000],
                          [12_345_678_901_234]],
    "format_sample_rate": [[0], [-1], [8000], [16000], [22050], [44100], [48000], [44150],
                           ["16000"], ["x"], [None], [44100.9], [True], [" 22050 "], [11025],
                           [1050]],
    "format_file_size": [[0], [1023], [1024], [1536], [1048575], [1048576],
                         [5 * 1024 ** 3], [1023.9], [-5], [1024 ** 4], [1101004],
                         [float("nan")]],
    # Naive values do not depend on the zone. Aware ones convert to the machine's zone
    # (pins.local_timezone, Pacific: America/Los_Angeles rules), across both daylight-saving
    # changes of 2026, and through the C runtime's localtime, which on Windows refuses moments
    # before 1970 and after 3001-01-19 (OSError: the text comes back unchanged).
    "format_timestamp": [["2026-03-04T05:06:07"], ["2026-12-31T23:59:59"],
                         ["2026-01-01T00:00:00"], ["2026-07-15T12:30:00"],
                         ["not a date"], [""],
                         ["2026-08-20T16:46:00+00:00"], ["2026-01-15T08:00:00Z"],
                         ["2026-03-08T09:59:00+00:00"], ["2026-03-08T10:00:00+00:00"],
                         ["2026-11-01T08:30:00+00:00"], ["2026-11-01T09:30:00+00:00"],
                         ["2026-08-20T16:46:00.123456+05:30"], ["2026-08-20T23:59:59.999999-07:00"],
                         ["1969-12-31T23:59:59+00:00"], ["1970-01-01T00:00:00+00:00"],
                         ["3001-01-19T07:59:59+00:00"], ["3001-01-19T08:00:00+00:00"],
                         ["9999-12-31T23:59:59+00:00"], ["0001-01-01T00:00:00+05:00"],
                         ["20260820T164600"], ["2026-08-20 16:46"], ["2026-08-20"],
                         ["0005-01-02T03:04:00"], ["2026-08-20T00:30:00"],
                         ["2026-08-20T12:00:00"], ["2026-02-30T10:00:00"], [" 2026-08-20"],
                         ["2026-W34-4T10:00"]],
}

WRAPPERS = {
    "format_audio_duration_edge_cases": lambda module, seconds: module.format_audio_duration(seconds),
}

TRACE = {
    "tests": ["tests/test_format_utils.py"],
    "functions": ["format_audio_duration", "format_timestamp"],
}

PINS = {
    "timezone_note": "format_timestamp converts aware values with astimezone(), so aware cases "
                     "hold the local_timezone of the machine that made them (Pacific Standard "
                     "Time: America/Los_Angeles rules); replay them with that zone passed in "
                     "explicitly",
}
