"""Curated inputs for ui_qt/utils/release_notes.py (fv_uimath render_release_notes_html).

The style colours are resolved against the current palette, which is the dark one in the
generator process (nothing sets another); PINS records it, and the C++ replay passes the dark
tokens of src/ui_theme/data/palettes.json."""

PINS = {"palette": "dark"}

_HASH = "e4706b1d" * 8

CASES = {
    "render_release_notes_html": [
        [""], ["   \n\n"], [None],
        ["- First item\n- Second item\n"], ["1. First\n2. Second\n"],
        ["1) First\n2) Second\n"], ["* star\n+ plus\n- dash"],
        ["### Highlights\n\nSomething changed."], ["# Title #\n\n###### Six ######"],
        ["####### seven hashes"], ["#nospace"],
        ["An **Active** badge, `gemini-3.7`, and *emphasis*."],
        ["See [the notes](https://example.com/x)."], ["[no scheme](ftp://example.com)"],
        ["[a](http://x.y/a_b) and [b](https://x.y/c?d=1&e=2)"],
        [f"Real change here.\n\n### SHA-256\n\n```\n{_HASH}\n```\n"],
        [f"Update notes.\n\nOpenWhisper-Setup.exe {'ab' * 32}\n"],
        ["A change.\n\n**Full Changelog**: https://example.com/compare\n"],
        ["Use <script>alert(1)</script> carefully"],
        ["Run it:\n\n```\ngit pull --ff-only\n```\n"],
        ["One sentence\nwrapped over lines.\n"],
        ["Quotes 'single' and \"double\" & amps"],
        ["## Checksums\nabc\n### sub\nstill dropped\n## Next\nkept"],
        ["## Assets\n- one\n# Top\nback"], ["### Verifying the download\nx\n## Notes\ny"],
        ["### Downloads:\nlinks\n### What's new\nnew"],
        ["<!-- hidden -->\nvisible"], ["text\n---\nmore"], ["***"], ["___"],
        ["- item\n  continued here\n- next"], ["- item\n\tcontinued tab\n\n- after blank"],
        ["~~~\ncode with <tags>\n~~~"], ["```\n\n```"], ["```\nunclosed fence"],
        [f"```\n{_HASH}\nfile.zip {_HASH}\n```\nafter"],
        ["**bold** __not__ _it_ *a* * b *"], ["snake_case_name and 2*3*4"],
        ["`code **not bold**` **`bold code`**"], ["`a` `b` `a`"],
        ["Thanks @someone for @text-body"], ["line\r\nwindows\rmac"],
        ["x" * 5990 + "\n" + "y" * 50], ["x" * 7000],
        [("para " * 300 + "\n") * 6],
        ["ünïcödé **bölд** _кур_ 日本語 😀 *ok*"], ["Full changelog: dropped?"],
        ["**full changelog**: x"], ["sha256 " + _HASH + " extra words here"],
        ["0x" + "a" * 40], ["- " + _HASH],
    ],
}

TRACE = {
    "tests": ["tests/test_release_notes.py"],
    "functions": ["render_release_notes_html"],
}
