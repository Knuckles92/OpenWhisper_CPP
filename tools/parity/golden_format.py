"""The golden-vector file format shared by golden_vectors.py, trace_plugin.py and the C++ reader
(tests/support/golden).

    {"module": "services.format_utils", "source_commit": "<sha>",
     "pins": {...}, "functions": {"format_audio_duration": [
        {"args": [62.5], "result": "1m 2s"},
        {"args": ["abc"], "kwargs": {"x": 1}, "raises": "ValueError", "traced": true}]}}

Values are JSON scalars, lists and objects. Floats are written with repr(), which round-trips
exactly; NaN and infinities are tagged {"$float": "nan" | "inf" | "-inf"}, and anything JSON
cannot hold is {"$repr": "..."}, which a reader cannot turn back into a value. Files are UTF-8
with LF line ends, indent 1, so a regenerated file diffs line by line.
"""
from __future__ import annotations

import json
import math
from pathlib import Path


class Unencodable(ValueError):
    """A {"$repr": ...} value: recorded for the reader, never fed back into a call."""


def encode(value):
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        if math.isnan(value):
            return {"$float": "nan"}
        if math.isinf(value):
            return {"$float": "inf" if value > 0 else "-inf"}
        return value
    if isinstance(value, (list, tuple)):
        return [encode(v) for v in value]
    if isinstance(value, dict):
        return {str(k): encode(v) for k, v in value.items()}
    return {"$repr": repr(value)}


def decode(value):
    if isinstance(value, list):
        return [decode(v) for v in value]
    if isinstance(value, dict):
        if set(value) == {"$float"}:
            return float(value["$float"])
        if set(value) == {"$repr"}:
            raise Unencodable(value["$repr"])
        return {k: decode(v) for k, v in value.items()}
    return value


def dumps(record: dict) -> str:
    return json.dumps(record, indent=1, ensure_ascii=False) + "\n"


def write(path: Path, record: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(dumps(record).encode("utf-8"))
