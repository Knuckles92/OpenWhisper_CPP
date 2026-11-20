"""Curated inputs for services/local_asr/nvidia.py segments_from_words (fv_engine
segments_from_words): the result dict NvidiaRecognizer._result builds, and the duration in
seconds. Covers the sentence ends after Python's rstrip (any Unicode whitespace), the 24-word
cut, and text without word timings.
"""


def _words(*items):
    return [{"text": text, "start": start, "end": end} for text, start, end in items]


def _run(count, text="w"):
    return [{"text": text, "start": float(i), "end": i + 0.5} for i in range(count)]


CASES = {
    "segments_from_words": [
        [{"text": "", "words": []}, 1.0],
        [{"text": "   ", "words": []}, 2.5],
        [{"text": "　 \n", "words": []}, 2.5],
        [{"text": "no timings here", "words": []}, 4.5],
        [{"text": " padded ", "words": []}, 0.0],
        [{"text": "ignored", "words": _words(("Hello", 0.0, 0.4), ("there.", 0.5, 0.9),
                                             ("How", 1.2, 1.4), ("are", 1.4, 1.5),
                                             ("you?", 1.5, 1.9), ("Fine", 2.5, 2.9))}, 3.0],
        [{"text": "", "words": _words(("Done! ", 0.0, 0.5), ("Next", 0.6, 1.0))}, 1.0],
        [{"text": "", "words": _words(("Stop. ", 0.0, 0.5), ("Go", 0.6, 1.0))}, 1.0],
        [{"text": "", "words": _words(("Wait?　", 0.0, 0.5), ("Then", 0.6, 1.0))}, 1.0],
        [{"text": "", "words": _words(("Odd!\x1c", 0.0, 0.5), ("Next ", 0.6, 1.0))}, 1.0],
        [{"text": "", "words": _words(("Not​.", 0.0, 0.5), ("quite.​", 0.6, 1.0),
                                       ("end", 1.1, 1.3))}, 1.5],
        [{"text": "", "words": _words(("a.b", 0.0, 0.5), ("c", 0.6, 1.0))}, 1.0],
        [{"text": "", "words": _words(("...", 0.0, 0.5), ("", 0.6, 0.6), ("x", 0.7, 1.0))}, 1.0],
        [{"text": "", "words": _words(("Ça?", 0.0, 0.5), ("niño.", 0.6, 1.0),
                                       ("日本語。", 1.1, 1.5))}, 2.0],
        [{"text": "", "words": _run(24)}, 30.0],
        [{"text": "", "words": _run(25)}, 30.0],
        [{"text": "", "words": _run(48)}, 60.0],
        [{"text": "", "words": _run(23) + _words(("end.", 23.0, 23.5), ("more", 24.0, 24.5))},
         30.0],
    ],
}
