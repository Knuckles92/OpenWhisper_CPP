"""Curated inputs for services/batch_upload.py (M2.3 upload-core, fv_dictation batch.h).

A request travels as {"paths": [...], "relation", "custom_instructions", "custom_combine"}; the
wrappers build BatchUploadRequest from it. Only the three relations the settings layer can store
are used: the C++ enum holds no other value.

    combine(request), source_names(request), batch_context(request, item index or None)
    source_name(path)
    result_times(items, total_elapsed_s, transcription_time_s, cleanup_time_s) where each item
        is {"duration", "elapsed", "cleanup_elapsed", "file_size"} -> the four BatchResult sums
    compose_batch_cleanup_prompt, join_raw_parts, format_batch_transcript (sections as pairs),
    batch_source_name(names[, limit])
"""

_PATHS = {
    "two": ["/audio/part0.mp3", "/audio/part1.mp3"],
    "three": ["/audio/part0.mp3", "/audio/part1.mp3", "/audio/part2.mp3"],
    "one": ["C:\\Users\\me\\Music\\interview.wav"],
    "none": [],
    "windows": ["C:\\rec\\a.m4a", "D:/b side.flac", "\\\\server\\share\\c.ogg", "relative.wav"],
    "braces": ["/x/{count}.wav", "/x/{names}.mp3", "/x/{name}.wav"],
    "unicode": ["/m/café 🎙.wav", "/m/日本語.mp3"],
}


def _request(module, spec):
    paths = spec["paths"]
    items = tuple(module.BatchItem(p) for p in paths)
    return module.BatchUploadRequest(
        items=items,
        relation=spec["relation"],
        custom_instructions=spec.get("custom_instructions", ""),
        custom_combine=spec.get("custom_combine", False),
    )


def _requests():
    specs = []
    for paths in _PATHS:
        for relation in ("separate", "sequential", "custom"):
            for instructions in ("", "   ", "  Two interviews with the same client.  ",
                                 "Voice memos.\nKeep the names.", "\u00a0Ünïcode\u2003"):
                for combine in (False, True):
                    if relation != "custom" and (instructions not in ("", "Voice memos.\nKeep "
                                                                       "the names.") or combine):
                        continue
                    specs.append({"paths": _PATHS[paths], "relation": relation,
                                  "custom_instructions": instructions, "custom_combine": combine})
    return specs


def _combine(module, spec):
    return _request(module, spec).combine


def _source_names(module, spec):
    return list(_request(module, spec).source_names)


def _batch_context(module, spec, index):
    request = _request(module, spec)
    return request.batch_context(None if index is None else request.items[index])


def _source_name(module, path):
    return module.BatchItem(path).source_name


def _result_times(module, items, total_elapsed, transcription_time, cleanup_time):
    request = module.BatchUploadRequest(items=())
    results = []
    for index, item in enumerate(items):
        results.append(module.BatchItemResult(
            item=module.BatchItem(f"/a/{index}.wav", item.get("duration")),
            elapsed_s=item.get("elapsed", 0.0),
            cleanup_elapsed_s=item.get("cleanup_elapsed", 0.0),
            file_size=item.get("file_size"),
        ))
    result = module.BatchResult(
        request=request, items=tuple(results), total_elapsed_s=total_elapsed,
        transcription_time_s=transcription_time, cleanup_time_s=cleanup_time,
    )
    return {
        "total_duration_seconds": result.total_duration_seconds,
        "total_file_size": result.total_file_size,
        "effective_transcription_time_s": result.effective_transcription_time_s,
        "effective_cleanup_time_s": result.effective_cleanup_time_s,
    }


WRAPPERS = {
    "combine": _combine,
    "source_names": _source_names,
    "batch_context": _batch_context,
    "source_name": _source_name,
    "result_times": _result_times,
}

_TENTHS = [{"duration": 0.1, "elapsed": 0.1, "cleanup_elapsed": 0.1, "file_size": 10}] * 10

CASES = {
    "combine": [[spec] for spec in _requests()],
    "source_names": [[{"paths": paths, "relation": "separate"}] for paths in _PATHS.values()],
    "batch_context": [
        [spec, index]
        for spec in _requests()
        for index in [None] + list(range(len(spec["paths"])))[:2]
    ],
    "source_name": [
        [p] for p in [
            "/audio/part0.mp3", "C:\\rec\\a.m4a", "D:/b side.flac", "C:", "C:name.wav",
            "\\\\server\\share\\c.ogg", "\\\\server\\share", "\\\\server", "//server/share/d.wav",
            "\\\\?\\UNC\\server\\share\\e.wav", "\\\\?\\C:\\f.wav", "\\\\.\\device", "dir\\",
            "relative.wav", "", "é:x.wav", "/m/café 🎙.wav", "a/b\\c", "\\root.wav",
        ]
    ],
    "result_times": [
        [[], 0.0, 0.0, 0.0],
        [[], 12.5, 0.0, 0.0],
        [[{"duration": 60.0, "elapsed": 3.0, "cleanup_elapsed": 1.5, "file_size": 1000},
          {"duration": None, "elapsed": 2.5, "cleanup_elapsed": 0.0, "file_size": None}],
         9.0, 0.0, 0.0],
        [[{"duration": 60.0, "elapsed": 3.0}], 9.0, 4.25, 2.0],
        [[{"duration": 1.0, "elapsed": 0.0}, {"duration": 2.0, "elapsed": 0.0}], 7.5, 0.0, 0.0],
        [[{"duration": 1.0, "elapsed": 0.0}], 0.0, -1.0, -2.0],
        [_TENTHS, 0.0, 0.0, 0.0],
        [[{"duration": 1e16, "elapsed": 1e16}, {"duration": 1.0, "elapsed": 1.0},
          {"duration": -1e16, "elapsed": -1e16}], 3.0, 0.0, 0.0],
        [[{"duration": 0.0, "file_size": 2**40}, {"file_size": 5}], 0.0, 0.0, 0.0],
    ],
    "compose_batch_cleanup_prompt": [
        ["BASE\n\nRules: 1. x", "CONTEXT"],
        ["", ""],
        ["Clean this.", "Two interviews.\nSame client."],
        ["{braces} stay", "{count}"],
    ],
    "join_raw_parts": [
        [["  one ", "", "   ", "two"]],
        [[]],
        [["", "  "]],
        [["a"]],
        [["\u00a0x\u2003", "\ty\n", "z"]],
        [["line one\nline two", " three "]],
    ],
    "format_batch_transcript": [
        [[["a.wav", " Hello. "], ["b.wav", "Bye."]]],
        [[]],
        [[["only.wav", ""]]],
        [[["x.mp3", "\n\n body \n"], ["y.mp3", "\u00a0z\u2003"]]],
    ],
    "batch_source_name": [
        [["a.mp3", "b.mp3"]],
        [[]],
        [["solo.wav"]],
        [[f"recording_{i:02d}.mp3" for i in range(20)], 60],
        [[f"recording_{i:02d}.mp3" for i in range(20)]],
        [["a_very_long_recording_name.mp3", "b.mp3"], 10],
        [["a_very_long_recording_name.mp3"], 10],
        [["a.mp3", "b.mp3", "c.mp3"], 0],
        [["a.mp3", "b.mp3", "c.mp3"], 26],
        [["a.mp3", "b.mp3", "c.mp3"], 27],
        [["a.mp3", "b.mp3", "c.mp3"], 32],
        [["café 🎙.wav", "日本語.mp3", "ü.wav"], 30],
        [["café 🎙.wav", "日本語.mp3", "ü.wav"], 31],
        [[f"f{i}.wav" for i in range(12)], 40],
        [[f"f{i}.wav" for i in range(120)], 120],
    ],
}

TRACE = {
    "tests": ["tests/test_batch_upload.py"],
    "functions": ["compose_batch_cleanup_prompt", "join_raw_parts", "format_batch_transcript",
                  "batch_source_name"],
}
