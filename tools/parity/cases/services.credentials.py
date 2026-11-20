"""Curated inputs for services/credentials.py (M0.7) and the python-dotenv parse it relies on."""

import os

import ow_isolation

NBSP = "\u00a0"

CASES = {
    "validate_api_key": [
        [""], ["  "], ["\n\t "], ["sk-abc"], ["  sk-abc\n"], ["sk-" + "x" * 509],
        ["sk-" + "x" * 510], ["x" * 512], ["x" * 513], [" " + "x" * 512 + " "],
        ["sk-\u201csmart\u201d"], ["sk-with space"], ["sk-tab\tinside"],
        [NBSP + "sk-abc" + NBSP], ["sk-" + NBSP + "abc"], ["sk-\u200babc"], ["sk-\u00e9"],
        ["sk-~!"], ["sk-\x7f"], [None],
    ],
    # The copy each rejection shows the user.
    "validate_api_key_error": [
        [""], ["  "], ["x" * 513], ["sk-\u201csmart\u201d"], ["sk-tab\tinside"], ["sk-abc"],
    ],
    "mask_key": [
        [None], [""], ["short"], ["x" * 11], ["abcdefgh1234"], ["sk-test-secret-value-1234abcd"],
        ["\u00e9" * 11], ["\u00e9" * 8 + "\u00fc\u00f6\u00e4\u00df"], ["sk-\U0001f600\U0001f600ab12cd"],
    ],
    "validate_credential_name": [
        [""], ["  "], [" K "], ["OPENAI_API_KEY"], [NBSP + "K" + NBSP], ["\tA B\n"], [None],
    ],
    # [file text, environment variables set while it is read]. Upper-case variable names only:
    # Windows' os.environ upper-cases names, which a case-sensitive replay would not.
    "dotenv_values": [
        ["export A=1\nB='single'\nC=\"double\"\n", {}],
        ["exportA=1\nexport  B=2\nexport\tC=3\n", {}],
        ['D="a\\nb\\tc\\"q\\\\ \\x"\nE=\'x\\\'y\\\\z\\n\'\n', {}],
        ['F="\\a\\b\\f\\r\\v"\n', {}],
        ["A=one\nB=${A}-two\nC=${FV_GOLDEN_SET}\nD=${FV_GOLDEN_UNSET:-fallback}\n"
         "E=${FV_GOLDEN_UNSET}\nF='${A}'\nG=\"${A}\"\nH=${A:-x}${B}\n",
         {"FV_GOLDEN_SET": "from-env"}],
        ["A=${FV_GOLDEN_SET:-unused}\nB=$A\nC=${A\nD=${}\nE=${:-dflt}\n",
         {"FV_GOLDEN_SET": "from-env"}],
        ["FV_GOLDEN_SET=file\nA=${FV_GOLDEN_SET}\n", {"FV_GOLDEN_SET": "from-env"}],
        ["NOVALUE\nA=${NOVALUE:-d}\n", {}],
        ["A=value # comment\nB=value#notcomment\nC=\"quoted\" # comment\n# full comment\n"
         "D= spaced value   \nE='q' #c\n", {}],
        ["A=\nB=''\nC=\"\"\nD\nE= \nF=#\n", {}],
        ["LONELY\nNEXT=1\n  INDENTED=2\n", {}],
        ["A=1\n'unterminated=2\nB=3\n", {}],
        ["C=\"open\nD=4\n", {}],
        ["=nokey\nX=1\nY = 2\nZ =\n", {}],
        ["A=1\r\nB=2\rC=3\r\n", {}],
        ["\ufeffA=1\n", {}],
        ["A=1\nB=2\nA=3\nC=${A}\n", {}],
        ["A=" + NBSP + "x" + NBSP + "\nB" + NBSP + "=1\n", {}],
        ["A\t=\tx\t# c\n", {}],
        ['A="line1\nline2"\nB=after\n', {}],
        ["A=1", {}],
        ["'QUOTED KEY'=v\n'K#2'=w\n", {}],
        ["A='a\"b'\nB=\"a'b\"\nC='trailing' junk\nD=x\n", {}],
        ["\n\n  \n# only comments\n", {}],
        ["", {}],
        ["K=\u00e9\u00e8 # \u00fc\n", {}],
        # Python's Unicode \s: NEL, ideographic space and the information separators.
        ["A=\u0085x\nB=y\u3000#c\nC=x\x1cy \x0b# c\nD=x\u2028y\n", {}],
        ['A="a\\u00e9"\nB="\\\\"\nC=a\\\\b\nD=\'a\\nb\'\n', {}],
        ["export\nexport A\n\tB=1\n  # comment\nC='multi\nline'\n", {}],
        ["A=${B}\nB=2\nC=${FV_GOLDEN_SET}${FV_GOLDEN_SET:-z}\n\u00c9=1\n",
         {"FV_GOLDEN_SET": "from-env"}],
        ["A=1\n\r\nB=2\r\r\nC=3", {}],
        ["A=x\0y\n", {}],
    ],
}


def _validate_api_key_error(module, raw):
    try:
        module.validate_api_key(raw)
    except ValueError as exc:
        return str(exc)
    return None


def _dotenv_values(module, text, environment):
    from dotenv import dotenv_values

    folder = ow_isolation.SCRATCH / "dotenv-cases"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / ".env"
    path.write_bytes(text.encode("utf-8"))
    previous = {name: os.environ.get(name) for name in environment}
    os.environ.update(environment)
    try:
        return [[name, value] for name, value in dotenv_values(path).items()]
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value
        path.unlink()


WRAPPERS = {
    "validate_api_key_error": _validate_api_key_error,
    "dotenv_values": _dotenv_values,
}

PINS = {
    "dotenv_note": "dotenv_values runs python-dotenv on the text written as UTF-8 bytes to a "
                   "scratch file, with the given variables set; the result is [name, value] "
                   "pairs in dict order",
}
