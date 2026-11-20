"""Curated inputs for ui_qt/widgets/recording_look_preview.py (fv_uimath sample_voice).

"_sample_words" is the word table random.Random(seed) lays out; C++ cannot re-roll it with
std::mt19937's own seeding, so src/uimath/data/sample_voice.json holds the seed-3 table
(tools/gen/overlay_tables.py) and this vector pins that file to Python's. "sample_level" is the
sample voice over t across several loops, around every word's edges, and at loop boundaries.
"tables" is the stage table and the loop length.
"""
FLOAT_TOLERANCE = 1e-12


def _word_edges():
    # The edge cases follow the word table, which tools/gen/overlay_tables.py dumped.
    import json
    from pathlib import Path

    table = Path(__file__).resolve().parents[3] / "src" / "uimath" / "data" / "sample_voice.json"
    edges = []
    for start, length, _peak in json.loads(table.read_text(encoding="utf-8"))["words"]:
        for t in (start - 1e-9, start, start + 1e-9, start + length / 2, start + length - 1e-9,
                  start + length, start + length + 1e-9):
            edges.append([t])
    return edges


def _tables(module):
    return {"stages": [list(s) for s in module.STAGES], "loop_s": module.LOOP_S,
            "word_count": len(module._WORDS)}


WRAPPERS = {"tables": _tables}

CASES = {
    "_sample_words": [[3]],
    "sample_level": (
        [[i * 0.05] for i in range(0, 200)]
        + [[-0.1], [-9.4], [-4.75], [9.4], [9.4 + 0.05], [18.8], [28.2 + 1.0], [1000.0], [0.0],
           [1e-9], [9.3999999], [4.9], [5.5], [2.5], [4.65]]
        + _word_edges()
    ),
    "tables": [[]],
}
