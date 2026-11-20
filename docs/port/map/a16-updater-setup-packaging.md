# App updates, setup update, guided setup

Area a16. Source checkout: `D:\coding\whisper_local` (paths below are relative to it).
Files read in full: `services/app_update.py` (1420 lines), `services/app_update_apply.py` (3065),
`services/app_update_macos.py` (741), `services/code_identity.py` (80), `services/guided_setup.py` (112),
`services/linux_deps.py` (574), `services/package_checks.py` (132), `services/release_checks.py` (247),
`services/setup_update.py` (195), `services/update_contract.py` (195), `services/update_data.py` (90),
`ui_qt/dialogs/app_update_dialog.py` (604), `ui_qt/dialogs/guided_setup.py` (1233).

Other-side code read to state contracts (not part of the area, owned elsewhere): `services/runtime/updater.py`,
`scripts/updater_helper.py`, `scripts/pack_update_archive.py` (head), `services/verified_download.py`,
`services/verified_files.py`, `ui_qt/utils/release_notes.py`, `ui_qt/widgets/update_notice.py`,
`ui_qt/system_tray.py:set_update_available`, `ui_qt/ui_controller.py` (update flow, guided-setup offer, code-change
restart prompt, About), `ui_qt/bootstrap.py` (health acknowledgement, guided-setup init), `main.py` (early update gate,
`--macos-update-helper`, `--self-test`, restart), `installer/OpenWhisper.iss` (update-related parts),
`ui_qt/styles/theme.qss:3500-3615` (update dialog rules), `docs/guided-setup.md`.

## What the user gets

- **Update notice.** Shortly after start (8 s) and every 4 h while running, the app asks GitHub for the newest release.
  If it is newer, an "Update to X.Y.Z" pill appears in the title bar and an "Update to X.Y.Z..." item in the tray menu.
  Opt-outs: "Check for updates automatically", "Notify me about updates", "Get beta updates" (Settings > General >
  Updates) and two checkboxes in the update dialog.
- **Check for Updates...** (Help-style menu item) always asks GitHub and always shows a dialog: update available,
  up to date, development build, or "Could not check for updates".
- **Update dialog.** Title, version line with download size, "WHAT'S NEW" card with the release notes rendered from
  GitHub Markdown, hint text, two opt-out checkboxes, and Later / Download and install (or Open release notes).
  Installing shows a progress panel (download bytes, verify, prepare/extract, "Restarting to finish the update...")
  with Cancel until the hand-off.
- **Windows install, in-app.** A per-user (HKCU) Inno install gets a native update: download a SHA-256-verified
  `tar.xz`, extract it beside the install, verify every file against an embedded manifest, snapshot settings and
  database, then a separate updater exe swaps directories atomically after the app quits, starts the new version
  for a health check (180 s) and rolls everything back (files, data, registry) if it does not report healthy. Any
  native failure can fall back to "Use installer instead": the verified Inno setup run silently over the install.
  Elevated / Program Files / HKLM installs use the silent setup directly.
- **macOS (Apple Silicon).** Verified DMG, mounted read-only, app bundle checked (bundle id, version, code signature,
  same Team ID), copied beside the installed bundle, swapped with `renamex_np(RENAME_SWAP)` after quit, started
  through LaunchServices with a health token, rolled back on failure. Cannot replace itself (translocated,
  read-only volume, other owner): the DMG is opened for a manual drag-install.
- **Linux, git checkouts, source copies.** Notify only. Linux packages: package manager owns the files, the dialog
  just opens the release page. Git checkouts show `git describe` and copy-paste `git pull --ff-only` commands and
  never auto-check.
- **Leftover errors.** If an update failed, was rolled back or was interrupted, the next launch shows a
  "Update failed" box with the reason.
- **Restart prompt.** If the code on disk changes under a running app (git pull, package upgrade) the app offers
  "Restart OpenWhisper" once (10 s poll), with "Don't show this again".
- **Guided setup.** A brand-new install (no settings file and no prior data) is offered a first-run window 750 ms
  after first paint: Welcome, then Record (microphone + test, shortcut, press-or-hold, speech model download),
  Clean up (AI cleanup switch, provider, level tiles, example), Output (auto-paste, copy, live transcript), then a
  Ready page with a try-it box that answers the user's first dictation. File > Guided setup... reopens it.
- **Linux preflight.** On Linux the app checks for the shared libraries Qt/PortAudio need before importing Qt and
  prints a distro-specific install line instead of crashing.
- **Release self-test.** Frozen builds have `--self-test` / `--workflow-smoke` used by CI.

## Feature inventory

Status vocabulary: port / drop (engine-specific) / undecided backend / other-platform only. "owner" marks rows that
need a project-owner decision first (see Open questions in the Porting notes).

| ID | Feature | Exact user-visible behavior | Source | Status |
|---|---|---|---|---|
| A16-01 | Version parsing and comparison | `parse_version`: strip whitespace and leading `v`/`V`, cut at first `-`, `+` or space, split on `.`, non-numeric segment -> 0, pad to 3. Compare tuples: current < latest = update_available, > = development, == = up_to_date. Lenient on purpose; padded `2.6.01` equals `2.6.1`. Strict parser (`parse_strict_version`, regex `\d+\.\d+\.\d+\Z`) is used only to authorize applying. | `app_update.py:parse_version,compare_versions`; `update_contract.py:normalize_version,parse_strict_version,is_newer_version` | port |
| A16-02 | Install channel detection | `installer` when frozen; `git` when not frozen and `<bundle_root>/.git` exists; else `source`. Labels: installer -> "Linux package" (linux), "macOS application" (darwin), "Windows installer"; git -> "Source checkout (git)"; source -> "Source copy". | `app_update.py:detect_channel,channel_label` | port (C++: installed build vs dev build; decide what "source" means) |
| A16-03 | Release track | `stable` unless `update_beta_track_enabled` is true; unreadable settings -> stable. Beta reads `releases?per_page=30`, skips drafts and tags that are not plain `[vV]?\d+\.\d+\.\d+`, picks highest version, stable beats prerelease on a tie. Prerelease assets installable only on beta track. Leaving beta never downgrades (reports development). Switching track clears `update_last_check_at` so the next automatic check is due now. | `app_update.py:release_track,release_track_changed,select_newest_release,parse_release_payload` | port |
| A16-04 | Fetch latest release from GitHub | GET `https://api.github.com/repos/Knuckles92/OpenWhisper/releases/latest` (stable) or `.../releases?per_page=30` (beta) with `Accept: application/vnd.github+json`, `User-Agent: OpenWhisper/<version>`, timeout 30 s, via the OS trust store (proxies, enterprise roots). Bad JSON -> "The update server returned a response that could not be read." Missing tag -> "The update server did not report a release version." Repo constant must change for FioriVoce. | `app_update.py:fetch_latest_release,GITHUB_REPO,LATEST_RELEASE_URL,RELEASE_LIST_URL`; `verified_download.py:open_url,USER_AGENT,NETWORK_TIMEOUT_S` | port (owner: repo, user agent) |
| A16-05 | API-free fallback on rate limit | HTTP 429, or 403 with `X-RateLimit-Remaining: 0` or a `Retry-After` header, counts as rate limited: store `update_api_retry_at` = max(now+60 s, `X-RateLimit-Reset`, now+`Retry-After`), then HEAD `github.com/<repo>/releases/latest`, require final URL `.../releases/tag/(v?\d+\.\d+\.\d+)`, fetch `.../releases/download/<tag>/SHA256SUMS.txt` (<= 64 KiB, each line `^[a-fA-F0-9]{64} [ *][A-Za-z0-9_.-]+$`, duplicates invalid), HEAD each wanted asset (status 200, 0 < Content-Length <= 2 GiB) and synthesize API-shaped asset entries. Notes become "[Read the release notes on GitHub](url)." While `update_api_retry_at` is in the future the API is not called at all. Beta falls back to newest stable. Missing/invalid checksums -> notify only. Status line on a background failure: "Could not check for updates (GitHub rate limit)." Dialog text: "GitHub rate-limited this update check. Try again later, or open https://github.com/Knuckles92/OpenWhisper/releases." | `app_update.py:_is_rate_limited,_remember_api_rate_limit,_api_retry_pending,_fetch_latest_release_from_web,_latest_public_release_tag,_fetch_release_checksums,_public_release_assets,_release_asset_from_checksum,update_check_failure_status` | port |
| A16-06 | Network trust rules | Initial URL must be https, port 80/443 or absent, no userinfo, no fragment. Redirects: max 5, every hop and the final URL re-validated. From `api.github.com`: only `api.github.com`, or (for `/releases/assets/<id>` API URLs) `release-assets.githubusercontent.com` with path prefix `/github-production-release-asset/`. From `github.com`: `github.com` or that CDN. HEAD stays HEAD across redirects. Unsafe -> "The update server redirected to an unsafe URL." / "The update URL is not trusted." / "The update server returned an unsafe URL." Error text via `describe_network_error`: certificate failure names `api.github.com, github.com and release-assets.githubusercontent.com` for IT; HTTP -> "The update server returned an error (CODE REASON)."; URLError -> "Could not reach the update server (REASON)." | `app_update.py:_redirect_url_allowed,_parsed_url_is_well_formed,_SafeRedirectHandler,_open,_describe_update_error`; `verified_download.py:describe_network_error` | port |
| A16-07 | Loopback soak feed override | `OPENWHISPER_UPDATE_FEED_URL` honoured only if scheme http/https, host a loopback IP literal, explicit port, no userinfo, path exactly `/releases/latest`, no query/fragment; otherwise logged and ignored. With the override, assets from the feed's own origin with the right file name are accepted, GitHub fallback and cooldown are bypassed, prerelease assets are installable on beta. | `app_update.py:update_feed_override,_is_loopback_feed_url,_asset_url_allowed`; `scripts/serve_update_feed.py` | port (dev/test tool) |
| A16-08 | Release asset recognition | Exactly one asset named `OpenWhisper-Setup-<ver>.exe`, `OpenWhisper-<ver>-win64.tar.xz`, `OpenWhisper-<ver>-macos-arm64.dmg`; `state` None or `uploaded`; `browser_download_url` must equal `https://github.com/<repo>/releases/download/<tag>/<name>` (or the feed origin); size int in (0, 2 GiB]; `digest` must be `sha256:` + 64 hex; `github_asset_id` kept only for a positive int and the exact URL. Drafts expose no assets; prereleases expose none off the beta track. An asset is "ready" only with url + sha256 + size > 0; without a digest the release is notify-only. | `app_update.py:parse_release_payload,_parse_named_asset,_digest_to_sha256,_asset_ready`; `update_contract.py:setup_asset_name,archive_asset_name` | port (owner: asset names) |
| A16-09 | Check scheduling | Timer 8 s after startup (`UPDATE_CHECK_DELAY_MS`); then due when 4 h (`UPDATE_CHECK_INTERVAL_S`) have passed since `update_last_check_at` AND 1 h since `update_last_failed_check_at`. A launch check ignores the 4 h but still honours the 1 h failure wait. Stored times in the future (clock moved back) are ignored; unparseable -> missing; naive timestamps taken as UTC. Disabled (`update_check_enabled` false) or git channel -> never. After each check the next is scheduled from stored times but never sooner than 1 h (`UPDATE_RECHECK_FLOOR_MS`). Failed check writes `update_last_failed_check_at` and never the success time. Settings read error -> no automatic check. Check runs on a shared worker; result delivered on the UI thread. | `app_update.py:seconds_until_auto_check,mark_check_completed,_save_check_timestamp`; `services/runtime/updater.py:schedule_check,_maybe_start_check,_check_wait_ms,_on_check_finished` | port |
| A16-10 | Check result handling in UI | A failed check keeps the last good result. If the update dialog is open a new check only raises it. A manual check opens the dialog (success or error). A failed automatic check shows an ambient status-bar message (rate limit variant, else "Could not check for updates."). | `ui_qt/ui_controller.py:on_update_check_finished`; `updater.py:_on_check_finished` | port |
| A16-11 | Update notice (title bar + tray) | Shown only when status == update_available AND check enabled AND notify enabled. Title-bar pill (`updateNoticeButton`): text "Update to X" (compact window: "Update"), tooltip "OpenWhisper X is available. Click to see what's new.", accessible name "Update to OpenWhisper X", download icon, hidden until a release is found. Tray: action "Update to X..." and tooltip "OpenWhisper: update X available". Both open the update dialog on the last found release. | `app_update.py:shows_update_notice`; `ui_qt/widgets/update_notice.py`; `ui_qt/system_tray.py:set_update_available`; `ui_controller.py:_refresh_update_notice,show_found_update` | port |
| A16-12 | Update dialog | See "AppUpdateDialog" below: five states (available/apply, available/notify, up to date, development, check failed), download state, error state, cancel semantics. | `ui_qt/dialogs/app_update_dialog.py` | port |
| A16-13 | Dialog opt-outs | Checkboxes "Don't notify me again" (= not `update_notify_enabled`) and "Don't check automatically" (= not `update_check_enabled`), initial values from settings, saved once on Later / primary / window close (not while downloading; re-armed after an error). Saves via `update_settings` as one write. After the dialog closes the Settings page is told "updates" changed. | `app_update_dialog.py:_persist`; `app_update.py:persist_prompt_choices`; `ui_controller.py:show_app_update_dialog` | port |
| A16-14 | Verified download | `download_release_asset`: validates digest (64 hex), file name == basename, size (0, 2 GiB]; reuses an existing identical file in `<LOCALAPPDATA>/OpenWhisper/updates` (size equal and SHA-256 equal -> emits `verifying` and returns); a stale or wrong file is deleted. Otherwise shared resumable downloader: `<file>.part` with mode 0600, Range resume, 416 restarts once, exact size and SHA-256, integrity failure deletes the partial, short transfer keeps prefix, cancel deletes partial, rename into place. GitHub release URL failing with 500/502/503/504 switches once per transfer to `api.github.com/repos/<repo>/releases/assets/<id>` with `Accept: application/octet-stream` (needs `github_asset_id`). Updater wording: "The update package has an invalid size.", "The update download could not be created safely.", "The update partial is not a regular file.", "The partial update could not be read.", "The update server returned an invalid resume response.", "...an invalid download size.", "...an unexpected download size.", "...more data than expected.", "The verified update could not be finalized. Please retry.", plus the shared "The download was canceled." and "The download failed its integrity check and was discarded. Please try again." Progress: `(downloading, done, total)` then `(verifying, total, total)`. | `app_update.py:download_release_asset,_download_verified,_reuse_verified_download,_validated_asset_digest,_regular_file_stat,_DOWNLOAD_MESSAGES`; `verified_download.py:download_verified`; `verified_files.py` | port |
| A16-15 | Apply-mode resolution | `ApplyMode`: macos_dmg, native, setup, notify_only. Non-installer channel -> notify. darwin: macos_dmg only on arm64/aarch64 with a ready DMG asset. linux -> notify. Windows frozen: native if native archive ready AND `native_apply_eligible`; else setup if setup asset ready; else notify. `can_apply` = mode in (native, setup, macos_dmg). | `app_update.py:resolve_release_apply_mode,can_apply`; `app_update_apply.py:resolve_apply_mode,native_apply_eligible` | port (Windows/mac: other-platform only for the mechanism) |
| A16-16 | Native-apply eligibility (Windows) | All of: HKCU Inno registration found (ARP key `Software\Microsoft\Windows\CurrentVersion\Uninstall\{CA36AD0A-...}_is1`, views default/64/32; HKCU preferred) with `InstallLocation` and `UninstallString`; DisplayVersion parses strictly; process not elevated; running dir not under Program Files (3 env vars); running dir == registered InstallLocation; no reparse points from the dir to root; registered uninstaller `unins*.exe` exists in the dir and matches the registered command path; `OpenWhisperUpdater.exe` present. HKLM installs are setup-only. | `app_update_apply.py:discover_install_registration,native_apply_eligible,_install_dir_eligible,registered_uninstaller_path,is_process_elevated,_is_under_program_files` | other-platform only (Windows) |
| A16-17 | Native candidate preparation | Under two named mutexes (setup gate must be free; update mutexes acquired non-blocking: "Setup is already running." / "Another update is already in progress."). Requires strictly newer version, eligible install, archive inside the updates dir. Reads the root manifest from the tar.xz without enumerating, validates it (schema 1, product, app id, architecture `win-x64`, topology revision 1, version == release, `minimum_updater_version` <= running version else `UpdateRequiresSetup`, member count, sizes <= 2 GiB, all file hashes well-formed, both exes present, only `OpenWhisper.exe`/`OpenWhisperUpdater.exe`/manifest at top level + `_internal/`). Free space check (1.15 margin) for install dir parent (current size + unpacked) and for the transaction dir (archive). Creates `tx/<uuid hex>/`, writes the PREPARING journal, extracts to sibling `<app>.new-<8 hex>` (regular files/dirs only, member limits 50,000 / depth 20 / name 240 / case-insensitive duplicate rejection / reserved Windows names / no `:`, `\`, absolute, `..`, trailing dot or space), verifies every file against the manifest, copies the Inno uninstaller files and a legacy `_internal/nvidia` tree into the candidate, hashes them as compatibility files, fsyncs, writes `.openwhisper-update-complete`, stages `OpenWhisperUpdater.exe` into the transaction dir, saves PREPARED. Cancel is honoured after extraction and after hashing. Any failure removes the transaction. A registered version that differs from the running one only logs a warning (the commit repairs it). | `app_update_apply.py:prepare_candidate,_check_preparation_request,safe_extract_tar_xz,_TarExtraction,validate_manifest,verify_tree_against_manifest,preserve_compat_files,build_compatibility_file_manifest,_stage_updater_helper` | other-platform only (Windows) |
| A16-18 | Native commit, health check, rollback | Helper (separate exe run from the transaction dir, restarts itself out of the install dir on frozen builds): wait for the parent pid (identity = exe path + creation time, 120 s; if it hangs, terminate it after re-checking identity), acquire app mutexes (120 s) else "OpenWhisper did not close, so the update was not installed. Your current version is unchanged - close OpenWhisper completely and try the update again.", re-verify install and candidate, snapshot data, write RunOnce recovery value, journal OLD_MOVED, rename app -> `.old-<id>`, journal NEW_ACTIVE, rename candidate -> app, release app mutexes, launch new exe with `--update-health <token>`, wait up to 180 s for `healthy` file == token (aborts at once if the process exits), update ARP (DisplayVersion, EstimatedSize, InstallDate), journal HEALTHY, clear RunOnce, delete leftovers. Any failure before health: terminate the new process, re-acquire app mutexes (30 s), restore tree, restore data, restore ARP, write `apply_error.txt`, relaunch the old app. Failure after health only writes "The new version is running, but update finalization failed: ...". | `app_update_apply.py:commit_prepared_update,_CommitAttempt,_wait_for_parent_exit,_force_close_pid,_wait_for_health,_restore_rollback,update_arp_after_success,restore_arp_after_rollback` | other-platform only (Windows) |
| A16-19 | Crash recovery of a half-done update | On launch before acquiring the app mutex: skip if health launch or app mutex held; for journals of this install dir: PREPARING/PREPARED with a dead parent -> abandon; OLD_MOVED/NEW_ACTIVE -> start the staged helper with `--recover` and exit. `recover_transaction` reconciles the three trees (active/candidate/rollback) by hash: new+healthy -> finalise; old -> clean up; rollback valid -> restore (unless a foreign exe was installed meanwhile -> SUPERSEDED); else error "The interrupted update has no validated rollback tree." Writes "The previous update was interrupted and was rolled back." | `app_update_apply.py:recover_before_start,recover_transaction,_reconcile_interrupted_commit,_finish_recovered_rollback,_retire_unstarted_transaction` | other-platform only (Windows) |
| A16-20 | Single-instance and update gates (Windows) | App holds `OpenWhisper-App-<GUID>` and `Global\...` mutexes for its lifetime (Inno `AppMutex` sees them). Launch refused with a native message box "OpenWhisper is finishing an update. Try again in a moment." while an update mutex exists (unless valid health token); launch waits up to 120 s for the Inno setup mutex then "OpenWhisper Setup is still running, so OpenWhisper did not start. Finish the installer, then start OpenWhisper again." Second instance exits silently. `release_application_mutex_for_setup` before spawning setup and before a restart. | `app_update_apply.py:acquire_application_mutex_or_exit,maybe_exit_if_update_in_progress,release_application_mutex_for_setup,is_valid_health_launch_token`; `main.py:_early_update_gate` | other-platform only (Windows) |
| A16-21 | Pre-update data snapshot | Before the swap: copy `openwhisper_settings.json` and SQLite-backup `openwhisper.db` (backup API so WAL is included) into `tx/<id>/data/`, fsync, record SHA-256 (null for absent files) in `data-snapshot.json`. On rollback verify digests, delete `-wal`/`-shm` sidecars, restore via temp file + write-through replace; absent files are restored as absent. Not restored when the new version already confirmed health ("never rewinds later user work"). | `update_data.py:snapshot_data,restore_data`; `app_update_apply.py:_finish_recovered_rollback` | port (needs the C++ settings/DB file names) |
| A16-22 | Setup (Inno) fallback | Verified `OpenWhisper-Setup-<ver>.exe` run detached with `/SP- /SILENT /NORESTART /OPENWHISPERUPDATE=1 /CURRENTUSER` (or `/ALLUSERS` when registered in HKLM) `/DIR=<running app dir> /LOG=<updates>/setup.log`; app releases its mutexes first, then exits; Inno relaunches the app (`IsInAppUpdate`). Offered automatically when native payload needs setup (`UpdateRequiresSetup`, only if a setup asset is ready), and as a button "Use installer instead" after a failed native update. A failed launch re-acquires the mutex and reports "The installer could not be started." | `app_update.py:download_installer,_apply_native_update`; `ui_controller.py:_setup_installer_command,_report_launch_failure` | other-platform only (Windows) |
| A16-23 | Setup helper protocol | Installer runs the bundled updater with `--setup-action prepare|finish|rollback --app-dir D --error-file F`: prepare backs up managed files (`OpenWhisperUpdater.exe`, `OpenWhisper.exe`, `.openwhisper-update.json`, `_internal`, sentinel) to `<app>.setup-backup` with marker `setup.json` (copying -> prepared), retires native transactions for the dir (restoring a half-swapped one first), then removes the managed files; finish verifies the installed tree against its manifest (allowing extra top-level files and `_internal/nvidia/`) and deletes the backup; rollback copies back. Errors go to the error file for Inno to show. | `setup_update.py`; `scripts/updater_helper.py:_run_setup`; `installer/OpenWhisper.iss:RunSetupHelper` | other-platform only (Windows); part of installer design |
| A16-24 | macOS in-place update | See Modules > app_update_macos. Result string `macos:<32 hex>`; staged hidden bundle `.<name>-update-<id>.app` beside the app; helper = staged app's own executable with `--macos-update-helper --transaction-id T --parent-pid P`; ready file; 60 s wait for ready; 120 s wait for parent exit; 180 s health wait; rollback message "OpenWhisper X did not start correctly, so version Y was restored." | `app_update_macos.py` | other-platform only (macOS) |
| A16-25 | Hand-off to the updater | Before applying, if recording / meeting / transcribing / component install is active: "Install update?" - "OpenWhisper is busy with <a recording|a meeting|transcription|a component install>. Install the update anyway? Unsaved work in that task may be lost." (default No; No -> dialog error "The update was not started." or, after download, "The update was not installed. Your work is still open."). On launch success: mark dialog handed-off, force-quit flag, 10 s hard-exit watchdog (`os._exit(0)`), `QApplication::exit(0)` (never `quit()`, which windows can veto). Launch failure keeps the app open ("The updater could not be started."). | `ui_controller.py:on_update_download_finished,_confirm_update_while_busy,exit_for_update,arm_handoff_watchdog,_native_helper_command,_start_detached` | port (mechanism platform-specific) |
| A16-26 | Cancel and attempt tokens | Each download uses a fresh cancel event and attempt object; a stale attempt can never change the current dialog or launch; a success that arrives after cancel is discarded (`discard_prepared_result`) and reported as "The download was canceled." / "The update was canceled." Cancel at the dialog: first press = "Canceling the update...", second press = close. After the RESTARTING phase Cancel is disabled. | `services/runtime/updater.py`; `app_update_dialog.py:_on_cancel,mark_handed_off`; `ui_controller.py:_report_download_error` | port |
| A16-27 | Leftover cleanup at startup | Delete regular files in the updates dir older than 30 s; prune finished/rolled-back/superseded native transactions (legacy journal-less ones after 24 h); on macOS prune `macos-tx`; remove a leftover setup backup for the running app dir. A running Inno setup exe stays locked and is collected next start. | `app_update.py:prune_stale_downloads`; `app_update_apply.py:prune_abandoned_transactions,_prune_transaction`; `app_update_macos.py:prune_transactions`; `setup_update.py:cleanup_setup_backup`; `runtime/updater.py:prune_leftovers` | port |
| A16-28 | Apply error surfacing | `apply_error.txt` (<= 2000 chars) written by helper/recovery; consumed (read and deleted) at the next non-health launch and shown as a warning box titled "Update failed". Native helper has no Qt: failures use a last-resort OS message box (`MessageBoxW`, error icon). | `app_update_apply.py:write_apply_error,consume_apply_error,native_message_box`; `ui_controller.py:show_apply_error_if_any` | port |
| A16-29 | Health-check launch | Started with `--update-health <token>`: main window disabled, guided-setup offer and settings pre-build skipped, no apply-error box; after 1500 ms the process writes the token to `tx/<id>/healthy` if the journal is NEW_ACTIVE, running dir == journal dir, and the exe hash == `new_exe_sha256`; then re-enables the window. If startup reported `update_readiness_error`, or the acknowledgement fails, exit code 1 (helper then rolls back). | `ui_qt/bootstrap.py:_schedule_update_acknowledgement`; `app_update_apply.py:write_health_acknowledgement,parse_health_token` | port (health protocol is platform-neutral in concept) |
| A16-30 | Code-changed restart prompt | Every 10 s (`CODE_CHANGE_POLL_MS`) compare `code_identity()` to the value at launch; installed build: `inode:size:mtime_ns` of the executable; git checkout: HEAD commit read from `.git/HEAD`, ref file or `packed-refs` without spawning git; uncommitted edits ignored. When changed and not busy: box "Restart OpenWhisper" / "OpenWhisper was updated while it was running." / "Restart to finish the update. Until you do, parts of the app that haven't loaded yet may not work." + checkbox "Don't show this again" (saves `update_restart_prompt_enabled=false`), buttons "Restart now" (default, accept) and "Later". Asked once per run; later only a failure asks again (warning icon, "OpenWhisper was updated while it was running, which likely caused this error.", detail text). Restart = save geometry, force quit, exit with `RESTART_EXIT_CODE`. Timer stops when the setting is false. | `code_identity.py`; `ui_controller.py:_on_code_change_poll,offer_restart_for_code_change,restart_app` | port (C++ has no lazy imports; value is mostly the installed-exe case, see notes) |
| A16-31 | About dialog update line | About box shows version, install channel label and "Update status: Update available: X / Development build, newer than X / Development build / Up to date" from the last result. (About body text itself is another area.) | `ui_controller.py:show_about_dialog` | port |
| A16-32 | Release notes renderer | GitHub Markdown subset to Qt rich text: input cut at 6000 chars on a line break; drops sections titled sha/checksums/hashes/verification/verify/artifacts/assets/downloads, bare digest lines, "Full changelog:" lines and HTML comments; headings -> 13px/600 paragraphs; bullet and ordered lists, rules, fenced code (dropped if only digests), paragraphs joined, indented list continuation; inline code with nbsp padding, `[text](http(s) url)`, `**bold**`, `*italic*`, `_italic_`; HTML escaped. Colours are palette tokens resolved at render time. | `ui_qt/utils/release_notes.py` (shared util, belongs to UI utils area but this area owns its use) | port |
| A16-33 | Guided setup eligibility | `initialize()` (before the main window or DB creates files): if `guided_setup_state` already in settings do nothing; else state = `pending` only when this launch started with no settings file AND none of `openwhisper.db`, `transcription_history.json`, `scratchpad.txt`, a non-empty `recordings/` or `meetings/` exists beside the settings file; otherwise `existing`. Written with `setdefault` under the store lock. Any exception: log, leave unset (corrupt settings are neither overwritten nor invited). States: pending, started, skipped, completed, existing. `should_invite` = state == pending. | `services/guided_setup.py`; `ui_qt/bootstrap.py:309`; `services/settings.py:582` | port |
| A16-34 | Guided setup offer timing | After first paint and not in a health launch: wait 750 ms; if still pending and no modal widget is active and the main window is visible and not minimised, show the Welcome page; else retry every 1000 ms. A visible dialog is not re-offered. File > "Guided setup..." opens straight on the first step (Record). Singleton window reused. | `ui_controller.py:offer_guided_setup,_offer_guided_setup_now,open_guided_setup,_show_guided_setup`; `main_window.py:969` | port |
| A16-35 | Guided setup window and navigation | See "GuidedSetupDialog". Footer: Skip setup / Not now / Close, Back, Continue / Get started / Finish / Done. Skip, Escape, window close and "All settings" save `skipped` (never downgrading a finished setup); Finish saves `completed` and shows the Ready page. Failed save: "Couldn't save setup progress: <err>" and stay. | `ui_qt/dialogs/guided_setup.py` | port |
| A16-36 | Record step | Microphone combo mirrors Settings' (filled asynchronously) + Test button toggling the microphone check ("Test"/"Stop"); backup microphones preserved; shortcut capture field + "Change" (default number-pad key hint "No number pad? Choose Change."); Press-or-hold combo (Press to start and stop / Hold to talk); speech engine row with Download button (when missing) and "Change" link; "More recording options" link. All changes go through the Settings dialog's own handlers and save immediately. | `guided_setup.py:_record_page,_choose_microphone,_toggle_mic_check,_choose_trigger,_capture_shortcut,_shortcut_captured,_download_speech` | port (Settings dialog is another area) |
| A16-37 | Clean up step | Switch "Use AI cleanup"; when on: privacy label (remote variant tinted warning), provider row ("<name> . <status>", button Change or Add key), three level tiles Light/Medium/High (+ none selected when a custom prompt wins, with note "Your own cleanup prompt is in use. Picking a level asks before replacing it."), before/after example ("Meet at two, actually three." -> light keeps it, medium/high "Meet at 3."), note "An illustration. Real results depend on your model and instructions." Provider summary text and privacy strings in Modules. | `guided_setup.py:_clean_up_page,_refresh_cleanup,provider_summary,CleanupChoices` | port (text-LLM providers belong to the cleanup areas) |
| A16-38 | Output step | Switches Paste automatically, Copy to clipboard, Show live transcript ("Not every speech engine supports it."), link "More output options". Wayland/Hyprland and macOS Accessibility variants of the paste detail text. | `guided_setup.py:_output_page,_refresh_output` | port |
| A16-39 | Ready step | Title "You're ready" / "Almost ready" (when speech model missing or downloading) with lede "Give it a try before you go." / "One thing left before your first dictation."; speech status line + Download link + progress bar; cleanup "needs an API key" line + "Add key" link; keycap showing the shortcut, help text, try-it text box (96..140 px) with live hint and result note: "That worked. OpenWhisper is set up." (immediate), then after 1500 ms (300 ms if auto-paste off) if the box is still empty it is filled and the note becomes "That worked. Here's what you said. It's on your clipboard too." / "It's saved in History."; failure "That one didn't come through: <message>" / "That one didn't come through. Try again." | `guided_setup.py:_ready_page,_refresh_ready,dictation_received,dictation_failed,_fill_try_box` | port |
| A16-40 | In-window speech model download | Download button asks the app to download the configured local model (the app's consent dialogs decide); shows "Downloading... X of Y" / "Installing what the model runs on..." with an 8 px progress bar on Record and Ready pages; failure text "The download didn't finish. Check your connection and try again." or the runtime's message; refreshes speech status when settled; ignores downloads it did not start. Without a local model or downloader it opens the Voice model settings page. | `guided_setup.py:_download_speech,model_download_*,runtime_install_*,_download_settled`; `services/guided_setup.py:speech_model_name,speech_model_downloaded` | adapt (Parakeet only; see Engine-specific) |
| A16-41 | Guided setup responsive layout | Rows switch from side-by-side to stacked below width `round(620 * ui_font_scale)`; stack height sized to the current page only; min size 540x460, default 720x690, bounded to screen minus (40, 60) on show; size grip; Settings is built lazily when leaving Welcome. | `guided_setup.py:_reflow,show_page,showEvent` | port |
| A16-42 | Linux library preflight | On Linux before Qt: probe 9 required SONAMEs (`libEGL.so.1`, `libGL.so.1`, `libxcb-cursor.so.0`, `libxkbcommon-x11.so.0`, `libxcb-icccm.so.4`, `libxcb-keysyms.so.1`, `libxcb-xkb.so.1`, `libxcb-shape.so.0`, `libportaudio.so.2`); on any miss print "OpenWhisper is missing required system libraries:" + list + "Install them with:" + apt/dnf/pacman command + "Then re-run the command. Clipboard copy uses Qt and does not need xclip." and exit 1. Distro family from `/etc/os-release` ID/ID_LIKE. | `linux_deps.py:check_linux_dependencies,missing_libraries,install_command,detect_package_family,REQUIRED_LIBRARIES` | other-platform only (Linux); list must be redone for the C++ build |
| A16-43 | Linux Meeting Mode remediation text | Structured offline guidance (`MeetingAudioRemediation`: title, explanation, commands, verification, restart/rollback notes) for 10 failure reasons (soundcard_missing, libpulse_missing, audio_server_unavailable, pipewire_pulse_missing, default_sink_missing, pactl_missing, monitor_source_missing, monitor_open_failed, unsupported_architecture, unknown_failure), per package family and audio stack, packaged vs source variants; never advises switching Pulse<->PipeWire; reinstall commands name `.deb` / `.pkg.tar.zst` assets. Used by `ui_qt/dialogs/meeting_linux_audio_dialog.py` and `meeting/capture/linux_audio.py` (meeting area). | `linux_deps.py:meeting_audio_remediation` and helpers | other-platform only (Linux); depends on whether Meeting Mode and its SoundCard capture are ported |
| A16-44 | Frozen self-test and import audit | `--self-test`: spawn-safety probe, import every optional module, UI lazy exports, QApplication, TLS trust store has CAs, bundled assets exist, SVG plugin loads, then the synthetic lifecycle smoke. `ON_DEMAND_MODULES` list is a freeze-completeness list. | `package_checks.py`; `main.py:_run_package_self_test` | drop (Python freeze/import plumbing); replace with C++ smoke (A16-46) |
| A16-45 | Workflow smoke (release gate) | `--workflow-smoke [report.json]` / part of self-test: decode a 1 s 16 kHz WAV fixture, run the streaming transcriber 5x with a fixture decoder (stop-to-result timings p50/p95), cancel while a decode is blocked (late text must not escape through Qt queued delivery), restart delivers exactly one result, DB persistence across reopen, recording journal recovery exactly once, backup create/restore with previous data kept. Emits JSON `{schema:2, kind:"synthetic_service_lifecycle", metrics, checks, passed, limitations}`. Peak RSS via `GetProcessMemoryInfo` / `getrusage`. | `release_checks.py` | adapt (idea ports; code is Python-service specific) |
| A16-46 | Installer / packaging outputs | Inno Setup script (per-user by default, `PrivilegesRequiredOverridesAllowed=dialog`, `CloseApplications=force`, `AppMutex`/`SetupMutex`, condensed upgrade, `[UninstallDelete]`), PyInstaller specs, updater exe spec, `.deb`/pacman/DMG builders, `pack_update_archive.py`, manifest writer. Not in this area's file list but defines what the updater consumes. | `installer/OpenWhisper.iss`, `OpenWhisper.spec`, `OpenWhisperUpdater.spec`, `scripts/*` | undecided (owner) |
| A16-47 | Update logging | Helper logs to `<localappdata>/OpenWhisper/updater.log` (rotating 512 KiB x1, INFO); macOS helper logs to `updates/macos-updater.log`; setup run to `updates/setup.log`. | `app_update_apply.py:setup_updater_logging,updater_log_path`; `app_update_macos.py:_setup_logging` | port |

### Added by check (feature inventory)

Found by re-checking the source; same status vocabulary as above.

| ID | Feature | Exact behavior | Source | Status |
|---|---|---|---|---|
| A16-48 | Apply dispatch fallbacks | `apply_update(result, progress, cancel, force_setup)`: `force_setup` -> mode SETUP regardless of `result.apply_mode`. Order: MACOS_DMG -> `_apply_macos_update` (re-resolves the mode, else "This copy of OpenWhisper cannot install the Mac update."; downloads the DMG, then `prepare_in_place_update`, which returns `macos:<id>` or, when the copy cannot replace itself, the DMG path to open by hand); NATIVE with a native asset -> `_apply_native_update`; NATIVE or SETUP with a ready setup asset (native mode but no native asset) -> `download_installer`; no release -> "No installer is available."; anything else -> "This copy of OpenWhisper cannot install the Windows setup package." `_apply_native_update` also fails with "This installation is not registered for native updates." when the ARP registration is missing, and maps `UpdateCanceled` to "The update was canceled." and any other `UpdateApplyError` to `AppUpdateError(str(exc))`. The result is therefore one of: a path (exe / dmg), `native:<id>`, `macos:<id>`. | `app_update.py:apply_update,_apply_native_update,_apply_macos_update` | port (shape); mechanisms platform-specific |
| A16-49 | Download-side wording not in A16-14 | "The update package is missing an integrity digest.", "The update package has an invalid filename.", "The downloaded update could not be inspected.", "The update download path is not a regular file.", "The old update download could not be replaced." (stale non-matching file could not be deleted), "GitHub did not report a stable release version." (API-free HEAD redirect had no stable tag). `download_installer` also never writes a setup exe into a source checkout (channel must be installer) and is Windows-only. | `app_update.py:_validated_asset_digest,_regular_file_stat,_reuse_verified_download,_latest_public_release_tag,download_installer` | port |
| A16-50 | `check_for_update` result shape and persistence | Result always carries `can_apply` / `apply_mode` / `release_track`, computed even when status is up_to_date or development. `git_summary` (`git describe --tags --always --dirty`, 2 s) is filled only on the git channel. A successful fetch writes `update_last_check_at` (ISO-8601 UTC via `datetime.isoformat()`) only when `persist=True`; any `AppUpdateError` writes `update_last_failed_check_at` (also for rate limits) and re-raises. Timestamp write errors are logged and swallowed. `parse_last_check_at`: non-string / blank / unparseable -> none; naive -> UTC. The dialog's opt-out write is one `update_settings` call (both keys together); a write failure only logs "Could not persist update preferences". | `app_update.py:check_for_update,_save_check_timestamp,parse_last_check_at`; `app_update_dialog.py:_persist` | port |
| A16-51 | Guided setup: step buttons and re-entry rules | The three step buttons are clickable and jump straight to Record / Clean up / Output from any page (not only Back / Continue); "All settings" in the header saves `skipped` (unless already finished), closes the window with accept and then opens Settings' home page. "Done" on Ready just accepts (nothing to save, `completed` was saved by Finish). Opening from the File menu calls `begin(welcome=False)`: it clears the try box, error text, try result and pending fill timer, remembers whether setup was already completed (`_finished`; then the skip link reads "Close" and no state is ever downgraded), and otherwise saves `started` before showing Record. Welcome offer (`begin(welcome=True)`) saves `started` only on "Get started". `show_page` clamps the page index to 0..4, cancels any shortcut capture and stops/hides the microphone check on every page change, resets scroll to the top, refreshes speech status on Record and Ready, and builds the Settings dialog lazily on the first page after Welcome (`_ensure_settings` also ensures Settings' Recording page and subscribes to the microphone model's reset/insert/remove signals). Hiding the window cancels capture, stops the mic check, stops the try-fill timer and drops "expecting download" unless a download is actually running. | `guided_setup.py:begin,show_page,_continue,_all_settings,_ensure_settings,hideEvent` | port |
| A16-52 | Guided setup: projection write-through rules | Every switch / combo in the window is a projection of a Settings control: the window re-syncs the Settings control from saved settings first (so a previously refused value can be tried again), sets it, calls `_source_result()`, then re-syncs from saved settings again so the window shows what was actually saved. Mapping: Use AI cleanup -> CLEANUP page (`transcript_cleanup_check`); Paste automatically and Copy to clipboard -> GENERAL (`auto_paste_check`, `copy_clipboard_check`); Show live transcript -> RECORDING (`streaming_enabled_check`); Press or hold -> HOTKEYS (`record_mode_combo`, then `_load_hands_free_latch`); Shortcut -> `_on_local_hotkey_captured("record_toggle", hotkey)` after reloading `current_hotkeys`; level tiles -> `choose_cleanup_level(level)` (ignored for the custom tile) then a queued refresh. `_source_result()` shows the Settings dialog's `message_label` text in the window's red error line only when it starts with "Couldn't", "That shortcut" or "Could not", else clears it, then refreshes. Microphone change: if the chosen token has vanished from the Settings combo (worker replaced inventory) just refresh; if the mic check is running it restarts on the new device. Switch states shown come from `resolve_bool_setting` of `transcript_cleanup_enabled`, `auto_paste`, `copy_clipboard`, `streaming_enabled`. Provider button: "Add key" opens API_KEYS when a key is needed, else "Change" opens CLEANUP. Settings keys read here: also `selected_model`-family keys via `speech_model_name`, the recording trigger mode, hands-free latch, cleanup level, custom cleanup prompt, preferred microphone token and saved microphone priority list. | `guided_setup.py:refresh,_apply_switch,_choose_*,_source_result,_provider_action` | port (explicit interfaces on the C++ Settings dialog) |
| A16-53 | Guided setup: remaining Ready / speech strings | Keycap help when no shortcut is set: "Set a shortcut on the Record step, or use the Record button." (else `shortcut_help`). Speech summary line variants: while a download runs "<summary> . <progress text>"; when readiness is unknown (None: non-local engine) "<summary> . <voice_detail>"; ready "<summary> . Downloaded and ready" (tone ready); missing: "<summary> . <error or 'Not downloaded yet. Download it now, or it's offered when you first dictate.'>" (tone attention). The Record-page Download button shows only when the model is missing and nothing is downloading; the Ready page Download link shows whenever nothing is downloading (inside a row that is itself only visible when missing or downloading). A runtime install failure shows the runtime's own message or "The install didn't finish. Try again."; `expecting_download` clears once settled with the model present or an error shown. The model download progress bar uses 0 as indeterminate when total is unknown. `model_download_*` events are ignored unless `expecting_download` and the model name matches the one requested. `shows_runtime_install(component_id)` is true only while this window is visible, expecting a download and the component is a runtime id (so the app suppresses its own install dialog for it). | `guided_setup.py:_refresh_speech,_refresh_ready,_download_speech,shows_runtime_install,runtime_install_*` | adapt (see Engine-specific) |
| A16-54 | Guided setup: prior-data probe details | `_has_prior_data(root)`: any of `openwhisper.db`, `transcription_history.json`, `scratchpad.txt` exists (existence only, even if empty), or `recordings/` / `meetings/` has at least one entry. `root` is the settings file's parent folder. Eligibility is evaluated before any component creates files, so the order of startup matters (`initialize` must run before the DB / history open). `save_state` rejects any value outside the five states with `ValueError`. | `services/guided_setup.py:_has_prior_data,save_state` | port |

## Modules

### services/update_contract.py (stdlib-only, shared with the helper)
- Responsibility: every constant and name the app, the helper exe, the build scripts and the Inno script must agree
  on; pure path/version helpers.
- Public surface: constants `APP_NAME="OpenWhisper"`, `APP_ID="{CA36AD0A-13B9-4737-87AD-ADB54A28EFC9}"`, `APP_EXE_NAME`,
  `UPDATER_EXE_NAME="OpenWhisperUpdater.exe"`, `INTERNAL_DIRNAME="_internal"`, `MANIFEST_NAME=".openwhisper-update.json"`,
  `SENTINEL_NAME=".openwhisper-update-complete"`, `NVIDIA_RELATIVE="_internal/nvidia"`, `MANIFEST_SCHEMA_VERSION=1`,
  `TOPOLOGY_REVISION=1`, `ARCHITECTURE="win-x64"`, `MINIMUM_UPDATER_VERSION="2.5.2"`, asset name prefix/suffix,
  `UPDATES_DIRNAME="updates"`, `tx`, `journal.json`, `healthy`, `apply_error.txt`, `native:` result prefix, mutex names
  (`OpenWhisper-App-<GUID bare>`, `OpenWhisper-Update-...`, `Global\` variants, `Global\OpenWhisper-Setup-...`), RunOnce value
  `!OpenWhisperUpdateRecover` (the `!` makes Windows delete the value only after the command starts), CLI args
  `--update-health`, `--transaction-id`, `--recover`, `--cleanup-transaction`, `--parent-pid`, managed top-level files/dirs.
  `TransactionState` (preparing, prepared, old_moved, new_active, healthy, rolled_back, superseded), `ApplyMode`,
  `normalize_version`, `parse_strict_version`, `is_newer_version`, `is_setup_bridge_release` (version <= 2.5.2 must use setup),
  `setup_asset_name`, `archive_asset_name`, `validate_transaction_id` (`[0-9a-f]{32}`), `local_app_dir` (Windows
  `%LOCALAPPDATA%\OpenWhisper`; macOS `~/Library/Application Support/OpenWhisper`; else `$XDG_DATA_HOME` or
  `~/.local/share` + `/OpenWhisper`), `updates_root`, `transaction_dir`, `journal_path`, `health_path`, `apply_error_path`,
  `encode_native_result`/`decode_native_result`.
- Edge cases: transaction ids are the only thing the helper accepts on its command line, validated before any path is
  built (path escape test `test_transaction_id_cannot_escape_root`).

### services/app_update.py (Qt-free; GitHub + orchestration)
- Responsibility: channel detection, version compare, scheduling math, GitHub release fetch with trust rules and
  API-free fallback, asset recognition, apply-mode resolution, verified download, native/mac/setup apply dispatch,
  stale download pruning, persisted check timestamps.
- Public surface: `InstallChannel`, `ReleaseTrack`, `UpdateStatus`, `DownloadPhase` (downloading, verifying, extracting,
  opening, restarting, rolling_back), `AppUpdateError`, dataclasses `ReleaseAsset(url, name, size_bytes, sha256,
  github_asset_id)`, `ReleaseInfo(version, tag_name, html_url, notes, setup_asset, native_asset, macos_asset, prerelease)`
  with `.asset` (native else setup), `UpdateCheckResult(status, current_version, channel, release, can_apply, apply_mode,
  git_hint, git_summary, release_track)`; functions `detect_channel`, `channel_label`, `parse_version`,
  `compare_versions`, `release_track`, `release_track_changed`, `seconds_until_auto_check`, `shows_update_notice`,
  `resolve_release_apply_mode`, `can_apply`, `source_update_hint`, `local_git_summary` (`git describe --tags --always --dirty`,
  2 s timeout), `parse_release_payload`, `select_newest_release`, `fetch_latest_release`, `check_for_update(persist=True)`,
  `mark_check_completed`, `persist_prompt_choices`, `updates_dir`, `prune_stale_downloads`, `download_release_asset`,
  `download_installer`, `apply_update(result, progress, cancel, force_setup)`, `discard_prepared_result`,
  `update_check_failure_status`, `update_feed_override`.
- Threading: all blocking; called from the shared executor (checks) and the component executor (downloads) so a
  large download never starves transcription. Progress callback `(phase, done, total)`; cancel is a `threading.Event`.
- State: timestamps in settings (`update_last_check_at`, `update_last_failed_check_at`, `update_api_retry_at`, ISO 8601 UTC).
- Constants: `_FAILED_CHECK_INTERVAL_S=3600`, `_MAX_CHECKSUM_BYTES=64 KiB`, `_MAX_ASSET_BYTES=2 GiB`, `_DOWNLOAD_KEEP_WINDOW_S=30`,
  `_MAX_REDIRECTS=5`, `per_page=30`, `config.UPDATE_CHECK_INTERVAL_S=14400`, `config.UPDATE_CHECK_DELAY_MS=8000`.
- Edge cases to keep: `download_installer` refuses on non-installer channel or non-win32 ("This copy of OpenWhisper cannot
  install the Windows setup package."); `apply_update` with `force_setup` uses setup regardless of `apply_mode`; native
  update with `UpdateRequiresSetup` auto-falls back to setup only if setup asset ready, integrity failure never auto-installs
  something else (`test_integrity_failure_does_not_automatically_install_something_else`); the `.part` suffix file is not
  pruned by the 30 s rule if newer; a download dir entry that is a directory (native transactions) is never deleted
  here; `UpdateCanceled` -> "The update was canceled."; `check_for_update` marks success only after a successful
  fetch and writes the failure time otherwise; `latest` redirect HEAD returns only a stable tag (a prerelease is never
  discovered API-free).

### services/app_update_apply.py (stdlib + winreg + ctypes; also runs inside the updater exe)
- Responsibility: everything about the Windows native update transaction: manifest build/validate, safe tar.xz
  extraction, journal, named-mutex gates, registry (ARP/RunOnce), commit, rollback, recovery, cleanup, launch and
  health protocol, helper CLI parsing, last-resort message box.
- Public surface (grouped): errors `UpdateApplyError`, `UpdateRequiresSetup`, `UpdateCanceled`; dataclasses
  `InstallRegistration(hive, key_path, install_location, uninstall_string, display_version, estimated_size_kb, install_date, view)`
  and `UpdateJournal` (23 fields, `to_dict`/`from_dict` with required-key and state validation); archive/manifest
  `validate_archive_member_name`, `iter_managed_files`, `validate_managed_topology`, `build_update_manifest`,
  `load_manifest`, `validate_manifest`, `verify_tree_against_manifest`, `read_manifest_from_tar_xz`, `safe_extract_tar_xz`,
  `pack_tar_xz`, `write_manifest_file`; install discovery `discover_install_registration`, `running_app_dir`,
  `uninstaller_basenames`, `registered_uninstaller_path`, `native_apply_eligible`, `resolve_apply_mode`; transaction
  `prepare_candidate`, `save_journal`, `load_journal`, `validate_journal_binding`, `commit_prepared_update`,
  `recover_transaction`, `recover_before_start`, `abandon_prepared_update`, `prune_abandoned_transactions`,
  `cleanup_transaction_after_parent`; locks `create_named_mutex`, `acquire_named_mutex(es)`, `release_mutex(es)`,
  `mutex_exists`, `acquire_application_mutex_or_exit`, `release_application_mutex_for_setup`,
  `maybe_exit_if_update_in_progress`; process `process_identity`, `is_process_elevated`, `_wait_for_pid`,
  `_force_close_pid`, `_launch_exe`, `leave_launch_directory`, `_environment_without_bootstrap`; health
  `parse_health_token`, `parse_transaction_id`, `parse_parent_pid`, `is_valid_health_launch_token`,
  `write_health_acknowledgement`; errors `write_apply_error`, `consume_apply_error`, `native_message_box`;
  file helpers `write_json_atomic` (tmp + fsync + `MoveFileExW(WRITE_THROUGH|REPLACE_EXISTING)` or `os.replace` + dir fsync),
  `reject_reparse_chain`, `canonical_path`, `paths_equal`, `file_sha256` (retry while locked).
- Threading / process model: `prepare_candidate` runs on the component worker thread inside the app; commit/recover run
  in the separate helper process (`OpenWhisperUpdater.exe`, windowless, started `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP |
  CREATE_NO_WINDOW`, `SetDllDirectory(NULL)` around launch, bootstrap env vars stripped). Exclusive preparation decorator
  takes the setup gate then the update mutexes with zero wait.
- State machine (journal `state`): `preparing` -> `prepared` -> `old_moved` -> `new_active` -> `healthy`;
  failure/abandon -> `rolled_back`; setup takeover -> `superseded`. Every transition writes the journal first (atomic) and is
  idempotent; recovery reconciles journal state with the three directories by hash. `cleanup_rollback` flag says whether
  `.old-<id>` and `.failed-<id>` trees belong to the transaction.
- Constants: `_PARENT_WAIT_S=120`, `_HEALTH_WAIT_S=180`, `_SETUP_LAUNCH_WAIT_S=120`, `_LOCK_WAIT_S=60`,
  `_CLEANUP_LOCK_WAIT_S=300`, `_LOCK_POLL_S=0.25`, `_LEGACY_PREPARATION_KEEP_S=24 h`, `_ERROR_LIMIT=2000`, `_MAX_MEMBERS=50,000`,
  `_MAX_DEPTH=20`, `_MAX_NAME_LEN=240`, `_DEFAULT_UNPACKED_CAP=2 GiB` (+4 MiB for the manifest), `_SPACE_MARGIN=1.15`,
  `_CHUNK_BYTES=1 MiB`, `_force_close_pid` wait 15 s, rollback app-mutex wait 30 s, cleanup helper copy
  `OpenWhisperUpdaterCleanup-<id>.exe` in the updates dir.
- Edge cases to keep (cite): lock retry on sharing violation/access denied for hashing, rename and delete
  (`_retry_while_locked`, line 295) because AV scanners and just-exited processes hold files; helper restarts itself from the
  updates root so its cwd does not pin the install dir (`leave_launch_directory`, 314); never terminate an unidentified or
  recycled pid (`_force_close_pid`, 2139); never overwrite an existing `.failed` or rollback tree (`_restore_rollback`,
  2519; `_check_install_unchanged`, 2813); a foreign executable installed during recovery makes the transaction SUPERSEDED
  rather than rolled back (`_reconcile_interrupted_commit`, 2985); candidate and rollback trees must be exactly
  `<app>.new-<id8>` / `<app>.old-<id8>` siblings (`_check_journal_trees`, 1137); only Inno uninstaller files
  `unins\d+\.(exe|dat|msg)` and `_internal/nvidia/` may be preserved (1168); archive member rules (485-535); manifest size
  and member-count arithmetic (655-698); only extract members authorized by the manifest with exactly matching size (880);
  an empty health file is not success; cancel after hashing must still cancel (1427); stale DisplayVersion is repaired not
  refused (1465); macOS path in `write_health_acknowledgement` delegates to the macOS module (2005); non-Windows branches of
  the mutex helpers are no-ops returning a truthy handle.
- RunOnce: `HKCU\...\RunOnce\!OpenWhisperUpdateRecover` = `"<tx>\OpenWhisperUpdater.exe" --transaction-id <id> --recover`
  is set before the first rename and cleared on success, rollback or retirement (compare case-insensitively).

### services/update_data.py
- `snapshot_data(journal)` and `restore_data(journal)`; names `openwhisper_settings.json`, `openwhisper.db`; marker
  `tx/<id>/data-snapshot.json`; SQLite `backup()` API; `.update-restore` temp + write-through replace; WAL/SHM sidecars removed
  before restoring a DB; snapshot validated (`set(records) == set(names)`) and re-hashed before any restore.
  Edge case: the data dir must not be a reparse chain (`reject_reparse_chain`).

### services/setup_update.py
- Functions `prepare_setup`, `finish_setup`, `rollback_setup`, `cleanup_setup_backup`, `retire_native_transactions`.
  Marker `<app>.setup-backup/setup.json {app_id, app_dir, state: copying|prepared|installed, files}`; backup restores copy
  back (not move) so recovery can be retried after any interruption including the exe; free-space check on the install
  size; `finish_setup` copies a legacy `_internal/nvidia` over if the new install has none; retirement restores a
  half-swapped native transaction (unless the health file confirms it or the active exe is foreign) then marks it SUPERSEDED
  ("older helpers reject this state before they can move an install").

### services/app_update_macos.py
- Journal `MacUpdateJournal(transaction_id, version, previous_version, app_path, staged_path, bundle_id, executable,
  health_token, state, parent_pid, helper_pid)` under `updates/macos-tx/<id>/journal.json` (strict field/type validation);
  states prepared, swapped, healthy, rolled_back, abandoned.
- `in_place_blocker` reasons (wording matters): "this copy is not an installed app bundle", "macOS runs this copy from a temporary
  location (App Translocation)", "the app is on a read-only volume", "the app's folder cannot be read (...)",
  "another account installed this copy", "this account cannot write to <dir>".
- `prepare_in_place_update`: `hdiutil attach -readonly -nobrowse -noautoopen -mountpoint <tx>/mount`, require
  `OpenWhisper.app` (not a symlink), `verify_candidate` (Info.plist, `CFBundleIdentifier` equals the running app,
  `CFBundleShortVersionString` == release, executable present and executable, `codesign --verify --deep --strict`, TeamIdentifier equal when
  the running copy has one), free space >= 1.1 x tree size ("There is not enough free disk space to install the update."), `ditto` to
  the hidden staged path, detach (retry with `-force`), verify the copy again, save journal. Messages: "The update disk image could not
  be opened.", "The update disk image does not contain OpenWhisper.app.", "The update's app has no valid Info.plist.", "The update is for a
  different app.", "The update's version does not match the release.", "The update's app has no executable.", "The update's code signature
  is not intact.", "The update is not signed by the same developer as this copy.", "The update could not be copied beside OpenWhisper."
- `start_helper` launches the staged exe with `--macos-update-helper`; the app polls `ready` every 100 ms for 60 s then exits.
  `run_helper`: waits (200 ms poll) for the parent to die, swaps via `renamex_np(RENAME_SWAP)` (falls back to three renames
  on ENOTSUP/EINVAL), launches via `/usr/bin/open -n [-W] <bundle> [--args ...]` (so macOS attributes microphone permission to the
  app itself), waits for `healthy` file or process exit, rolls back (stop new app by signalling the app pid found via `ps`, swap back,
  write the restored message, relaunch old). Everything used after the swap is loaded before it. Exit codes 0 ok, 1 failed, 2 refused.
- `prune_transactions`: keeps live-helper and recent (< 1 h) unlaunched ones, deletes unreadable ones older than 24 h.

### services/code_identity.py
- `remember_launch()` (called from `main.py` before `main()`), `changed_since_launch()`, `code_identity()`; pure file reads, no
  subprocess, safe on the UI thread. Returns None when unknown (then never reports a change).

### services/guided_setup.py
- Constants `PENDING/STARTED/SKIPPED/COMPLETED/EXISTING`; `initialize`, `should_invite`, `is_completed`, `save_state`,
  `speech_model_name(settings)`, `speech_model_downloaded(settings)` (None when no local model, False when runtime missing,
  else HF cache check; never downloads). Reads only the disk. Setting `guided_setup_state` (string, `in_table=False`).

### services/linux_deps.py
- Stdlib-only. See A16-42/43. `probe_library` uses `ctypes.CDLL`; C++ equivalent is `QLibrary::load` or `dlopen`. `install_command`
  returns `sudo apt install -y ...`, `sudo dnf install -y ...`, `sudo pacman -S --needed ...`. Package tables per SONAME are in
  `REQUIRED_LIBRARIES` / `MEETING_AUDIO_LIBRARIES`. `OPENWHISPER_RELEASES_URL` = `https://github.com/knuckles92/openwhisper/releases`.

#### Added by check (linux_deps)
- Package tables, per SONAME -> {apt, dnf, pacman}: libEGL.so.1 -> libegl1 / mesa-libEGL / libgl; libGL.so.1 -> libgl1 / libglvnd-glx / libgl; libxcb-cursor.so.0 -> libxcb-cursor0 / xcb-util-cursor / xcb-util-cursor; libxkbcommon-x11.so.0 -> libxkbcommon-x11-0 / libxkbcommon-x11 / libxkbcommon; libxcb-icccm.so.4 -> libxcb-icccm4 / xcb-util-wm / xcb-util-wm; libxcb-keysyms.so.1 -> libxcb-keysyms1 / xcb-util-keysyms / xcb-util-keysyms; libxcb-xkb.so.1 -> libxcb-xkb1 / libxcb / libxcb; libxcb-shape.so.0 -> libxcb-shape0 / libxcb / libxcb; libportaudio.so.2 -> libportaudio2 / portaudio / portaudio. Meeting-only extra (never fails bootstrap): `libpulse.so.0` -> libpulse0 / pulseaudio-libs / libpulse. (The C++ build needs its own list: PortAudio only if a PortAudio backend is kept.)
- `detect_package_family`: reads `/etc/os-release` (fallback `apt` when unreadable or unknown); concatenates `ID=` and `ID_LIKE=` values, lowercase, substring match: dnf for fedora/rhel/centos/rocky/almalinux, pacman for arch/manjaro/endeavouros, apt for debian/ubuntu/linuxmint/pop/elementary.
- `install_command` joins only packages that exist for the family and returns "" when none; unknown family -> "".
- `check_linux_dependencies(stream=None)` writes to stderr by default, returns 0 on non-Linux or when all libraries load, else 1; the caller (`main.py`, `install.sh`) exits with it. The message lines are `OpenWhisper is missing required system libraries:`, `  - <soname>` per miss, `Install them with:`, `  <command>`, `Then re-run the command. Clipboard copy uses Qt and does not need xclip.`
- `LINUX_SYSTEM_AUDIO_GUIDE = "docs/linux-system-audio.md"` is the document Meeting Mode remediation points to (docs area, not ported unless Meeting Mode is). `reinstall_commands(family, version)` (empty without a version or for dnf): apt -> `sudo apt install --reinstall ./OpenWhisper-<ver>-linux-amd64.deb`, pacman -> `sudo pacman -U ./OpenWhisper-<ver>-linux-x86_64.pkg.tar.zst`. Advice-only service commands: `systemctl --user enable --now pipewire pipewire-pulse wireplumber` and a `grep -E 'pipewire|pulse'` listing; never executed by the app.

### services/package_checks.py, services/release_checks.py
- Python freeze/self-test plumbing (see A16-44/45). Only the ideas carry over.

### ui_qt/dialogs/app_update_dialog.py (see UI section)
### ui_qt/dialogs/guided_setup.py (see UI section)

## Data and formats

### Settings keys (JSON settings file, `services/settings.py`)
| Key | Type | Default | Read / written where |
|---|---|---|---|
| `update_check_enabled` | bool | `True` (`config.UPDATE_CHECK_ENABLED`) | `seconds_until_auto_check`, `shows_update_notice`, dialog checkbox, Settings tile "Check for updates automatically" |
| `update_notify_enabled` | bool | `True` | `shows_update_notice`, dialog checkbox, Settings tile "Notify me about updates" |
| `update_restart_prompt_enabled` | bool | `True` | code-change poll, restart box "Don't show this again", Settings tile "Ask me to restart after an update" |
| `update_beta_track_enabled` | bool | `False` | `release_track`; Settings tile "Get beta updates"; toggling calls `release_track_changed` |
| `update_last_check_at` | ISO-8601 string (UTC, `isoformat()`), raw | absent | written on each successful check; removed on track change |
| `update_last_failed_check_at` | ISO string | absent | written on a failed check |
| `update_api_retry_at` | ISO string | absent | written on a rate limit; consulted before any API call |
| `guided_setup_state` | string `pending|started|skipped|completed|existing` | absent (seeded at startup) | `services/guided_setup.py` only; not stored in the settings table (`in_table=False`) |

Tile copy in Settings (owned by the settings area, but the wording belongs to this feature): "Check for updates automatically - When
OpenWhisper starts and every 4 hours while it runs, asks GitHub (api.github.com) for the latest release. The request includes the app
version, no account or device ID. Nothing is installed without your approval."; "Notify me about updates - Shows an Update button in the
title bar and the tray menu when a newer release is found. Requires automatic checks." (disabled when checks are off); "Ask me to restart
after an update - When OpenWhisper is updated while it is running, offers to restart so the update finishes. Turn off if you chose \"Don't
show this again\" and changed your mind."; "Get beta updates - Also offers test releases a few days before everyone else gets them. They
are verified like any release but may have rough edges. Turning this off never downgrades: you keep the beta until a newer regular release
is out."

### Files on disk (per-user data root `local_app_dir()`)
- `updates/` - verified downloads (`OpenWhisper-Setup-X.exe`, `OpenWhisper-X-win64.tar.xz`, `...dmg`, `*.part`), `setup.log`, `apply_error.txt`,
  `OpenWhisperUpdaterCleanup-<id>.exe`, `macos-updater.log`.
- `updates/tx/<32 hex>/` - Windows transaction: `journal.json` (sorted keys, indent 2, `schema_version:1`; fields listed in `UpdateJournal.to_dict`),
  `OpenWhisperUpdater.exe` (staged copy), `healthy`, `data/{openwhisper_settings.json,openwhisper.db}`, `data-snapshot.json`.
- `updates/macos-tx/<32 hex>/` - mac transaction: `journal.json`, `mount/`, `ready`, `healthy`.
- `updater.log` beside the app log (parent of `updates/`).
- Install dir siblings: `<app>.new-<id8>`, `<app>.old-<id8>`, `<app>.failed-<id8>`, `<app>.setup-backup/`.
- Inside a Windows install: `OpenWhisper.exe`, `OpenWhisperUpdater.exe`, `.openwhisper-update.json`, `.openwhisper-update-complete`, `_internal/`, Inno
  `unins000.exe/.dat/.msg`.
- Registry: ARP key (`DisplayVersion`, `EstimatedSize` DWORD KB, `InstallDate` `YYYYMMDD`, `InstallLocation`, `UninstallString`); RunOnce value above.
- Settings file name `openwhisper_settings.json`, DB `openwhisper.db`, legacy `transcription_history.json`, `scratchpad.txt`, dirs `recordings/`, `meetings/` in the data root
  (the guided-setup "prior data" probe and the update snapshot depend on these names; FioriVoce will have its own names).

### Manifest `.openwhisper-update.json`
`{schema_version:1, product:"OpenWhisper", app_id:"{GUID}", version, architecture:"win-x64", topology_revision:1, minimum_updater_version:"2.5.2",
unpacked_bytes, member_count (= files + 1), files:{relative/posix/path:{size, sha256}}}`. Written by `write_manifest_file` at packaging time,
embedded as the archive's root member, re-read from the extracted tree and required to be identical.

### Update payloads
- `tar.xz` (xz preset 9) whose members are the install tree's contents (no leading folder), regular files only; archive name `OpenWhisper-<ver>-win64.tar.xz`.
- `SHA256SUMS.txt` release asset (shown above) for the API-free path.
- GitHub JSON fields consumed: `tag_name`, `html_url`, `body`, `draft`, `prerelease`, `assets[].{name,state,browser_download_url,size,digest,id}`.

### Command line (process-to-process contract)
`--update-health <token>`; `--transaction-id <id> [--recover] [--cleanup-transaction] [--parent-pid <pid>]`; `--setup-action prepare|finish|rollback
--app-dir D --error-file F`; `--macos-update-helper --transaction-id T --parent-pid P`; env `OPENWHISPER_UPDATE_FEED_URL`,
`OPENWHISPER_UPDATER_RELAUNCHED`, `PYINSTALLER_RESET_ENVIRONMENT` (Python-only). Inno setup switches in A16-22.

### Things an existing install depends on
Windows ARP GUID and mutex names (Inno `AppMutex`/`SetupMutex` literally contain them), asset naming, manifest field names, journal layout, `unins*`
file handling, `_internal` managed dir. A shipped helper at version >= `MINIMUM_UPDATER_VERSION` must be able to run any later journal ("bridge" rule:
releases <= 2.5.2 had to use setup). FioriVoce is a new product, so these can be re-chosen once instead of mirrored, but must then stay frozen.

## UI details

### AppUpdateDialog (`ui_qt/dialogs/app_update_dialog.py`)
- Window: `QDialog#appUpdateDialog`, title "OpenWhisper Updates", modal, min width 520, background token `@bg`.
- Layout tree (`QVBoxLayout`, margins 24,22,24,20, spacing 10):
  1. `WrappedLabel#updateTitleLabel` (19px, weight 700, `@text-heading`).
  2. `WrappedLabel#updateBodyLabel` (13px, `@text-secondary-strong`; property `tone="error"` -> `@danger-text-soft`), selectable by mouse.
  3. `QFrame#updateNotesCard` (bg `@surface-sunken`, 1px `@border`, radius 12; inner margins 16,12,6,12, spacing 6): `QLabel#updateNotesCaption` "WHAT'S NEW"
     (10px, 700, letter-spacing 1px, `@text-secondary`) + `_NotesView(QTextBrowser)#updateNotesView` (no frame, no horizontal scroll, external links open, fixed height
     tracked from document height clamped 56..280, `document.setIndentWidth(14)`, QSS padding `0 10px 0 0`, 13px, 6px-wide scrollbar with `@surface-active` handle radius 3,
     min-height 24, hover `@text-muted`). Painted fade: 22 px (or half the viewport) linear gradient from `surface-sunken` opaque to transparent at the top edge when scrolled
     and at the bottom edge when more remains. Card hidden when there are no renderable notes or status is not update_available.
  4. `WrappedLabel#updateHintLabel` (12px, `@text-secondary-strong`, selectable); hidden when empty.
  5. Progress panel `QFrame#updateProgressPanel` (same card styling; margins 16,14,16,14, spacing 10; hidden initially): `AnimatedProgressBar` (indeterminate or fraction), a row (spacing 8)
     with `QLabel#updateProgressDetail` (12px) + stretch + `QLabel#updateProgressPercent` (12px, 600, `@accent-soft`).
  6. `QWidget#updateOptions` (margins 0,4,0,0, spacing 6): `QCheckBox#updateDontNotifyCheck` "Don't notify me again", `QCheckBox#updateDontCheckCheck` "Don't check automatically"
     (13px `@text-tertiary`, indicator 16x16 radius 4).
  7. stretch, `QFrame#updateFooterDivider` (fixed height 1, `@border-subtle`), button row (margins 0,4,0,0, spacing 8, right-aligned): `Button#updateSetupButton` "Use installer instead" (hidden),
     `Button#updateLaterButton`, `PrimaryButton` (property `buttonId="updatePrimaryButton"`, default button). All footer buttons: base minimum size (0, 40) so they fit their text; QSS padding 9px 18px, 13px.
- Strings:
  - Title: "Could not check for updates" / "OpenWhisper X[ beta] is available" (X = release version or "a new version"; " beta" when prerelease) / "This is a development build" / "OpenWhisper is up to date".
  - Body: error text or "The update server could not be reached."; otherwise "You have <current> (<channel label>)." then two spaces then "Download size: <format_size_bytes>." (update available; size of the asset for the apply mode:
    native -> archive, setup -> setup exe, macos -> dmg, notify-only -> none) / "This copy is newer than the latest release (X)." / "The latest release is X."
  - Hint (only update_available): mac DMG explainer "OpenWhisper installs the verified update and restarts, and goes back to this version if the new one does not start. When it cannot replace itself here, the disk image opens instead: drag OpenWhisper to Applications and replace the previous copy."; "Local git: <describe>"; "To update this checkout:\n<git pull --ff-only\npip install -r requirements.txt>"; joined by blank lines.
  - Primary label: check failed -> "Open releases page"; not update_available -> hidden; `can_apply` -> "Download and install"; else "Open release notes". Later label: "Close" (not update_available / failed) else "Later" (becomes "Cancel" while busy, "Close" after cancel requested).
  - Phase text: "Downloading the update...", "Verifying the download...", "Preparing the update...", "Restarting to finish the update...", "Restoring the previous version...", cancel: "Canceling the update..."; error fallback "The download failed."; detail line "<done> of <total>" (download phase with total) else "OpenWhisper <release version>".
- Behaviour: `result_action` in {later, primary, use_setup}; closing the window == Later (persists opt-outs) unless a download is running (then it cancels, second press closes); primary with `can_apply` + callback switches to download mode (hides notes, hint and options; disables primary/setup/checkboxes; shows panel; resizes window to content), otherwise accepts and the controller opens `release.html_url` (apply impossible) or the releases page (check failed). `set_progress`: fraction when total > 0 with "N%" label, else indeterminate; phase RESTARTING calls `mark_handed_off` (clears busy so the dialog can close, disables Cancel). `set_error`: re-enables everything, tone error, restores notes/hint/options, re-arms persistence, shows "Use installer instead" only when apply mode is native and a digest-verified setup asset exists. Callbacks `on_download_requested(result)`, `on_cancel_requested()`, `on_setup_requested(result)`.
- No animations beyond `AnimatedProgressBar` (shared widget). No keyboard shortcuts besides default button/Enter/Escape.

### Update notice pill (`QPushButton#updateNoticeButton`, parent area title bar)
Background `@accent-tint`, text `@accent-soft`, 1px border `@accent-tint-border`, radius 11, padding 2px 10px 2px 8px, margin 0 8px, 12px weight 600; hover `@accent-tint-strong`/`@accent-tint-border-strong`;
pressed `@accent-muted`; icon `download-blue.svg` 14x14; pointing-hand cursor; hidden when no version.

### GuidedSetupDialog (`ui_qt/dialogs/guided_setup.py`)
- Window `QDialog#guidedSetupDialog`, title "Set up OpenWhisper", app icon, non-modal, property `openwhisperHotkeyWindow=True` (hotkey/dictation treat it as an app window so the try-it box takes dictation),
  min 540x460, default 720x690, size grip, own stylesheet `_STYLE` (tokens `@slate-*`, `@accent`, `@success-text`, `@warning-text`, `@warning-tint`, `@danger-text`).
- Root `QVBoxLayout` margins 0 spacing 0: header, body (stretch), footer.
  - `QFrame#guidedHeader` (bg `@slate-field`, bottom border `@slate-border`; row margins 20,12,16,12 spacing 10): 28x28 app icon, label "Set up OpenWhisper" (`#guidedBrand`, 15px/600), stretch, link button `All settings` (`#guidedLink`).
  - Body `QWidget#guidedCanvas` margins 28,16,28,0 spacing 12: step row (3 `QPushButton#guidedStep`, stretch 1 each, spacing 8, hidden on Welcome; text `"<n or check>  Record|Clean up|Output"`, accessible name "Step N of 3: Title", dynamic properties `active`, `done`; 13px, 2px bottom border `@slate-border`, active: `@accent`/600, done: `@slate-text-2` border `@accent`; hover/focus colours), `QScrollArea#guidedScroll` (no frame, no horizontal bar) holding a `QStackedWidget` of 5 pages (non-current pages use Ignored size policy so the page height is its own), `QLabel#guidedError` (13px `@danger-text`, hidden).
  - `QFrame#guidedFooter` (top border `@slate-border-subtle`; margins 20,12,20,14 spacing 10): link "Skip setup" ("Not now" on Welcome, "Close" once completed, hidden on Ready), stretch, "Back" (neutral, visible for pages after Record), primary "Continue" ("Get started" on Welcome, "Finish" on Output, "Done" on Ready; default button). Buttons: `Button` styled with `neutral_button` / `compact_primary_button` (minimum height 34), pointing-hand cursor, autoDefault.
- Page scaffold: `QWidget#guidedPage`, `QVBoxLayout` margins 0,8,0,16 spacing 14; heading group spacing 6 with title (`#guidedTitle` 22px/600; Welcome uses `#guidedHeroTitle` 28px/600) and lede (`#guidedLede` 14px `@slate-text-2`). Card `QFrame#guidedCard` (bg `@slate-field`, 1px `@slate-border`, radius 12; margins 20,16,20,16 spacing 16). Row = title (`#guidedRowTitle` 14px/600) + description (`#guidedDetail` 13px `@slate-text-3`; `tone=ready` `@success-text`, `tone=attention` `@warning-text`) with a control on the right (row spacing 18, text column spacing 3). Labels are `WrappedLabel` PlainText.
- Pages:
  - Welcome: 64x64 app icon above; "Welcome to OpenWhisper" / "Speak, and your words appear wherever you're typing. Three quick choices and you're ready."; card of 3 rows with 22x22 icons (`microphone-blue.svg` "Record - Your microphone and the shortcut that starts dictation.", `wand-purple.svg` "Clean up - Optional AI tidying for punctuation and filler words.", `bolt-green.svg` "Output - Paste into the app you're using, copy it, or both."), footnote "Takes about a minute. Every choice saves as you go, and you can come back any time from File -> Guided setup."
  - Record: "How will you record?" / "Pick a microphone and the shortcut that starts dictation." Rows: Microphone (`ElidingComboBox#basicSettingsCombo`, min height 38, min width 200, expanding; Test button; accessible names "Microphone", "Test microphone"; detail "The one OpenWhisper listens to. Test it to see that it hears you." + " Your backup microphones stay as they are." when more than one saved), embedded `MicrophoneCheck` (hidden until Test; `QProgressBar#microphoneCheckBar`), Shortcut (`ProfileHotkeyInput` + Change; tooltip "Click Change, then press a shortcut. Escape cancels."; detail from `shortcut_help`), Press or hold (combo, 38 px; detail "Press once and talk for as long as you like, or hold the shortcut like a walkie-talkie."), Speech engine (Download button accessible name "Download the speech model", Change link, status text, 8 px `QProgressBar#guidedProgress` named "Speech model download"); link "More recording options".
  - Clean up: "Tidy up what you say?" / "Optional. After each dictation, an AI model fixes punctuation and drops filler words." Row "Use AI cleanup" with `SettingsSwitch` (detail "Fixes punctuation and formatting after transcription." / "Off: you get exactly what the speech engine heard, and no text is sent anywhere for cleanup."); options group (spacing 14): privacy label `#guidedPrivacy` (13px, bg `@slate-raised`, 1px border, radius 8, padding 10px 12px; property `remote=true` -> warning colours), provider row, `CleanupChoices` (3 tiles from `cleanup_levels.SEGMENTS[:3]`: Light "Punctuation & fillers", Medium "+ corrections & lists", High "+ tighter wording"; accessible name "Cleanup level"), custom-prompt note, preview card `QFrame#guidedPreview` (margins 16,12,16,12, spacing 18, two columns "What you say" -> "Meet at two, actually three." and "What you get" -> result, sample label 17px), illustration note. Enabling the switch scrolls the privacy label into view (`ensureWidgetVisible(privacy, 0, 8)`).
  - Output: "Where should your words go?" / "Choose what happens when you finish speaking." Rows "Paste automatically - Insert the text into the app you're typing in." (platform variants: Wayland "Uses the desktop's paste shortcut on Hyprland; other Wayland desktops paste manually."; macOS without Accessibility "Needs Accessibility access. Open More output options to grant it, or paste with Cmd+V."), "Copy to clipboard - Keep it ready to paste yourself when auto-paste is off or can't reach an app.", "Show live transcript - Watch words appear while you speak. Not every speech engine supports it."; link "More output options".
  - Ready: see A16-39; keycap `QLabel#guidedKeycap` (22px/600, `@slate-raised`, border `@slate-border-strong` with 3px bottom, radius 8, padding 6px 16px, min width 64, text from `format_hotkey_display(hotkey)` or "No shortcut"), help `#guidedLede`, try box `QPlainTextEdit#guidedTry` (15px, radius 10, padding 10px 12px, border turns `@accent` on focus, tab changes focus, accessible name "Try dictation here", placeholder "Try it here: press <shortcut> and say something." or "Try it here."), note label, footnote "Change any of this later in Settings, or run setup again from File -> Guided setup."
- Dynamic strings: `provider_summary` returns `(name, status, privacy, remote, needs_key)`: installed-agent provider -> status "Uses your agent's sign-in." / "Install or sign in to this agent in All settings.", privacy "Transcript text is sent to <name>'s model when cleanup is on. No audio is sent for cleanup."; unknown profile -> ("Provider unavailable", "Choose a provider in All settings.", "The saved cleanup provider is unavailable. Choose a provider before using AI cleanup.", remote, no key); else privacy "Transcript text is sent to <name> [on this computer ]when cleanup is on. No audio is sent for cleanup."; status "API key saved . Connection not tested" / "API key needed" / "No API key needed . Connection not tested". `shortcut_help(hotkey, settings)`: push_hold -> "Hold it while you speak, then let go." (+ " Double-tap to keep recording hands-free." if latch) else "Press it to start dictating, and again to stop."; + " It's on the number pad." for `kp ` keys; + the mouse-shortcut note. Ready page try note variants: "Auto-paste is off, so click here and press Ctrl+V|Cmd+V after you speak." (copy on) / "Auto-paste and copying are both off, so the text only appears in History." / "Your words land wherever the cursor is, in any app." Ready cleanup note: "AI cleanup needs an API key for <name>. Until it has one, your text arrives without cleanup." Speech note: "<summary> isn't downloaded yet, so your first dictation starts with the download." / "<summary>: <error or progress text>"; summary row text "<summary> . Downloaded and ready" / "Not downloaded yet. Download it now, or it's offered when you first dictate."
- Timers: `_refresh_timer` (single shot 0 ms, coalesces refresh requests from Settings' microphone model signals), `_privacy_reveal_timer` (0 ms), `_try_fill_timer` (1500 / 300 ms). Window activation (returning from Settings) re-checks speech status and refreshes. Accessibility: step buttons, controls and progress bars have accessible names; Enter presses the focused button and Continue elsewhere; focus goes to the Continue button (or try box on Ready), never to the header link.
- Cross-widget contracts: calls into the Settings dialog (`ensure_page`, `audio_device_combo`, `record_mode_combo`, `_recording_look_recorder`, `_on_profile_capture`, `_on_local_hotkey_captured`, `choose_cleanup_level`, `_update_cleanup_prompt_ui`, `_load_hands_free_latch`, `message_label`, `models.voice_summary/voice_detail`, `_native_wayland`) - in C++ these become explicit interfaces on the settings dialog; destinations `GENERAL, HOTKEYS, RECORDING, VOICE_MODEL, API_KEYS, CLEANUP`. Controller feeds `dictation_received`, `dictation_failed`, `model_download_started/progress/finished`, `runtime_install_progress/finished`, `shows_model_download`, `shows_runtime_install`.

## External dependencies

| Python package / OS API | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| `urllib.request` + `ssl` (`services.http_tls.verified_context`) | HTTPS to GitHub with OS trust store, redirects, HEAD, Range | `QNetworkAccessManager` (QNetworkRequest, redirect policy `ManualRedirectPolicy` to re-validate each hop, `QSslConfiguration` system CA) - runs on its own thread or async; Qt uses the platform proxy/trust store |
| `json`, `re`, `datetime`, `ipaddress` | payload parse, validators, ISO timestamps, loopback check | `QJsonDocument`, `QRegularExpression`, `QDateTime::fromString(Qt::ISODateWithMs)`, `QHostAddress::isLoopback()` |
| `hashlib.sha256` | file digest | `QCryptographicHash::Sha256` (streaming) |
| `tarfile` (xz), `lzma` | native update archive | `libarchive` (vcpkg `libarchive` with xz) or `liblzma` + own tar reader; needs the same member-safety rules |
| `sqlite3.backup` | consistent DB snapshot | SQLite C API `sqlite3_backup_*` (or QtSql driver handle); FioriVoce DB TBD |
| `winreg`, `ctypes` + kernel32/advapi32 (mutexes, `OpenProcess`, `GetProcessTimes`, `QueryFullProcessImageNameW`, `TerminateProcess`, `MoveFileExW`, `GetTokenInformation`, `MessageBoxW`, `SetDllDirectoryW`) | ARP/RunOnce, single-instance gate, process identity, write-through rename, elevation test | Win32 API directly (`RegOpenKeyEx`, `CreateMutexW`, ...); `QSettings` is not enough for the KEY_WOW64 views |
| `subprocess` | launch helper, `git describe`, `hdiutil`, `codesign`, `ditto`, `open`, `ps` | `QProcess::startDetached` / `CreateProcessW` with `DETACHED_PROCESS|CREATE_NEW_PROCESS_GROUP|CREATE_NO_WINDOW`; mac: `QProcess` or `posix_spawn` |
| `plistlib` | Info.plist read (macOS) | `CFBundle` / `QSettings(plist, NativeFormat)` / small plist parser via `QXmlStreamReader` |
| `ctypes.CDLL("libSystem").renamex_np` | atomic bundle swap | `renamex_np` directly (`<stdio.h>`) |
| `os.fsync`, `os.replace`, reparse-point attribute | durability, path safety | `QSaveFile` (+ `commit()` fsync), `FlushFileBuffers`, `GetFileAttributesW` (`FILE_ATTRIBUTE_REPARSE_POINT`), `QFileInfo::isSymLink` |
| `shutil.disk_usage` | free space | `QStorageInfo::bytesAvailable()` |
| `threading.Event/Timer`, executors | cancel tokens, handoff watchdog, workers | `std::atomic<bool>` / `QFuture` with cancel flag, `QTimer`, hard-exit watchdog via `std::thread` + `_exit` |
| `ctypes.CDLL(soname)` (linux_deps) | library probe | `QLibrary` / `dlopen` |
| PyQt6 widgets (QDialog, QTextBrowser, QProgressBar, QStackedWidget, QScrollArea, QPlainTextEdit, QTimer, QSignalBlocker, QLinearGradient/QPainter) | the two dialogs | same classes in Qt 6 Widgets |
| `multiprocessing`, `importlib` | freeze self-test | not needed |
| Inno Setup (build time) | Windows installer | keep Inno, or WiX/MSIX/Qt IFW - owner decision |
| PyInstaller (build time) | packaging | `windeployqt`/`macdeployqt`/`linuxdeploy`, CMake `install()` + CPack |

## Platform-specific code

- **Windows:** everything in `app_update_apply.py` (mutexes, ARP, RunOnce, elevation, process identity, reparse points, write-through moves, locked-file retries, helper exe, `MessageBoxW`), `setup_update.py`, the Inno
  script, `--update-health` launch, `OpenWhisperUpdater.exe` that the main exe also copies into the transaction dir. HKCU/per-user vs HKLM/all-users matters: only per-user non-elevated installs update natively.
  `_internal` and `nvidia` are PyInstaller/CUDA layout details (not meaningful in C++, see Engine-specific).
- **macOS:** `app_update_macos.py` (Apple Silicon only; no Intel DMG asset), `MacUpdateJournal`, `hdiutil`, `codesign`, `ditto`, `renamex_np`, LaunchServices `open`; guided-setup paste text mentions Accessibility
  (`is_accessibility_trusted`); `~/Library/Application Support/OpenWhisper` data root; `main.py` dispatches `--macos-update-helper` before loading Qt/CUDA.
- **Linux (X11, Wayland, Hyprland):** notify-only (package manager owns the files); library preflight (`linux_deps`) incl. xcb libraries that matter on X11 sessions and `libportaudio`; Wayland/Hyprland only appears in the
  Output step text ("Uses the desktop's paste shortcut on Hyprland; other Wayland desktops paste manually."); `XDG_DATA_HOME` data root; meeting-audio remediation text covers PulseAudio/PipeWire. Packaged reinstall commands exist
  for apt (`.deb`) and pacman (`.pkg.tar.zst`) only.
- Git checkout / source copy: `code_identity`, `local_git_summary`, `source_update_hint`; these are developer-only paths and do not exist once FioriVoce is a native binary (a dev build could just report "development build").

## Engine-specific code

- **Drop:** `_internal/nvidia` legacy GPU-tree preservation in `preserve_compat_files`, `build_compatibility_file_manifest`, `setup_update.finish_setup` (the Python app shipped CUDA wheels; FioriVoce loads the NeMo runtime
  by path from a user-chosen CPU/GPU build). `release_checks` fixture decoder around `StreamingTranscriber` and `faster-whisper` imports in the self-test (`modules` list in `main.py`), `ON_DEMAND_MODULES`,
  multiprocessing spawn probe, PyInstaller `_PYI_*` environment scrubbing and the "restart from updates root because onefile bootloader parent pins the cwd" workaround (a native exe has no bootloader parent; keep the cwd-
  not-in-install-dir rule though, `leave_launch_directory` first half).
- **Adapt (Parakeet in-process):** `guided_setup.speech_model_name/speech_model_downloaded` branch on `local_whisper` / `BACKENDS` / "auto" model resolution / `hf_access.is_model_cached` / `missing_runtime`. For
  FioriVoce there is one local engine: the model name is the configured Parakeet model, "ready" = NeMo runtime library found at the configured path AND the model file present (`Recognizer` can report this without
  loading), None when a cloud/remote engine is selected (undecided backends). `runtime_install_*` callbacks map to whatever replaces the downloadable-component installer (NeMo runtime download is an open question: see
  runtimes in `src/engine/runtime.*`). The Ready/Record pages talk about "what the model runs on" (runtime install) - keep the copy if a runtime download exists, else drop that row.
- **Adapt:** the update snapshot must back up whatever FioriVoce persists (settings file, history DB) - not the Python names. The helper exe replaces the Python `scripts/updater_helper.py`; it can be a second small C++ executable
  built from the same `src/updater` core (no Qt) sharing `update_contract`.
- Updates never touch the model files or runtime (they live outside the install dir in the user data dir): keep that invariant.
- **Undecided backend dependence:** "Show live transcript - Not every speech engine supports it." Parakeet TDT is offline-only (live preview = windowed re-decode), so the copy is still accurate; the cleanup provider rows depend on
  the text-LLM area (cloud/agent providers), not on the speech engine.

## Dependencies on other areas

Calls out of this area:
- `config` (`bundle_root`, `is_frozen`, `local_app_dir`, `config.UPDATE_*`, `config.VERSION`, `_version.__version__`).
- `services.settings` (`SettingsKey`, `resolve_*`, `settings_manager.load_all_settings/update_settings/save_setting/mutate_settings/is_new_install/settings_file`) - settings area.
- `services.verified_download` / `verified_files` (resumable verified download, error wording, SHA-256, free space, lock retry) - shared network/file utility area. Contract: `download_verified(url, sha256, size, dest, progress, cancel, opener, describe_error, max_bytes, messages)`.
- `services.http_tls.verified_context`; `services.format_utils.format_size_bytes`.
- Guided setup: `cleanup_prompts.custom_prompt`, `credentials.resolve_credential`, `hotkey_manager.format_hotkey_display/load_hotkey_settings/is_accessibility_trusted`, `text_llm.get_profile`, `installed_agents`, `services.local_asr.catalog`, `hf_access`, `transcriber.local_backend` (engine-specific), Settings dialog internals,
  `ui_qt.dialogs.cleanup_levels`, `settings_microphones`, widgets `MicrophoneCheck`, `ProfileHotkeyInput`, `SegmentedBar`, `SettingsSwitch`, `ElidingComboBox`, `WrappedLabel`, `Button/PrimaryButton`, `AnimatedProgressBar`, `app_icon`, `design_icon`, `restyle.set_style_property`, `font_scale`, `palette.token_color/resolve_tokens`.
- `services.backup`, `backup_startup`, `recording_journal`, `database`, `audio_processor`, `streaming_transcriber` (only in `release_checks`).

Calls into this area:
- `main.py` (early update gate, `--macos-update-helper`, `--self-test`, `--workflow-smoke`, Linux preflight, `remember_launch`, release mutex before restart).
- `ui_qt/bootstrap.py` (`guided_setup.initialize`, health-token handling, `_schedule_update_acknowledgement`, `offer_guided_setup`).
- `services/runtime/updater.py` (`UpdaterRuntime`: scheduling, workers, attempts, leftovers) and `ApplicationController` (`update_readiness_error`, `on_check_for_updates`, `on_update_download`, `on_update_cancel`, `on_update_abandon`, `get_component_installing`).
- `ui_qt/ui_controller.py` (dialog flow, notice, handoff, restart prompt, About, guided-setup singleton and signals), `main_window.py` (File > Guided setup..., Help > Check for Updates..., title bar notice), `system_tray.py`, `settings_dialog.py` (Updates tiles, `release_track_changed`).
- `services/automation/channel_bridge.py` (`mutex_exists`) and `meeting/capture/linux_audio.py`, `ui_qt/dialogs/meeting_linux_audio_dialog.py` (`linux_deps`).
- Build tooling: `scripts/build_component.py` (imports `GITHUB_REPO`), `scripts/pack_update_archive.py`, `scripts/updater_helper.py`, `OpenWhisperUpdater.spec` hidden imports, `install.sh` (`check_linux_dependencies`).

## Tests

Tests for this area (all under `tests/`):
- `test_app_update.py` (782): channel detection, version compare (padded patch, prerelease suffix ignored), schedule math (disabled, git, missing stamp, interval, failure wait, launch, clock skew, invalid), notice rule, payload parsing (real latest shape, zip asset ignored, missing digest = notify only, off-GitHub asset ignored, feed origin), apply modes, error wording (403 vs rate limit, 429, certificate guidance, rate-limit status copy), `check_for_update` persistence, prompt choices, download hash mismatch discards / matching reuse, archive naming and URL allow-list, HKCU/HKLM, stale-download pruning.
- `test_app_update_rate_limit.py` (305): API cooldown, secondary limits, public-release fallback, checksum trust, size validation, stable-tag-only fallback, failure throttle, loopback bypass, HEAD redirect.
- `test_app_update_release_track.py` (351): stable/beta selection, drafts, ties, no downgrade, join/leave, track switch resets check time, soak feed prerelease.
- `test_app_update_download_fallback.py` (253): asset-id retention, API fallback on 500/502/503/504, bounded attempts, Range/416 behavior, digest/size enforcement, redirect allow-lists.
- `test_app_update_apply.py` (960): strict versions, archive member names, safe extract (symlink, hardlink, duplicate case, `..`), manifest validation, top-level topology, transaction-id path safety, preserve uninstaller/NVIDIA, eligibility (HKLM, missing uninstaller, elevated), commit / rollback / recover in every state, live parent handling and force-close identity rules, prepare cancel, locked-file retry, abandoned transaction pruning, launch directory handling.
- `test_app_update_macos.py` (449): result round-trip, bundle discovery, blockers, candidate verification, prepare/rollback/health/prune, `open` launching and signalling, real `renamex_np` swap, UI start/abandon.
- `test_app_update_dialog.py` (297), `test_update_notice.py` (164), `test_app_update_handoff.py` (235): dialog copy/states, cancel semantics, notice rules, hand-off exit and watchdog, setup fallback args.
- `test_update_resilience.py` (407): partial download/resume, locked cleanup, setup-vs-recovery arbitration, data snapshot incl. WAL, cancel races, bridge floor, env scrubbing, legacy journals.
- `test_update_startup_gate.py` (80), `test_updater_helper.py` (140), `test_frozen_updater.py` (41, opt-in with a built helper).
- `test_code_identity.py` (106), `test_linux_deps.py` (205), `test_release_notes.py` (78), `test_guided_setup.py` (943), `test_windows_installer.py`, `test_linux_installer.py`, `test_macos_installer.py` (static packaging contracts), `test_multiprocessing_startup.py`.

Pure logic whose input/output vectors can be generated from Python and replayed against C++ (golden vectors):
1. `parse_version` / `compare_versions` / `parse_strict_version` / `normalize_version` (table of strings -> tuple/status/exception).
2. `seconds_until_auto_check` (settings dict + now + interval + channel -> seconds or None).
3. `parse_release_payload`, `select_newest_release`, `_parse_named_asset`, `_digest_to_sha256` (JSON in, ReleaseInfo out) - feed the real GitHub shapes and the corrupted variants from the tests.
4. `_redirect_url_allowed`, `_parsed_url_is_well_formed`, `_is_loopback_feed_url`, `_asset_url_allowed` (URL pairs -> bool).
5. `_fetch_release_checksums` line parser; `_is_rate_limited` / `_remember_api_rate_limit` (header sets + now -> retry_at).
6. `validate_archive_member_name`, `validate_managed_topology`, `validate_manifest`, `_check_journal_*` (inputs -> ok / error message).
7. `render_release_notes_html` (markdown -> HTML string, with palette substitution stubbed).
8. `provider_summary`, `shortcut_help`, `should_invite` / `initialize` decision table (existing files layout -> state), `update_check_failure_status`, dialog text functions (`_title_text`, `_body_text`, `_hint_text`, labels) as data tables.
9. `linux_deps.detect_package_family`, `install_command`, `meeting_audio_remediation(reason, family, stack, packaged)` (large deterministic text table, 10 reasons x 4 families x stacks).
10. Journal JSON round trip (`UpdateJournal.to_dict/from_dict`) and manifest generation for a fixture tree (stable sorted JSON).

## Porting notes

### Suggested decomposition
Qt-free core (no Qt, builds with a compiler alone, like `src/engine`), new `src/update/`:
1. `update_contract.{h,cpp}` - constants, version parsing, asset/path names, transaction ids (~250 lines).
2. `release_feed.{h,cpp}` - GitHub payload model and parsing, track selection, asset recognition, URL trust rules, checksum parser, rate-limit logic; HTTP behind an interface `HttpClient { get, head, download-with-range }` so tests use fakes (~700 lines).
3. `update_schedule.{h,cpp}` - `seconds_until_auto_check`, persisted timestamps via a small `SettingsStore` interface (~150).
4. `verified_download.{h,cpp}` - resumable, size+SHA-256 verified download (shared with model/runtime downloads; likely owned by another area - agree on one implementation) (~400).
5. `native_update/` (Windows only, `#ifdef _WIN32`, compiled into both the app and a tiny `fiorivoce-updater.exe` target that does not link Qt): manifest, safe tar.xz extraction, journal, mutexes, registry, commit/rollback/recover, data snapshot (~2000-2500 lines; this is the bulk). Share the code via a static lib `fv_update_native`.
6. `macos_update.mm` (Objective-C++, mac only: hdiutil/codesign/ditto/renamex_np/LaunchServices) (~600).
7. `setup_update` (Inno helper actions) only if Inno stays (~250).
8. `code_identity` (~80), `linux_preflight` (~250 incl. tables).
9. Guided-setup domain `guided_setup_state.{h,cpp}` (eligibility + speech readiness via engine) (~150).

Qt layer in `src/app/`: `UpdaterController` (QObject, schedules checks with `QTimer`, runs core on `QtConcurrent`/worker thread, attempt tokens, signals mirroring `UpdaterRuntime`), `AppUpdateDialog` (+ `NotesView` fade widget, release-notes renderer producing HTML for `QTextBrowser`), `UpdateNoticeButton`, tray action, About status line, restart prompt box, `GuidedSetupDialog` (+ `CleanupChoices`, projection of the Settings dialog), startup health acknowledgement, early update gate in `main()` (before `QApplication`).

Estimated C++ size: core feed/schedule/download ~1500, Windows native updater ~2300, mac ~600, setup/Inno helper ~250, code identity + Linux preflight + remediation text ~450, update dialog + notice + release notes + controller ~1100, guided setup window ~1400, self-test/smoke ~250, tests ~2500. Total roughly 8-10k lines including tests (about 6-7k without tests) if the full Windows transaction design is kept; ~4k without the native tar.xz updater (setup-only Windows path, notify-only elsewhere).

### Order of work
1. `update_contract`, version parsing, release feed parsing + URL trust + checksum parser + schedule (pure, golden-vector tested). 2. `HttpClient` on `QNetworkAccessManager` and verified download. 3. Controller + notice + dialog with notify-only behavior (open release page) on all platforms - ships the visible feature. 4. Settings tiles/keys (with the settings area). 5. Guided-setup state + dialog (depends on the Settings dialog, cleanup providers, mic check and hotkey capture of other areas, so it lands late). 6. Decide installer strategy, then Windows setup-fallback apply (download verified installer, silent run, handoff). 7. Windows native transaction updater (only if owner wants it). 8. macOS in-place. 9. Code-identity restart prompt, Linux preflight, smoke/self-test. Packaging (A16-46) is a prerequisite for 6-8 and should be settled at the start of that sub-project.

### Risks
- Biggest: a bug in the Windows swap/rollback logic bricks a user's install. The Python code is heavily test-pinned (about 2,500 test lines) and hard-won (lock retries, pid identity, bridge floor); a straight port needs the same test matrix, and a real-VM test of power-loss between renames. Consider shipping notify-only + silent Inno setup first.
- Process-identity, mutex and handoff semantics are subtle (`QApplication::exit` vs `quit`, 10 s hard-exit watchdog, health launch with main window disabled, readiness error path). Keep all of them.
- Archive safety: a C++ tar.xz extractor must re-implement every member check (case-fold duplicates, reserved names, depth, manifest authorization). Prefer libarchive in a "no-extract-to-disk" mode and write files myself.
- `QNetworkAccessManager` redirect handling must be manual to re-validate hosts and keep HEAD; Qt is not synchronous - the download worker and cancel token need care, and HEAD/Range/416 behaviour must be tested against a local server (the soak feed script is the template).
- ABI/layout: C++ build has no `_internal` tree and no PyInstaller; manifest topology (`MANAGED_TOP_LEVEL_*`) must be redefined to match the real deployment layout (exe, DLLs, `platforms/`, `styles/`, translations...) - top-level files are enumerated, so every new DLL needs a decision.
- Guided setup is a projection of the Settings dialog internals (private members, `QSignalBlocker` round-trips to resync after failed saves); port the Settings dialog first or define explicit interfaces, or the setup window will duplicate save logic.
- GitHub rate limiting: unauthenticated API gives 60 requests/hour/IP; the fallback path is real, not theoretical.
- Time zone/format: store UTC ISO-8601 exactly as `datetime.isoformat()` (`+00:00` suffix, microseconds) if sharing a settings file with the Python app is ever needed; otherwise freely re-choose.

### What native code can do better
- No lazy-import problem: the code-identity restart prompt shrinks to "installed executable replaced while running", which a native updater already handles by exiting; keep only if the Linux package manager can replace the binary under a running app (it can).
- Startup: the gate before Qt (`maybe_exit_if_update_in_progress`, recovery dispatch) is a few Win32 calls in `main()`, not a Python import chain; a launch check delay of 8 s is no longer needed to avoid import cost (keep the delay to stay out of the startup path anyway).
- Memory/speed: streaming SHA-256 and extraction without Python overhead; the `os.walk` + hash of the whole tree (slow part noted in the cancel comment) becomes I/O bound.
- Smaller helper: a ~300 KB static C++ updater instead of a second PyInstaller onedir copy plus the "restart from the updates root" dance.
- Use `QSaveFile`/`ReplaceFileW` for atomic JSON, `QLockFile`/named mutex with one abstraction across platforms; `QStorageInfo` for space; `QCryptographicHash` for digests.
- Single source of truth for version/branding: CMake `project(VERSION)` generating `version.h` (already `src/engine/version.h.in`) and the updater asset names.

### Open questions (owner decisions)
1. Release/repo identity: GitHub repository for FioriVoce releases, asset naming, new Windows `AppId` GUID, mutex names, and whether to keep OpenWhisper's names for a coexisting install (likely new).
2. Windows update strategy: keep the full native tar.xz + helper transaction, or only the silent-Inno-setup path (plus notify on other platforms), or another packager (MSIX/WiX/Qt IFW)?
3. Is Linux/macOS update scope notify-only (as in Python) or should macOS in-place and Linux AppImage updates be built?
4. Beta track and the "development build" status: keep both (they are cheap) given FioriVoce will start at 0.1.x?
5. Cloud/remote engines (undecided): affects `speech_model_name` returning None and the Clean-up provider rows in guided setup; also whether Meeting Mode (and so the Linux audio remediation text, A16-43) is ported at all.
