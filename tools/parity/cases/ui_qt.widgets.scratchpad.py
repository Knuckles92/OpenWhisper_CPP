"""The text rules of ui_qt/widgets/scratchpad.py (M2.3 upload-core, fv_dictation
scratchpad_text.h); the window itself is M2.25's.

    _spaced(text, before, after)                       Python's argument order
    _utf16_slice(text, start, end)                     the text is encoded to UTF-16-LE first
    append_block(existing, text) -> what is inserted   ScratchpadWindow.append_block on a stub
    word_count_text(text) -> the status line           ScratchpadWindow._show_word_count on a stub

No QApplication is made: append_block runs on a stub editor with QTextCursor swapped, for the
call, for a stand-in that records the inserted text.
"""

_BEFORE = [
    "", "Hello", "Hello ", "Hello.", "(", "[", "{", "\"", "'", "“", "‘", "/", "-",
    "—", "\n", "\t", "end\n", "x–", " ", "word ", "\U0001f600", "1",
    "’", ")",
]
_TEXT = [
    "", "world", " world", "\nworld", "\tworld", ".", ",", ";", ":", "!", "?", ")", "]", "}",
    "%", "…", "'", "\"", "’", "”", "(aside)", "—dash", "word ", "word\n",
    "\U0001f600 ok", "ok \U0001f600", " nbsp", "été",
]
_AFTER = ["", "next", " next", ".", "\n", "9", "é", "_x", "\U0001f600", "(", "一",
          "١"]


def _spaced_cases():
    cases = []
    for b, before in enumerate(_BEFORE):
        for t, text in enumerate(_TEXT):
            after = _AFTER[(b * 7 + t * 3) % len(_AFTER)]
            cases.append([text, before, after])
    for after in _AFTER:
        for text in ("word", "word ", "word\n", "\U0001f600", "a "):
            cases.append([text, "x", after])
    return cases


def _utf16_slice(module, text, start, end):
    return module._utf16_slice(text.encode("utf-16-le"), start, end)


class _Recorder:
    def __init__(self):
        self.calls = []

    def __call__(self, *args):
        self.calls.append(args)


def _append_block(module, existing, text):
    inserted = []

    class _Cursor:
        MoveOperation = module.QTextCursor.MoveOperation

        def __init__(self, _document):
            pass

        def movePosition(self, _operation):
            return True

        def insertText(self, value):
            inserted.append(value)

    class _Editor:
        def toPlainText(self):
            return existing

        def document(self):
            return None

        def ensureCursorVisible(self):
            pass

    stub = type("Stub", (), {})()
    stub.editor = _Editor()
    original = module.QTextCursor
    module.QTextCursor = _Cursor
    try:
        module.ScratchpadWindow.append_block(stub, text)
    finally:
        module.QTextCursor = original
    return inserted[0]


def _word_count_text(module, text):
    class _Editor:
        def toPlainText(self):
            return text

    shown = []

    class _Label:
        def setText(self, value):
            shown.append(value)

    stub = type("Stub", (), {})()
    stub.editor = _Editor()
    stub.status_label = _Label()
    stub.word_count = lambda: module.ScratchpadWindow.word_count(stub)
    module.ScratchpadWindow._show_word_count(stub)
    return shown[-1]


WRAPPERS = {
    "_utf16_slice": _utf16_slice,
    "append_block": _append_block,
    "word_count_text": _word_count_text,
}

_SLICE_TEXT = ["", "hello world", "a\U0001f600b", "\U0001f600\U0001f600", "café 一",
               "x" * 1500]

CASES = {
    "_spaced": _spaced_cases(),
    "_utf16_slice": [
        [text, start, end]
        for text in _SLICE_TEXT
        for start, end in [(0, 0), (0, 3), (1, 3), (2, 3), (1, 2), (-5, 2), (0, 100), (3, 1),
                           (2, 4), (-1000, 1000), (500, 1700)]
    ],
    "append_block": [
        [existing, text]
        for existing in ["", "  ", "\n\n", "First.", "First.\n", "First.\n\n", "First.  ",
                         " ", "line\r"]
        for text in ["Second.", "  Second.  ", "", "\nSecond\n", " x "]
    ],
    "word_count_text": [
        [t] for t in [
            "", "   ", "one", "one two", " one  two\tthree\nfour ", "a b", "a b",
            "a​b", "x " * 999, "x " * 1000, "x " * 12345, "\U0001f600 \U0001f600",
            "a\x1cb\x1dc\x1ed\x1fe", "a\x85b",
        ]
    ],
}
