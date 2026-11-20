# webui: the meeting dashboard

The React page that `openwhisper-cpp.exe` serves on 127.0.0.1 during and after a meeting (and to guests
on the local network). It is OpenWhisper's dashboard, vendored; `SOURCE` names the commit and
what differs. OpenWhisper_CPP's server (`src/meeting_web`) serves `dist/index.html` and
`dist/assets/*` only.

| Path | What it is |
|---|---|
| `dist/` | The built bundle that is served. Committed, as in OpenWhisper. No Node runs at build time or at run time. |
| `dist.sha256` | `<sha256>  <path relative to dist/>` for every file under `dist/`, sorted. The meeting_web tests fail when `dist/` drifts from it. |
| `src/`, `index.html`, `public/`, `package*.json`, `vite.config.ts`, `tsconfig.json` | The source the bundle is built from. |
| `tests/*.cjs` | OpenWhisper's 22 node tests of the page. Not run by CTest. |
| `scripts/*.cjs` | Two browser checks (layout, pulse playback) that need Playwright. Not run by CTest. |
| `.gitattributes` | Turns line-ending conversion off here: the files are served and hashed byte for byte, as OpenWhisper committed them (LF). |
| `.gitignore` | Ignores `node_modules/` from the owner's `npm ci`; `rebrand.py` skips it as well. |

## Product name

The built page and the source carry the product name in a few places (page title, header brand,
three browser storage keys, a help text naming the log file and its folder). They read
OpenWhisper_CPP: `tools/webui/rebrand.py` applies a fixed map of exact literals to the source and to
`dist/`. See `tools/webui/README.md` for the commands.

## Optional owner steps (they download packages, so no tool or worker runs them)

Run the page's own tests:

```powershell
cd webui
npm ci
node --test tests/*.cjs
```

Rebuild the bundle from the source here (the source already says OpenWhisper_CPP, so the dist rules of
`tools/webui/rebrand_map.json` no longer apply):

```powershell
cd webui
npm ci
npm run build
cd ..
# delete the entries whose "file" starts with "dist/" from tools/webui/rebrand_map.json
python -I -B tools/webui/rebrand.py --manifest
python -I -B tools/webui/rebrand.py --check
```

A rebuild changes the content-hashed file names under `dist/assets/`; `dist/index.html` points at
the new ones, and the server reads them from there.
