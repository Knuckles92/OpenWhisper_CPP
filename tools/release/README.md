# Release tools

Python 3.12, standard library only. They build, check and serve a release candidate on this
machine; none of them uploads, publishes or contacts a release service. The tests are
`tools/release/tests` (CTest `release.tools`, label `release`):

```powershell
ctest --preset windows-ci -L release
python -I -B -m unittest discover -s tools/release/tests -p "test_*.py"
```

Every tool reads the release identity, `src/update/data/release_identity.json`, for the setup
name, the executable names and the timestamp server. `--identity <file>` (or the environment
variable `FIORIVOCE_RELEASE_IDENTITY`) names another copy; the tests use
`tests/fixtures/release_identity.json`. Keys a tool does not know are ignored.

| Tool | What it does |
|---|---|
| `windows_signing.py` | `sign`, `verify`, `metadata`, `inno-args`: optional Authenticode signing and the signing record |
| `build_metadata.py` | Writes `BUILD-METADATA.json` (schema 1) and `SHA256SUMS.txt` for a candidate folder, then checks them |
| `check_release_qualification.py` | Checks a candidate folder: digests, metadata, exact inventory, signing record |
| `generate_notices.py` | `THIRD_PARTY_NOTICES.md` and `licenses/` for the install tree |
| `pin_component.py` | The catalog entry (URL, SHA-256, size, extracted bytes) for a runtime archive |
| `serve_update_feed.py` | A loopback stand-in for GitHub's release feed, to soak an update |
| `check_release_health.py` | Checks a --workflow-smoke report against the release health gate (deadlines, data-safety checks, --baseline regressions) |
| `release_common.py` | The identity file and the file names derived from it (not a command) |

## Signing (`windows_signing.py`)

Nothing configured means unsigned, and every command says so. Configure exactly one method:

| Method | Variables |
|---|---|
| Certificate in the Windows store | `FIORIVOCE_SIGN_CERT_SHA1` (40-digit thumbprint) |
| PFX file (local builds) | `FIORIVOCE_SIGN_PFX` and `FIORIVOCE_SIGN_PASS` |
| Azure Trusted Signing | `FIORIVOCE_SIGN_DLIB` and `FIORIVOCE_SIGN_METADATA` (credentials from `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`) |

A partial or conflicting configuration is an error. `FIORIVOCE_SIGN_TIMESTAMP_URL` overrides the
RFC 3161 server (default: the identity's `timestamp_url`; Trusted Signing uses Microsoft's).
`FIORIVOCE_SIGNTOOL` names `signtool.exe`; otherwise it is found on `PATH`, then as the newest
`Windows Kits\10\bin\<version>\x64\signtool.exe`. Every signature uses SHA-256 and a timestamp.

```
windows_signing.py sign FILE...                      # signtool sign; unsigned: prints that and exits 0
windows_signing.py inno-args                         # /DSignToolName=... /Sfiorivoce=... ($ escaped); nothing when unsigned
windows_signing.py verify --report R FILE...         # writes windows-signing.json; unsigned passes only as "unsigned"
windows_signing.py metadata --report R --release DIR # BUILD-METADATA signing fields, bound to the setup exe's digest
```

`verify` fails when signing is configured and a file is unsigned, invalid, untimestamped or
signed by another certificate, and when nothing is configured and a file carries a signature.

## Release candidate

```
build_metadata.py --dir out/installer --version 0.1.0 --commit <sha> \
    [--signing out/installer/windows-signing.json] [--source-qualification passed] \
    [--ref R --repository O/N --run-id N --run-attempt N --run-url U] [--built-at T]
check_release_qualification.py --dir out/installer [--require-windows-signed] \
    [--require-source-qualification] [--expected-commit SHA] [--expected-run N] \
    [--draft-assets saved.json [--uploaded]]
```

A candidate folder holds `<Product>-Setup-X.Y.Z.exe` (and, once there is one,
`<Product>-X.Y.Z-win64.zip`), `BUILD-METADATA.json`, `SHA256SUMS.txt` and optionally
`windows-signing.json`. Nothing else may be in it. `BUILD-METADATA.json` is schema 1:
`source_qualification` (`passed` or `not-run`), `windows_setup_only`, `windows_signed`,
`windows_signing` (method, signer subject and thumbprint, when signed), `version`, `commit_sha`,
`built_at_utc`, and the run fields when given. `SHA256SUMS.txt` has one
`<64 hex>  <name>` line per file, two spaces, LF line endings, sorted by name; the update check's
parser reads that format too. Without `--signing` the build is recorded as unsigned.

`--draft-assets` validates a saved `{"assets": [{"name", "digest"}]}` document (what a release
service lists) against the candidate, before an upload or after one (`--uploaded`); the script
never fetches it.

## Release health (`check_release_health.py`)

```
check_release_health.py out/smoke.json [--baseline previous-release-health.json]
check_release_health.py --executable <stage>/fiorivoce-cli.exe --output out/release-health.json \
    [--baseline previous-release-health.json] [--timeout 45]
```

Checks the report of `fiorivoce-cli --workflow-smoke` (or runs the smoke into `--output` first,
under a process exit deadline): every data-safety check passed, every metric present, finite and
within its deadline. `--baseline` rejects timing or RSS regressions against an accepted report
from equivalent hardware. An unreadable report or baseline fails with a message.

## Notices (`generate_notices.py`)

```
generate_notices.py --vcpkg-installed <build>/vcpkg_installed --triplet x64-windows \
    --qt <Qt6_ROOT> --out <stage> [--require-qt-licenses]
```

Lists every port in `vcpkg_installed/vcpkg/info` for the triplet with the text of
`share/<port>/copyright`, copies the Qt kit's licence files (looked for in the kit, its parents'
`Licenses` folders and `share/qt6/licenses`), and adds the hand-written fragments in `notices/`
in file-name order (`04-dashboard.md` covers what the staged `webui\dist` bundles: React, React
DOM and Scheduler under MIT, and the DM Sans and Instrument Serif fonts under OFL-1.1, whose texts
ship in `webui\dist\licenses`; `test_generate_notices.py` checks it against
`webui/package-lock.json`, so a new runtime package of the dashboard fails until it has a notice). A port that installs code but has no `copyright` file fails; a port that
installs none (vcpkg's `boost-uninstall`) is listed without one. A Qt kit without licence files is
a warning (the Qt kit on this machine, built from source, has none), or an error with
`--require-qt-licenses`: pass it for a build that ships.

## Component pins (`pin_component.py`)

```
pin_component.py --url https://github.com/.../nemo-speech-0.1.0-windows-x86_64-cpu.zip
pin_component.py --file nemo-speech.zip [--published-url URL] [--extract zip|nemo-tar|none]
```

Prints `{"install_bytes": ..., "archives": [{"name", "url", "sha256", "size_bytes", "extract",
"root"}]}` for the runtime catalog. The archive is downloaded to a temporary folder (HTTPS only,
redirects included; loopback HTTP for tests, never through a proxy) and deleted; nothing is
extracted, member sizes are summed, and a member that escapes its folder is an error.

## Update feed (`serve_update_feed.py`)

```
serve_update_feed.py --setup out/installer/FioriVoce-Setup-0.1.1.exe --version 0.1.1 [--archive out/update/FioriVoce-0.1.1-win64.zip] [--prerelease] [--port 0]
```

Binds an IPv4 loopback address (127.0.0.1 by default). The setup and optional native update ZIP
must have their exact filenames for the given version. The GitHub-shaped feed at
`/releases/latest` lists each supplied artifact with its size, SHA-256 digest and download URL.
With `--prerelease`, `/releases?per_page=30` lists the same release. The exact
`/download/<filename>` paths support `HEAD` and single byte ranges. It prints the URL to put in
`FIORIVOCE_UPDATE_FEED_URL` and makes no outbound request.
It serves until Ctrl+C: stop it when the soak is over. The tests that start it (in-process, or
the command as a child process) stop it in their clean-up, so a failing test leaves no server.
