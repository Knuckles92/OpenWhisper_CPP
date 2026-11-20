"""Curated inputs for ui_qt/widgets/wrapped_label.py (fv_ui_widgets withBreakPoints)."""

ZWSP = "\u200b"
SHA256 = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"

CASES = {
    "_with_break_points": [
        [""], ["plain prose with spaces"], ["a"],
        # A run is cut every 8 characters only from 16 characters on.
        ["x" * 15], ["x" * 16], ["x" * 17], ["x" * 24], ["x" * 25], ["x" * 31], ["x" * 32],
        ["x" * 33], ["y" * 40],
        [SHA256], ["commit " + SHA256[:40] + " on main"],
        # Spaces, hyphens, slashes and backslashes end a run.
        ["x" * 15 + " " + "y" * 15], ["x" * 15 + "-" + "y" * 15], ["x" * 15 + "/" + "y" * 15],
        ["x" * 15 + "\\" + "y" * 15], ["x" * 16 + "-" + "y" * 16], ["x" * 16 + "/" + "y" * 16],
        ["parakeet-tdt-0.6b-v3-multilingual-q8_0-gguf"], ["a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q/r"],
        ["https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3/resolve/main/model.gguf"],
        # Backslash paths: a break point after every backslash, and the runs between them.
        ["C:\\Users\\someone\\Documents\\Recordings\\meeting-2026-11-14.wav"],
        ["C:\\Users\\someone\\AppData\\Local\\OpenWhisper_CPP\\speech-models\\parakeet-v3"],
        ["\\\\server\\share\\a_very_long_folder_name_without_breaks\\file.txt"],
        ["\\"], ["\\\\"], ["x" * 20 + "\\" + "y" * 20],
        # Existing break points count as separators.
        ["x" * 10 + ZWSP + "y" * 10], ["x" * 20 + ZWSP + "y" * 20],
        # Whitespace of any kind ends a run: \s is Unicode whitespace.
        ["x" * 15 + "\t" + "y" * 15], ["x" * 15 + "\n" + "y" * 15],
        ["x" * 15 + "\u00a0" + "y" * 15], ["x" * 15 + "\u3000" + "y" * 15],
        ["x" * 15 + "\x1c" + "y" * 15], ["x" * 15 + "\u2028" + "y" * 15],
        ["x" * 15 + "\u200a" + "y" * 15],
        # Code points, not UTF-16 units: an astral character counts once.
        ["\U0001F600" * 16], ["\U0001F600" * 17], ["\U0001F600" * 20], ["\U0001F600" * 15],
        ["a" + "\U0001F600" * 7 + "b" * 8], ["\U0001F680" * 8 + "ab" * 8 + "\U0001F680"],
        ["\u00e9" * 17], ["e\u0301" * 9], ["\u65e5\u672c\u8a9e" * 6],
        ["\u0661\u0662\u0663" * 6], ["\u2603" * 16 + " " + "\u2603" * 16],
        # Several runs in one text, and a run next to the end.
        ["x" * 16 + " " + "y" * 17 + " " + "z" * 3], ["short " + "w" * 40 + " tail"],
        ["x" * 16 + "." + "y" * 16], ["x" * 16 + "," + "y" * 16], ["x" * 16 + "_" + "y" * 16],
    ],
}
