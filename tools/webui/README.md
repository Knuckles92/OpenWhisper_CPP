# tools/webui

`rebrand.py` replaces the product name in the vendored dashboard (`webui/`) by exact text
substitution, so the page says FioriVoce without Node ever running. The replacements are in
`rebrand_map.json`: per file, an exact literal, its replacement and how many times the literal
occurs there. A run whose count differs from the map refuses before it writes anything (a minified
name that merely contains the product name must not be touched).

Run from the repository root:

```powershell
python -I -B tools/webui/rebrand.py                  # apply; a second run changes nothing
python -I -B tools/webui/rebrand.py --check          # nothing left, replacements in place, dist.sha256 matches
python -I -B tools/webui/rebrand.py --manifest       # rewrite webui/dist.sha256
python -I -B tools/webui/rebrand.py --report         # replacements per file
python -I -B tools/webui/rebrand.py --against-commit D:\coding\whisper_local\.git   # read-only: the tree is webui/ of the reference commit with exactly the map applied
python -I -B tools/webui/rebrand.py --against D:\coding\whisper_local\webui   # the same against a working copy (line-ending edits there show up)
python -I -B tools/webui/rebrand.py --self-test      # undo the map on a temporary copy, apply it, compare
python -I -B -m unittest discover -v -s tools/webui -p "test_rebrand.py"
```

`--check` also fails on any `open whisper`, `openwhisper` or `open_whisper` (any case) left in
`webui/`, except what the map's `allowed` list names with an exact count and a reason.

When the map changes (after a new OpenWhisper commit is vendored, say): copy the new files in,
run `rebrand.py`, then `--manifest`, then `--check`. Counts that differ are the signal that the
page changed where a literal lives.
