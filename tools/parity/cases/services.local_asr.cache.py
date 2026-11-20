"""Curated inputs for services/local_asr/cache.py (M1.11).

is_cached runs over a model folder built in a scratch folder (local_app_dir patched) for
"parakeet-v3", whose pin is replaced by a small spec: weights.gguf of 14 bytes and notes.txt of 3.
A scenario gives installed.json's text (None: no file) and the files' sizes.
"""

import json
import shutil

import ow_isolation

_SCRATCH = ow_isolation.SCRATCH / "local-asr-cache-cases"

SPEC = {
    "repo": "test/model",
    "revision": "pinned",
    "files": [
        {"name": "weights.gguf", "url": "https://example.invalid/weights.gguf",
         "sha256": "ab" * 32, "size_bytes": 14},
        {"name": "notes.txt", "url": "https://example.invalid/notes.txt",
         "sha256": "cd" * 32, "size_bytes": 3},
    ],
}


def _is_cached(module, installed_json, sizes):
    shutil.rmtree(_SCRATCH, ignore_errors=True)
    folder = _SCRATCH / "speech-models" / "parakeet-v3"
    folder.mkdir(parents=True)
    saved = (module.local_app_dir, module.artifacts)
    module.local_app_dir = lambda: str(_SCRATCH)
    module.artifacts = lambda key: SPEC
    try:
        if installed_json is not None:
            (folder / "installed.json").write_text(installed_json, encoding="utf-8")
        for name, size in sizes.items():
            (folder / name).write_bytes(b"x" * size)
        return module.is_cached("parakeet-v3")
    finally:
        module.local_app_dir, module.artifacts = saved
        shutil.rmtree(_SCRATCH, ignore_errors=True)


_EXACT = json.dumps(SPEC)
_REORDERED = json.dumps({"files": SPEC["files"], "revision": "pinned", "repo": "test/model"})
_INDENTED = json.dumps(SPEC, indent=2)
_OTHER_SHA = json.dumps({**SPEC, "files": [{**SPEC["files"][0], "sha256": "ef" * 32},
                                           SPEC["files"][1]]})
_FLOAT_SIZE = _EXACT.replace('"size_bytes": 14', '"size_bytes": 14.0')
_FULL = {"weights.gguf": 14, "notes.txt": 3}

CASES = {
    "is_cached": [
        [None, _FULL],
        [_EXACT, _FULL],
        [_EXACT, {"weights.gguf": 14}],
        [_EXACT, {"weights.gguf": 13, "notes.txt": 3}],
        [_EXACT, {"weights.gguf": 15, "notes.txt": 3}],
        [_EXACT, {**_FULL, "extra.bin": 9}],
        [_REORDERED, _FULL],
        [_INDENTED, _FULL],
        [_OTHER_SHA, _FULL],
        [_FLOAT_SIZE, _FULL],
        ["", _FULL],
        ["{oops", _FULL],
        ["[]", _FULL],
        ["null", _FULL],
        [json.dumps({**SPEC, "extra": 1}), _FULL],
    ],
}

WRAPPERS = {"is_cached": _is_cached}

PINS = {
    "spec": SPEC,
    "scenario_note": "is_cached(installed.json text or None, {file name: size}) for parakeet-v3 "
                     "with the pin replaced by 'spec'",
}
