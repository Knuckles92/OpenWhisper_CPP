"""Curated inputs for services/text_rewrite.py (fv_llm llm/rewrite.h).

compose_rewrite_prompt runs with its keyword arguments in order (instruction, has_selection,
dictionary_block, context_block). rewrite_with runs on a stand-in cleaner with TranscriptCleanup's
contract (the input back plus last_error on failure, optionally an attempt_timeout_s): the vector
is what rewrite_with returns and every cleanup call's (text, timeout_s, deadline_s); the prompt it
sends is compose_rewrite_prompt's vector. The C++ replay is tests/llm/test_rewrite.cpp.
"""


class _Cleaner:
    def __init__(self, reply, error, attempt_timeout_s):
        self.reply, self.error = reply, error
        self.last_error = "not run"
        self.calls = []
        if attempt_timeout_s is not None:
            self.attempt_timeout_s = attempt_timeout_s

    def cleanup(self, text, system_prompt=None, timeout_s=None, deadline_s=None):
        self.calls.append([text, timeout_s, deadline_s])
        self.last_error = self.error
        return text if self.error else self.reply


def _compose(module, instruction, has_selection, dictionary_block, context_block):
    return module.compose_rewrite_prompt(instruction, has_selection=has_selection,
                                         dictionary_block=dictionary_block,
                                         context_block=context_block)


def _rewrite_with(module, text, instruction, reply, error, attempt_timeout_s):
    cleaner = _Cleaner(reply, error, attempt_timeout_s)
    result = module.rewrite_with(cleaner, text, instruction)
    return {"result": list(result), "calls": cleaner.calls}


WRAPPERS = {"compose_rewrite_prompt": _compose, "rewrite_with": _rewrite_with}

SENTINEL = "<<NEEDS_SELECTION>>"

CASES = {
    "compose_rewrite_prompt": [
        ["  Make it friendlier  ", True, "Spell these: Acme", "<context>before</context>"],
        ["Make it friendlier", True, "", ""],
        ["shorter", True, "", "<context>x</context>"],
        ["write a thank-you note", False, "", ""],
        ["write a thank-you note", False, "Spell these: Acme", "<context>y</context>"],
        ["\tfix\n the grammar \n", True, "Terms", ""],
        ["", True, "", ""],
        ["é", False, "", "ctx"],
    ],
    "_describe_error": [
        ["canceled"], ["cleanup unavailable"], ["empty response"], ["timed out after 9 s"],
        ["timed out"], ["Error code: 401 - invalid key\nmore detail"], ["x" * 500],
        ["  \n  "], [""], ["  first\r\nsecond"], ["a b"], ["a\x85b"], ["a\x0bb"],
        ["a\x1cb"], ["é" * 119], ["é" * 120], ["é" * 121], [("word " * 30).strip()],
        ["y" * 118 + "  z" * 5], ["Canceled"], ["timed outward"],
    ],
    "rewrite_with": [
        ["hello there", "add an exclamation mark", " Hello there! \n", None, None],
        ["  \n", "write a thank-you", "Thanks so much!", None, None],
        ["", "make this shorter", SENTINEL, None, None],
        ["", "make this shorter", f" `{SENTINEL}` ", None, None],
        ["", "make this shorter", f'"{SENTINEL}"', None, None],
        [f"kept {SENTINEL}", "capitalise", f"Kept {SENTINEL} literally", None, None],
        ["the selection", "shorter", "", "canceled", None],
        ["the selection", "shorter", "", "cleanup unavailable", None],
        ["the selection", "shorter", "", "empty response", None],
        ["the selection", "shorter", "", "timed out after 9 s", None],
        ["the selection", "shorter", "", "Error code: 401 - invalid key\nmore detail", None],
        ["s", "i", "", "x" * 500, None],
        ["s", "i", "   ", None, None],
        ["s", "  ", "Rewritten.", None, None],
        ["x" * 20001, "shorter", "Rewritten.", None, None],
        ["x" * 20000, "shorter", "Rewritten.", None, None],
        [" " * 20001, "shorter", "Rewritten.", None, None],
        ["x" * 2001, "shorter", "Rewritten.", None, None],
        ["x" * 2000, "shorter", "Rewritten.", None, None],
        ["", "y" * 2001, "Rewritten.", None, None],
        ["", "write a thank-you", "Rewritten.", None, None],
        ["hello", "shorter", "Rewritten.", None, 120.0],
        ["hello", "shorter", "Rewritten.", None, 8.0],
        ["x" * 2001, "shorter", "Rewritten.", None, 45.0],
        ["é" * 2001, "shorter", "Rewritten.", None, None],
    ],
}
