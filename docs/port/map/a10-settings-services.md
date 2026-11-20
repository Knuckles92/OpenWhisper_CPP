# Settings store, setting types, credentials, backup and restore

Area `a10-settings-services`. Source (relative to `D:\coding\whisper_local`):

| File | Lines | One line |
|---|---|---|
| `services/settings.py` | 2151 | JSON settings file manager, every `SettingsKey` declaration, value enums, resolvers, install-time migrations |
| `services/setting_types.py` | 381 | Typed `Setting` kinds (bool/int/float/str/choice/list/dict/optional/raw) with read-time validation |
| `services/credentials.py` | 591 | API keys in the OS credential store; resolution order store -> env -> `.env` |
| `services/backup.py` | 1426 | `.owbackup` ZIP export, inspection, staged restore, offline apply with journal and rollback |
| `services/backup_manager.py` | 352 | `BackupCoordinator` QObject: worker thread, progress, schedule (daily/weekly), retention |
| `services/backup_startup.py` | 236 | Cross-process data-root lease (shared/exclusive file lock), restore-at-startup gate, restart spawn |

No UI lives in this area. The Settings "Backup" and "API keys" pages that drive it are in `a11-settings-dialog-ui`
(`ui_qt/dialogs/settings_backup.py`, `ui_qt/dialogs/settings_api_keys.py`); the restart-on-restore wiring is in
`a03-ui-controller` and `a01-app-shell` (`main.py`).

## What the user gets

- Every preference persists across launches in one human-readable JSON file, `openwhisper_settings.json`, in the
  data root. A corrupt or unreadable file never crashes the app: readers see "no settings" (defaults), and writes
  refuse rather than overwrite the broken file.
- Settings read defensively: any stored value of the wrong type or outside its allowed set silently reads as the
  default. Old saved names keep working (legacy engine names, renamed overlay looks, the old `hf_hub_offline` flag).
- One-time install migrations: a new install gets casual chat tones, keeps the engine it started with, and decides
  once whether meetings use a separate speech engine.
- API keys (OpenAI, OpenRouter, Groq, OpenCode, TypeSafe, ...) are saved in Windows Credential Manager / macOS
  Keychain / Secret Service under service name `OpenWhisper`, never in the JSON file or the log. Settings shows only
  the last four characters (`••••abcd`) and where the active key comes from (saved, environment, `.env`).
- A locked/unresponsive Linux keyring never hangs the splash: calls time out after 4 s (120 s once an unlock prompt
  is showing), and the app then reports "The system keyring did not respond. Saved API keys are unavailable."
- Backup: Settings -> Backup writes a portable, checksummed `.owbackup` file (with or without recordings). Secrets,
  pairings and network opt-ins are stripped; restoring never turns on automation that can act for the user.
- Restore: pick a `.owbackup`, it is inspected, then staged; the app restarts (exit code 73) and the restore is
  applied before settings or SQLite open. The previous data is kept in `.openwhisper-restore/previous-*`, and a crash
  mid-restore rolls back on next launch.
- Optional automatic backups (Off / Daily / Weekly) to a chosen folder, keeping 1-100 (default 5) of them; failures
  retry after 10 minutes.
- A second OpenWhisper process on the same data cannot start while a restore is pending, and a restore cannot apply
  while another process uses the data.

## Feature inventory

| ID | Feature | Exact user-visible behavior | Source | Disposition |
|---|---|---|---|---|
| A10-01 | JSON settings file | Path `<data_root>/openwhisper_settings.json`; written `indent=2` + trailing newline, UTF-8; atomic temp `.openwhisper_settings.json.*.tmp` in same dir, fsync, `os.replace` | `settings.py:SettingsManager._save_all_settings_unlocked` (1079-1113) | port |
| A10-02 | Tolerant load | Missing file -> `{}`; any error (not UTF-8, not JSON, not an object, directory in place, too deep) -> `{}` and warning `"Failed to load all settings: ..."` | `settings.py:_load_all_settings_unlocked` (1054-1077) | port |
| A10-03 | Strict read-modify-write | `update_settings`/`mutate_settings`/`save_setting` re-raise on a malformed file, leaving it byte-identical | `settings.py:mutate_settings` (1257-1275) | port |
| A10-04 | Cached reads keyed on file signature | Re-read only when (abs path, dev, ino, size, mtime_ns, ctime_ns) changes; picks up outside edits and atomic replaces; readers always get copies | `settings.py:_file_signature`, `_read_unlocked`, `load_all_settings`, `load_settings` (1042-1174) | port |
| A10-05 | Thread-safe transactions | One RLock covers whole read-modify-write; concurrent writers lose no keys | `settings.py:SettingsManager.__init__` (1012-1015) | port |
| A10-06 | Settings watchers | `watch(keys, cb)` -> `cb(changed_keys)` after a save through this manager changes one of `keys`; runs on the saving thread after the file is replaced; exceptions logged `"A settings watcher failed"`; returns an unsubscribe fn | `settings.py:watch`, `_watched_values`, `_tell_watchers` (1176-1217) | port |
| A10-07 | New-install detection | `is_new_install()` true only if the settings file did not exist when the manager was built and the path is unchanged | `settings.py:is_new_install` (1033-1040) | port |
| A10-08 | Typed setting kinds | Bool/Int/Float/Str/Choice/IntChoice/List/Dict/OptionalList/OptionalDict/Raw with documented fallbacks (see Modules) | `setting_types.py` | port |
| A10-09 | Setting declarations | ~190 keys with types, defaults, ranges, aliases (Data and formats) | `settings.py:SettingsKey` (485-968) | port (engine-only keys: drop) |
| A10-10 | Defaults table | `SETTING_DEFAULTS` = defaults of every `in_table` setting; read-only | `settings.py:994-996` | port |
| A10-11 | Lookup by bare key | `SETTINGS_BY_KEY[key]` for backup, MCP settings tools, `resolve_bool_setting` | `settings.py:1465-1467` | port |
| A10-12 | Value enums and labels | Display labels for font scale, theme, overlay looks (+details), streaming layout, walkthrough copy/snap/page size, meeting languages (29 entries) | `settings.py:101-465` | port |
| A10-13 | Hotkey settings | `load_hotkey_settings()` returns saved `hotkeys` dict or `config.DEFAULT_HOTKEYS` copy (whole dict, no per-key merge) | `settings.py:1285-1304` | port |
| A10-14 | Engine selection load/save + legacy API migration | Saved `api_whisper`/`api_gpt4o`/`api_gpt4o_mini` rewritten to `selected_model:"api"` + `api_transcription_model`; invalid -> `config.DEFAULT_BACKEND`; save rejects unknown with `"Invalid model '<x>'. Valid models: [...]"` | `settings.py:load_model_selection`, `save_model_selection` (1306-1339) | port, but backend set shrinks (see Engine-specific) |
| A10-15 | API model retirement | Before 2027-02-26 picker shows `"<model> (retiring Feb 26, 2027)"` for gpt-4o-transcribe, gpt-4o-mini-transcribe, whisper-1; after, they are hidden and resolve to `gpt-transcribe` | `settings.py:api_model_choices`, `api_model_label`, `serving_api_model`, `resolve_api_transcription_model` (55-92) | undecided backend (cloud API) |
| A10-16 | Hugging Face access policy | `ask`/`always`/`never`; legacy `hf_hub_offline:true` -> `never`, else `ask`; migration persisted and legacy key removed; env `HF_HUB_OFFLINE` in {1,on,true,yes} is a hard override | `settings.py:load_hf_access_policy`, `save_hf_access_policy`, `is_hf_hub_offline_env_set` (1341-1395) | port (Parakeet model download) |
| A10-17 | New-install seeding | Only on a new install, once: writes `app_style_tones = services.app_styles.NEW_INSTALL_TONES` | `settings.py:seed_new_install_settings` (1401-1424) | port |
| A10-18 | Keep unchosen engine | New install saves `config.DEFAULT_BACKEND`; existing Linux install with no saved engine and default `parakeet` saves `local_whisper` | `settings.py:keep_unchosen_engine` (1427-1460) | adapt (OpenWhisper_CPP only has Parakeet) |
| A10-19 | Meeting speech resolution | `resolve_meeting_speech` -> {source, model, separate, note}; note text `"<name> can't transcribe meetings, so meetings use <label>."` | `settings.py:MeetingSpeech`, `resolve_meeting_speech`, `own_meeting_speech`, `_own_meeting_model`, `meeting_model_label` (1774-1916) | port, simplified to Parakeet/remote |
| A10-20 | Keep separate meeting speech | One-time: existing install whose own meeting engine differs from dictation's keeps `meeting_speech_separate=true`; new installs false | `settings.py:keep_separate_meeting_speech` (1847-1880) | port (simplified) |
| A10-21 | Text-LLM provider/model pair resolution | Provider kept only if a known profile (built-in or `custom_*`) and model non-blank; installed agents (claude_code, codex, opencode_cli) accept empty model; else defaults; `openai` + `gpt-4o-mini` reads as `gpt-6-luna` | `settings.py:_resolve_text_llm_assignment`, `resolve_transcript_cleanup_provider/model`, `resolve_meeting_llm_*`, `default_transcript_cleanup_model` (1664-1971) | port |
| A10-22 | Cleanup rules and prompt composition | Rules: list only, strings stripped, blanks dropped, capped at 50; composed as `"<base>\n\nAdditional user-taught rules (always apply):\n1. ..."` | `settings.py:resolve_transcript_cleanup_rules`, `compose_transcript_cleanup_prompt`, `resolve_transcript_cleanup_prompt` (1649-1766, 2124-2132) | port |
| A10-23 | Recording retention resolvers | count limit only in `custom` mode; bytes = MB*1024*1024 only in `size_limit` mode | `settings.py:resolve_max_saved_recordings[_bytes]` (1618-1640) | port |
| A10-24 | Theme resolution | No saved theme on an Omarchy desktop -> `omarchy` | `settings.py:resolve_ui_theme` (1524-1532) | port (Linux) |
| A10-25 | Meeting misc resolvers | agent models per installed agent; speaker-id `openai` -> `local` after retirement; Linux preview ack versioned (must equal 1); voice command names (defaults `note taker, notetaker, assistant, openwhisper, open whisper`, lowercased, whitespace-collapsed); context folder path (`~` expanded, absolute, normalized); report views (fallback `("ribbon",)`); insight review dict | `settings.py:1974-2151` | port |
| A10-26 | TypeSafe feature gates | Feature switches AND master `typesafe_enabled` | `settings.py:resolve_typesafe_*` (2032-2079) | port |
| A10-27 | Credential store | `get/has/set/delete/request_access/status/refresh`; cache of lookups; service `OpenWhisper`, account = variable name | `credentials.py:CredentialStore` (264-430) | port |
| A10-28 | Platform backends picked by name | Windows Credential Manager (WinVault), macOS Keychain, Secret Service; never keyring's plugin discovery | `credentials.py:platform_backend` (104-127) | port |
| A10-29 | Secret Service lookup-only reads | Reads/deletes search all collections, never create a keyring; unlocked copies preferred; only a locked match prompts | `credentials.py:_secret_service_backend_class` (176-257) | port (Linux) |
| A10-30 | Keyring timeouts | 4.0 s per D-Bus call, extended to 120 s once a prompt shows; on timeout store marked unavailable with reason `"the system keyring did not respond"` | `credentials.py:_run_with_timeout` (133-166), `_mark_unresponsive` | port (Linux) |
| A10-31 | Failed read is cached | A failed/denied read caches None; not retried until Save/Remove/explicit access request | `credentials.py:CredentialStore.get` (329-356) | port |
| A10-32 | Key validation | Strip; empty -> `"Paste a key first."`; >512 -> `"That is longer than any API key (512 characters max)."`; any char not in 0x21..0x7E -> `"The key contains a space or a character outside plain ASCII. Copy it again from the provider's dashboard."` | `credentials.py:validate_api_key` (466-487) | port |
| A10-33 | Key mask | `••••` + last 4 chars, or `••••` alone when shorter than 12 | `credentials.py:mask_key` (490-494) | port |
| A10-34 | Resolution order and source | stored -> process env -> `.env` (loaded into env without overriding); source enum stored/environment/dotenv/none; `environment_shadowed` | `credentials.py:resolve_credential`, `credential_source`, `environment_shadowed` (550-591) | port |
| A10-35 | Error copy | `"<backend> is not available: <reason>"`, `"Couldn't save the key to <backend> (<ExcType>)."`, `"Couldn't remove the key from <backend> (<ExcType>)."`, `"Couldn't access <backend> (<ExcType>)."`; reason `"the keyring package is not installed"` | `credentials.py:299-430` | port |
| A10-36 | Create backup | `.owbackup` ZIP: manifest + sanitized settings + SQLite online-backup snapshot + owned files; atomic publish; progress phases `scan`, `archive`; cancellable | `backup.py:create_backup` (766-843) | port |
| A10-37 | Settings sanitizing for backup/import | Removes pairings, context folder path; forces network/automation opt-ins off; disables saved webhooks/programs/filters and webhook/program/agent actions; drops secret-named keys; strips userinfo/query/fragment from http(s) URLs except in `dictation_snippets` and link-action templates | `backup.py:_scrub`, `_sanitize_actions`, `_sanitize_settings` (263-360) | port |
| A10-38 | Database sanitizing | Meeting spool/chunk paths rewritten to `__OW_BACKUP_DATA__/meetings/...`; host/guest tokens regenerated; remote/agent endpoint, pid, heartbeat cleared; `record_sync` emptied; text-only drops audio chunks and history `audio_file`; warnings `"History entry <id> refers to external audio"`, `"History entry <id> has no local audio file"` | `backup.py:_DatabaseSanitizer` (449-583) | port |
| A10-39 | Legacy history merge | If DB history table empty and `transcription_history.json` exists, its entries are inserted into the snapshot | `backup.py:_merge_legacy_history` (586-626) | port (only if OpenWhisper_CPP imports OpenWhisper data) |
| A10-40 | Inspect backup | Validates ZIP structure, names, sizes, ratios, checksums, manifest, settings JSON, SQLite integrity and every path reference | `backup.py:inspect_backup`, `_inspect_backup`, `_validate_manifest` (852-1067) | port |
| A10-41 | Stage restore | Validates, extracts to `.openwhisper-restore/stage-<32hex>/data`, rebases DB paths to this root, re-sanitizes settings, writes stage manifest and `pending.json` | `backup.py:prepare_restore` (1145-1198) | port |
| A10-42 | Apply restore at startup | Journaled move of owned names into `previous-<UTCstamp>-<8hex>`, staged in; rollback on failure; crash recovery from journal; `last_restore.json` records previous | `backup.py:apply_pending_restore`, `_resume_journal`, `_install_stage`, `_rollback` (1311-1426) | port |
| A10-43 | Previous-data location | Shown in Settings after a restore | `backup.py:previous_data_dir` (1133-1142) | port |
| A10-44 | Backup coordinator | One worker; signals progress (0-1000), backup_finished, inspection_finished, restore_staged, schedule_changed, busy_changed; refusal `"Another backup operation is already running."` | `backup_manager.py:BackupCoordinator` | port |
| A10-45 | Automatic backups | Off/Daily(86400 s)/Weekly(604800 s); checked every 60 s; file `OpenWhisper-auto-YYYYMMDDTHHMMSSZ-<12hex>.owbackup`; retention keeps newest N of recorded auto archives only; retry 600 s after failure | `backup_manager.py:_scheduled_tick`, `_prune_automatic_backups`, `set_schedule` | port |
| A10-46 | Backup preferences file | `<data_root>/backup-preferences.json` (not in settings JSON) | `backup_manager.py:_load_preferences`, `_save_preferences` | port |
| A10-47 | Data lease | `.openwhisper-data.lock` byte-range lock (1 byte), shared for normal processes, exclusive for restore; `"The application data is in use by another OpenWhisper process."` | `backup_startup.py:DataLease` | port |
| A10-48 | Startup restore gate | Up to 4 attempts; error copy (see Modules) | `backup_startup.py:acquire_startup_data_lease` | port |
| A10-49 | Restart after restore | Exit code 73; relaunch with no args; PyInstaller env cleanup | `backup_startup.py:RESTART_EXIT_CODE`, `restart_command`, `_restart_environment`, `spawn_restart` | port (drop PyInstaller env parts) |

## Modules

### services/setting_types.py

Responsibility: a settings key that is also its JSON key string, with type, default and validation.

- `Setting(str, Generic[T])` (55-127): `key`, `in_table`, `default` (deep copy each time; `test_defaults_are_fresh_copies`),
  `parse(raw)` (value or default), `read(settings=None)` (unset -> default; `None` loads the file via
  `settings_manager.load_all_settings()`), `stored(settings=None)` (unset or invalid -> `None`, never the default).
  Copy/deepcopy return self; pickle stores a bare `str` (73-80).
- Kinds and exact rules (port these exactly; they are pinned by `test_typed_settings.py:test_kinds_validate_as_documented`
  and the equivalence battery):
  - `BoolSetting` (130-134): only a real JSON bool; `0`, `"yes"`, `None` -> default.
  - `IntSetting` (137-185): non-strict = Python `int()` semantics: accepts `"12"`, `3.7` (truncates to 3), `True` (1),
    `"  12 "`; rejects `"3.7"`, `"x"`, `None`, lists, Infinity (OverflowError, 175). `strict` = JSON integer only, not
    bool. `valid=(lo,hi)` out of range -> default; then `clamp=(lo,hi)` pulls into range. Either bound may be None.
  - `FloatSetting` (188-194): JSON number, not bool.
  - `StrSetting` (197-221): `str` only; optional `normalize` applied to stored value only (default used as declared).
  - `ChoiceSetting` (224-260): `str` only; alias lookup on the raw string before normalize; then normalize; must be in
    `choices`. Constructor raises `ValueError("<key>: default ... is not one of its choices")` if default not a choice.
  - `IntChoiceSetting` (263-298): int via `int()` (or strict), must be in choices.
  - `ListSetting`/`DictSetting` (301-323): type check only, items validated by the owning module.
  - `OptionalListSetting`/`OptionalDictSetting` (326-351): unset -> `None`; wrong type -> empty container (not default!).
    Always `in_table=False`.
  - `RawSetting` (354-365): value unchanged, `None` when unset.
- Python `int()` corner cases a C++ port must replicate for non-strict ints: bool -> 0/1; float -> truncate toward
  zero; string -> optional surrounding whitespace, optional sign, decimal digits, underscores between digits allowed
  (`"1_000"`), no decimal point. Golden vectors are the safest way to pin this.

### services/settings.py

Responsibility: the settings store singleton, the declaration catalogue, value enums with UI labels, resolvers, and
install-time migrations.

Public surface:
- `SettingsManager(settings_file=None)` (1007-1386): `is_new_install`, `file_signature`, `load_all_settings`,
  `load_settings(keys)`, `get(key, default)`, `watch(keys, cb) -> stop`, `save_all_settings(dict)`,
  `update_settings(updates, remove=())` (returns committed copy), `mutate_settings(fn)` (fn edits in place; result
  returned; raising fn writes nothing), `save_setting(key, value)`, `load/save_hotkey_settings`,
  `load/save_model_selection`, `load/save_hf_access_policy`.
- Module singleton `settings_manager = SettingsManager()` (1398), built at first import, after the startup restore.
- `SettingsKey` (485-968), `ALL_SETTINGS`, `SETTING_DEFAULTS`, `SETTINGS_BY_KEY`, `LEGACY_STREAMING_KEYS` (973-978:
  `streaming_overlay_enabled`, `streaming_paste_enabled`, `streaming_tiny_model_enabled`, `live_typing_enabled` must be
  dropped whenever the live-preview toggle is written; the writer is elsewhere).
- Enum classes with labels (101-465): `RecordingRetentionMode`, `RecordingTriggerMode`, `UiFontScale` (90 Small, 100
  Default, 115 Large, 130 Extra large), `UiTheme` (Dark, Light, Match system, Omarchy desktop), `WalkthroughCopyFormat`
  (PDF copy; Agent (PDF + instructions); PDF and Markdown copy; Markdown copy; Word document copy), `WalkthroughSnapMode`
  (Region; Window under the pointer; Whole screen; In 3 seconds), `WalkthroughPageSize` (Automatic (Letter or A4 for your
  region); Letter; A4), `RecordingOverlayLook` (11 looks with LABELS, DETAILS and ALIASES live->ribbon, orb->pulse,
  bubbles->baitball), `StreamingOverlayLayout` (Header "Clock and a small meter above the words", Footer "The full look
  under the words"), `TranscriptCleanupProvider`, `TranscriptCleanupLevel`, `TranscriptCleanupModelSort` (values are
  OpenRouter `sort` query values), `TranscriptCleanupReasoning`, `MeetingAgentCore` (`direct` legacy reads as invalid
  -> default `pi`), `TypeSafeProvider`, `TypeSafeTextModel`, `MeetingSpeakerIdBackend`, `MeetingLanguage` (29 code/label
  pairs, 412-442), `MeetingServerBind`, `HuggingFaceAccessPolicy`.
- Constants: `MCP_DEFAULT_PORT=8767`, `REMOTE_HOST_DEFAULT_PORT=47821`, `WALKTHROUGH_CAPTURE_DELAYS=(0,1,2,3,5,10)`,
  `MEETING_LINUX_PREVIEW_ACK_VERSION=1`, `INSIGHT_REVIEW_CONSENT="typesafe-text-v1"`, `DEFAULT_VOICE_COMMAND_NAMES`.
- Resolvers (1470-2151): `setting_value` (raw, unvalidated; `KeyError` for non-table keys), `resolve_bool_setting(key)`
  (raises `TypeError("<key> is not an on/off setting")`), `resolve_choice_setting(key, choices)`, plus ~70
  `resolve_*` aliases bound to `SettingsKey.X.read` (1515-1615) and the derived ones listed in the inventory.

Threading: `RLock` around every file access; the watcher callbacks and `_tell_watchers` run outside the lock on the
saving thread (1204-1217, 1274). Any thread may read or write. Qt consumers bridge watcher callbacks to the GUI thread
themselves (`ui_qt/utils/engine_selection.py:52` relays through a signal).

Cache design (1016-1023, 1129-1158): the parsed dict is cached with the file signature; `load_all_settings` hands out a
pickle round-trip copy (perf note: 0.10 ms vs 0.70 ms deepcopy for 45 KB). `load_settings(keys)` deep-copies only the
requested keys. If the file changed during the read (signature before != after), the result is not cached (1141-1147).
A save calls `_forget_cached_unlocked` instead of caching the written dict (1102-1104).

Edge cases a port must keep:
- `_load_all_settings_unlocked` catches every exception in non-strict mode (1069-1077); strict mode re-raises
  (`test_unreadable_settings_load_as_empty_and_block_writes`, `test_settings_path_that_cannot_be_read_loads_as_empty`).
- Failed `os.replace` leaves the original file and removes the temp file (1106-1113; `test_failed_atomic_replace_preserves_previous_settings`
  also asserts no stray file remains in the directory).
- `save_all_settings` takes the "before" snapshot for watchers non-strictly (1223-1224).
- Watcher change detection compares before/after with `!=` and a sentinel for absent keys (1197-1207); the callback
  receives only the intersection of its keys and the changed set.
- `load_model_selection` writes during a read (migration) (1311-1316).
- `load_hf_access_policy` persists only if a legacy key or an (invalid) policy value existed (1363).
- `keep_unchosen_engine`, `keep_separate_meeting_speech`, `seed_new_install_settings` tolerate stand-in managers without
  `is_new_install` (return False) and swallow write failures with a warning.
- `_own_meeting_model` reads `selected_model` raw (not validated) on purpose (1909-1913).
- `resolve_meeting_context_folder_path` does not require the folder to exist.
- `_log_fallback` logs a repeated fallback warning once, then at debug (35-46).

### services/credentials.py

Responsibility: API keys and tokens in the OS per-user credential store; resolution across store, env, `.env`.

Public surface: `SERVICE_NAME="OpenWhisper"`, `MAX_API_KEY_LEN=512`, `SECRET_SERVICE_TIMEOUT_S=4.0`,
`SECRET_SERVICE_PROMPT_TIMEOUT_S=120.0`, `KEYRING_NO_RESPONSE`, `UNRESPONSIVE_KEYRING_STATUS`; `CredentialSource`;
`CredentialStoreStatus(available, backend_name, reason)`; `CredentialStoreError`, `CredentialStoreTimeout`;
`CredentialBackend` protocol; `MemoryCredentialBackend`; `platform_backend_name()`, `platform_backend()`,
`memory_backend()`; `CredentialStore`; `store()`, `set_store()` (tests); `keyring_did_not_respond()`;
`validate_credential_name`, `validate_api_key`, `mask_key`; `resolve_credential`, `credential_source`,
`environment_shadowed`.

Threading: `CredentialStore` methods hold an RLock (278) for the whole backend call, so a slow store blocks other
credential callers. Secret Service calls run on a daemon worker thread with timeout; the timed-out thread is abandoned
(158-162). `_prompting` thread-local signals that a prompt is showing (130, 169-173).

State: `_status` probed lazily once; `refresh()` and `request_access()` reset it; `_mark_unresponsive` sets unavailable
with `KEYRING_NO_RESPONSE`. `_cache[name]` holds value or None (including failures, 350-355). `set` writes cache;
`delete` returns False when nothing was stored (catches keyring `PasswordDeleteError`).

Edge cases: logs never contain the value, only names and exception type names (`test_secret_never_reaches_the_log`);
`request_access` re-probes the store and raises on failure while caching None (361-382); `.env` parse is cached on
(path, mtime_ns, size) (497-525); `.env` entries are copied into the process env via `process_env.load_for_app`, never
overriding an existing different value, and an entry an environment cannot hold is skipped and logged once by name
(532-547); `credential_source` reports `dotenv` when the env value equals the `.env` value (576-581).

### Added by check

Credentials (`services/credentials.py`):
- `validate_credential_name(name)` (459-463): strips; empty or whitespace-only raises `ValueError("Credential name is
  empty")`. Applied to every store call's variable name. Unlike `validate_api_key` it does no charset check.
- `mask_key` threshold is `_MIN_MASK_SUFFIX_LEN = 12` (line 31): keys under 12 characters mask to `••••` alone.
- `DEFAULT_BACKEND` (consumed by `selected_model`, A10-14/18) is platform dependent: `parakeet_mlx` on Apple Silicon
  macOS 14+, `parakeet` on Windows x64 / Linux x86_64, else `local_whisper` (config.py:804-808). OpenWhisper_CPP fixes it to
  `parakeet`.
- Secret Service writes (`set_password`) are expected to prompt (create or unlock a keyring) and call `_note_prompt()`
  so the 120 s window applies; reads and deletes never create a keyring (credentials.py:225-255).

Meeting speech (`services/settings.py` 1883-1892): `resolve_meeting_asr_source(settings)` and
`resolve_meeting_whisper_model(settings)` are thin views over `resolve_meeting_speech(...).source/.model` (A10-19), so
callers of the old names must keep working. `resolve_meeting_llm_profile` returns the profile for the resolved provider
(`get_profile` or `builtin_profile`); `resolve_meeting_llm_endpoint` returns the non-secret snapshot dict from
`snapshot_from_profile(profile, model).to_dict()` (1940-1959).

`resolve_typesafe_feature_enabled(feature, settings)` takes one of `citations`, `semantic_search`, `question_radar`,
`highlights` (a `KeyError` for anything else) and ANDs it with `typesafe_enabled`; `resolve_typesafe_topic_shift_enabled`
and `resolve_typesafe_voice_commands_enabled` follow the same master-switch rule (2032-2079).

Keys the table abbreviates by suffix (full key strings): `recording_overlay_dot`, `recording_overlay_text`,
`recording_overlay_clock`; `meeting_report_ribbon`, `meeting_report_brief`, `meeting_report_signal`;
`typesafe_citations_enabled`, `typesafe_semantic_search_enabled`, `typesafe_question_radar_enabled`,
`typesafe_highlights_enabled`; `walkthrough_show_details`, `walkthrough_show_about`, `walkthrough_show_captions`.
Declared key count is 186 (not ~190). Only environment variables read in this area: `HF_HUB_OFFLINE`, plus any
credential name via `os.getenv` and `.env` entries.

### services/backup.py

Responsibility: export, inspect, stage and apply portable backups of owned user data. Pure functions over a data root;
no Qt. All errors are `BackupError` (cancel: `BackupCancelled`), messages user-visible.

Constants (37-82): `FORMAT="openwhisper-backup"`, `FORMAT_VERSION=1`, `ARCHIVE_EXTENSION=".owbackup"`,
`MANIFEST_NAME="manifest.json"`, `RESTORE_DIR=".openwhisper-restore"`, `PENDING_NAME="pending.json"`,
`JOURNAL_NAME="journal.json"`, `LAST_NAME="last_restore.json"`, `PATH_MARKER="__OW_BACKUP_DATA__/"`, `OWNED_NAMES`
(13 names), `SQLITE_SIDECARS` (`-wal`, `-shm`, `-journal`), `RECORDING_NAMES`, `LEGACY_HISTORY_NAMES`,
`MAX_FILES=100000`, `MAX_TOTAL_BYTES=1 TiB`, `MAX_FILE_BYTES=64 GiB`, `MAX_MANIFEST_BYTES=8 MiB`, `MAX_RATIO=10000`,
`CHUNK_SIZE=1 MiB`, free-space margin 32 MiB, member attr `(S_IFREG|0600)<<16`, stage name `stage-[0-9a-f]{32}`,
previous name `previous-[A-Za-z0-9_-]+`.

Public surface: `BackupInfo` dataclass (path, created_at, app_version, schema_version, include_recordings, file_count,
total_bytes, archive_bytes, warnings); `create_backup(dest, *, data_dir, include_recordings=True, progress, cancel)`;
`inspect_backup(path)`; `prepare_restore(path, *, data_dir, progress, cancel)`; `apply_pending_restore(*, data_dir)`;
`pending_restore_exists(data_dir)`; `previous_data_dir(data_dir)`. `progress(phase, done, total)`; `cancel` is a
callable or an object with `is_set()` (threading.Event).

Create flow (766-843): check destination (must end `.owbackup`, not inside any recording folder or the restore dir,
733-738) -> temp work dir `.owbackup-build-*` beside destination -> SQLite online backup snapshot (read-only URI, 256
pages per step, 50 ms sleep, cancel checked per step; refused `"End the live meeting before creating a backup"` if a
`meeting_sessions.status` is active/paused) or a fresh schema when no DB -> merge legacy history -> sanitize DB ->
collect owned files (no symlinks/junctions/special files; `remote_records/cache` skipped) -> read settings under a file
signature -> sanitize settings -> warning `"Host-kept-only records are not in this local archive"` when
`remote_records_location` is host/both -> size and free-space checks -> write ZIP (Zip64; audio suffixes
`.wav .mp3 .flac .ogg .m4a .mp4` stored, others deflated; each member streamed with SHA-256 and source signature
re-checked before and after: `"Data changed during backup: <name>"`) -> manifest -> full `_inspect_backup` of the temp
archive -> sources unchanged -> fsync -> `os.replace` to destination -> fsync dir.

Manifest JSON (821-830): `{"format","format_version","created_at" (UTC ISO, seconds, "+00:00"),"app_version",
"schema_version","include_recordings","files":[{"path","size","sha256"}],"warnings":[str]}` written compact
(`separators=(",",":")`, `ensure_ascii=False`). Member names are `data/<owned path>` plus `manifest.json`.
Settings member is `indent=2` + newline. First two files are always `data/openwhisper_settings.json`, `data/openwhisper.db`.

Validation rules (_validate_manifest and helpers, 852-1062): member names canonical relative POSIX, no `\`, NUL, control
chars, leading `/`, empty/`.`/`..` parts, or `:`; under `data/<OWNED_NAME>`; legacy history and `remote_records` never
allowed in an archive; nested paths only under recording folders; no case-fold duplicates or directory entries; no
symlink/special mode bits; per-file size and compression-ratio limits; manifest file list exactly equals members; core
files present; text-only archive has no recording members; warnings are strings <= 500 chars; schema version int in
[0, current]; settings member <= 8 MiB and a JSON object; DB `quick_check` ok and no `foreign_key_check` rows; no live
meeting; every meeting path is `PATH_MARKER + meetings/...` and every chunk/history audio file is an archive member.

Stage (1145-1198) and apply (1337-1426): see inventory A10-41/42. Apply order is `RESTORE_NAMES` (owned names then
sidecars); rollback runs in reverse and is existence-based, not offset-based (1319-1334). Journal phases `applying` ->
`committed`; `committed` on startup finishes; `applying` rolls back, deletes the journal, then a fresh apply runs
(1377-1400). Staged DB and settings are rewritten during staging, so their expected size/hash come from
`staged_db_*`/`staged_settings_*` manifest keys (1220-1225).

Errors worth keeping verbatim (all `BackupError`): see the strings at 115, 129, 148-176, 188-193, 220-243, 252-259, 389-391,
409, 455, 509, 529-531, 591-619, 663-669, 676, 684-686, 707, 714, 735-738, 748, 757, 763, 790-806, 855-872, 885-903,
909-927, 939-942, 961, 979, 986, 1017, 1025-1027, 1041-1062, 1087-1116, 1129, 1141, 1153, 1160-1167, 1213-1216,
1233-1307, 1329, 1351, 1385-1396. The ones the user most often sees: `"End the live meeting before creating a backup"`,
`"End the live meeting before preparing a restore"`, `"A restore is already waiting for restart"`,
`"Not enough free disk space for a safe backup or restore"`, `"Backup file must end in .owbackup"`,
`"Choose a backup destination outside recorded data"`, `"Unsupported backup format"`,
`"Backup database schema is incompatible with this app"`, `"Backup ZIP is corrupt"`, `"Backup canceled"`.

Lazy imports to respect (379-384, 418-428, 308-309, 846-849): `services.database.SCHEMA_VERSION`, SQLAlchemy
`services.models.Base` (fresh schema when no DB), `services.actions.model.load_actions/ActionKind`, `_version`. Startup
`apply_pending_restore` must not need the database layer or Qt.

### services/backup_manager.py

Responsibility: Qt-facing coordinator (QObject) that runs backup operations on one worker thread and owns the schedule.

Signals (29-35): `progress(str phase, int done, int total)` (done scaled 0..1000, total 1000 or 0), `backup_finished(object
BackupInfo|None, str error)`, `inspection_finished(object, str)`, `restore_staged(str path, str error)`,
`schedule_changed()`, `busy_changed(bool)`, private `_worker_finished` (queued back to the owner thread).

Constructor injection (37-66): `data_dir`, `acquire()` (returns a refusal string or None), `release()`, `service` (the
backup module), `clock`, `worker_context` (context manager the backup/restore runs inside; the controller pauses record
sync and remote-engine records there). `ThreadPoolExecutor(max_workers=1)`, `QTimer` 60 000 ms started by `start()`.

Methods: `busy`, `operation`, `start()`, `snapshot()` -> dict {last_backup, frequency, retention_count, destination,
busy, operation, last_error, previous_data_dir, next_backup_at}; `set_schedule(frequency, retention_count, destination)`;
`create_backup(path, include_recordings=True)`; `inspect_backup(path)`; `prepare_restore(path)`; `cancel()` (backup only;
restore is not cancellable, 191-195); `shutdown()` (stop timer, set cancel, join worker, release).

State machine: idle -> `_claim` (busy/closed -> refusal emitted to manual callers only; inspect skips acquire;
others call `acquire`, a non-empty reason is emitted as the error) -> busy (`busy_changed(True)`) -> worker -> `_finish`
on the owner thread (release, clear busy, store `last_error`, automatic failure sets `_retry_after = now+600`,
`busy_changed(False)`, `schedule_changed`, result signal unless closed).

Progress throttle (260-268): same phase and not complete and < 0.1 s since last -> dropped.

`set_schedule` validation strings (154-180): `"Wait for the current backup operation to finish."`, `"Choose Off, Daily, or
Weekly."`, `"Keep between 1 and 100 automatic backups."`, `"Choose an existing folder for automatic backups."`,
`"Choose a backup folder outside OpenWhisper's recording folders."`; failed save restores previous prefs.

Scheduled tick strings (310-320): `"Automatic backup is waiting for its destination folder to be available."`, `"Choose a
backup folder outside OpenWhisper's recording folders."`. `next_backup_at = max(now, last_auto_at+interval, retry_after)`
(89-96). After success: `last_backup` = BackupInfo as dict; automatic also sets `last_auto_at` and appends to
`scheduled_archives`, then prunes; failure there returns `"Backup saved, but its history or retention could not be
updated: <err>"` (270-287). Prune deletes only recorded files whose parent is the destination, name matches
`OpenWhisper-auto-\d{8}T\d{6}Z-[0-9a-f]{12}\.owbackup`, not a symlink (325-342).

Controller refusals passed in as `acquire` (application_controller.py:757-783, area a03): `"Wait for OpenWhisper to
finish starting before backing up or restoring."`, `"Wait for recording recovery to finish before backing up or
restoring."`, `"Wait for the walkthrough to finish saving before backing up or restoring."`, `"Finish recording or
transcription before backing up or restoring."`, `"Finish the meeting and its processing before backing up or
restoring."`, `"Finish dictating the cleanup rule before backing up or restoring."`; acquire also disables the main
window's central widget until release.

### services/backup_startup.py

Responsibility: process-lifetime shared lease on the data root; pending restore applied under an exclusive lease
before anything opens settings or SQLite; restart helper.

- `DataLease.acquire(data_dir, exclusive=False)` (65-108): root = normcase(realpath(abspath)); creates dir; refuses a
  symlinked lock (`"The application data lock is a symbolic link."`) or non-file (`"The application data lock is not a
  file."`); opens `O_RDWR|O_CREAT|O_BINARY|O_NOFOLLOW` 0600; writes one NUL byte if empty; Windows `LockFileEx` on byte
  0 length 1 with `LOCKFILE_FAIL_IMMEDIATELY` (+ `LOCKFILE_EXCLUSIVE_LOCK`); POSIX `flock(LOCK_SH|LOCK_EX | LOCK_NB)`.
  Busy -> `DataLeaseBusy("The application data is in use by another OpenWhisper process.")`. `close()` unlocks and closes.
- `acquire_startup_data_lease(data_dir)` (140-186): idempotent per root (different root -> `"OpenWhisper already holds a
  different data directory."`); loop up to 4: shared lease; if no pending restore keep it; else release, take exclusive
  (busy -> `"A backup restore is pending, but another OpenWhisper process is still using this data. Close every
  OpenWhisper window and History API process, then launch again to finish the restore."`), apply (False ->
  `"The pending backup restore was not applied. The existing data was left in place; launch again after resolving the
  restore error."`), release, retry shared. Shared busy on first step -> `"Another OpenWhisper process is restoring this
  data. Wait for it to finish, then launch again."`. Exhausted -> `"The backup restore changed repeatedly during startup;
  launch again."`.
- Callers wrap failures (main.py:367-375) as `"Could not safely open application data: <exc> The backup restore
  remains pending. Close other OpenWhisper processes and start the app again."` shown in a critical QMessageBox titled
  `OpenWhisper` (frozen builds) and stderr, exit 1.
- `RESTART_EXIT_CODE=73`; `restart_command` = `[exe]` frozen or `[python, main.py]`, never original argv;
  `_restart_environment` strips `_PYI_*`/`_MEIPASS2`, sets `PYINSTALLER_RESET_ENVIRONMENT=1`, removes `_MEIPASS` PATH
  entries; `spawn_restart` uses `CREATE_NO_WINDOW` on Windows.

## Data and formats

### Files on disk (data root = `config.data_root()`, area a01: `OPENWHISPER_DATA_DIR` env, else per-user dir such as `%LOCALAPPDATA%\OpenWhisper` / `~/.local/share/OpenWhisper`, or a source checkout)

| Path | Owner | Format |
|---|---|---|
| `openwhisper_settings.json` | settings.py | JSON object, indent 2, newline at end |
| `.openwhisper_settings.json.<rand>.tmp` | settings.py | transient atomic-write temp |
| `backup-preferences.json` | backup_manager.py | `{"frequency":"off|daily|weekly","retention_count":1-100,"destination":str,"last_backup":BackupInfo dict or null,"last_auto_at":unix float,"scheduled_archives":[str] (<=1000 kept on load)}`, indent 2; ignored if symlink or > 1 MiB |
| `.openwhisper-data.lock` | backup_startup.py | 1 byte; byte-range lock target |
| `.openwhisper-restore/pending.json` | backup.py | `{"stage":"stage-<32hex>","created_at":str}` |
| `.openwhisper-restore/journal.json` | backup.py | `{"phase":"applying|committed","stage","previous","stage_names":[sorted names]}` |
| `.openwhisper-restore/last_restore.json` | backup.py | `{"previous":"previous-<YYYYMMDDTHHMMSSZ>-<8hex>"}` |
| `.openwhisper-restore/stage-*/manifest.json`, `stage-*/data/...` | backup.py | archive manifest + `staged_db_sha256/size`, `staged_settings_sha256/size` |
| `.openwhisper-restore/previous-*/...` | backup.py | owned data replaced by the last restore (kept indefinitely) |
| `.env` (data root, then source dir) | credentials.py | dotenv; read only |
| `*.owbackup` | backup.py | ZIP (Zip64), see Modules |

Owned names moved by backup/restore: `openwhisper_settings.json`, `openwhisper.db` (+ `-wal`, `-shm`, `-journal` on
restore only), `transcription_history.json[.bak]`, `meetings.json[.bak]`, `recorded_audio.wav`,
`.recorded_audio-recovery`, `recordings/`, `meetings/`, `captures/`, `remote_records/`, `scratchpad.txt`. Not backed up:
credential store, `.env`, TLS key, models, logs, cache, `mcp_event_subscriptions.json`, `backup-preferences.json`.

Database columns touched by backup (tables owned by a09 history / a28 meeting persistence): `schema_version.version`;
`meeting_sessions.{id,spool_dir,status,host_token,guest_token,asr_remote_json,agent_endpoint_json,app_pid,app_heartbeat_at}`;
`meeting_audio_chunks.{id,file_path}`; `meeting_segments.chunk_id`; `transcription_history.{id,text,raw_text,timestamp,model,audio_file,transcription_time,audio_duration,file_size}`
(`audio_file` relative to `recordings/`, or absolute legacy); `record_sync` (emptied). Legacy history JSON:
`{"entries":[{id,text,raw_text,timestamp,model,audio_file,transcription_time,audio_duration,file_size}]}`.

Compatibility: if OpenWhisper_CPP should read an existing OpenWhisper data root or `.owbackup`, keep every name, key and
format above byte-compatible. This is an owner decision (Open questions).

### Credential store entries

Service `OpenWhisper`, account = variable name. Names seen in callers: `OPENAI_API_KEY`, `OPENROUTER_API_KEY`,
`GROQ_API_KEY`, `OPENCODE_GO_API_KEY`, `OPENCODE_ZEN_API_KEY`, `TYPESAFE_API_KEY` (text_llm.py, typesafe.py,
speech_endpoints.py), `OPENWHISPER_REMOTE_ENGINE_TOKEN` (remote_asr/settings.py:24), MCP token
`OPENWHISPER_MCP_TOKEN` (agent_mcp/runtime.py:18), `OPENWHISPER_MCP_EVENT_SECRET_<id>` (events_ext.py:99), webhook
secrets (automation/webhooks.py), API token `OPENWHISPER_API_TOKEN` (agent_api/cli.py:22), channel token
`OPENWHISPER_TOKEN`. Custom text-LLM profiles carry their own variable name. On Windows these are Generic credentials
with target name per keyring's WinVault convention (`<username>@OpenWhisper`, falling back to `OpenWhisper` for legacy);
a C++ port must use the same target to read keys an existing install saved (verify against keyring's
`WinVaultKeyring` source before porting). macOS: generic password, service `OpenWhisper`, account = name. Secret
Service: attributes `service=OpenWhisper`, `username=<name>` (keyring's `_query`), plus `application=Python keyring
library` on items keyring created.

### Settings keys

Kind abbreviations: B bool, I int, F float, S str, C choice, IC int-choice, L list, D dict, OL/OD optional list/dict,
R raw. `*` = `in_table=False` (no `SETTING_DEFAULTS` entry). Disposition: P port, X drop (engine-specific), U undecided
backend. Owner area in brackets is the main reader.

| Key | Kind | Default | Constraints / notes | Disp |
|---|---|---|---|---|
| hotkeys | OD* | None | name -> hotkey string; absent -> `config.DEFAULT_HOTKEYS` (Windows/Linux: record_toggle `ctrl+alt+r`, cancel `ctrl+alt+escape`, enable_disable `ctrl+alt+shift+r`, minimize_tray `ctrl+alt+m`, meeting_toggle "", paste_walkthrough `ctrl+shift+f12`, plus unbound extras; other platform variant uses `kp *`, `kp -`, `ctrl+alt+kp *`; config.py:774-792) [a06] | P |
| selected_model | C | `config.DEFAULT_BACKEND` | choices `local_whisper, api, parakeet, qwen_asr, nemotron, moonshine, parakeet_mlx, parakeet_unified, remote`; aliases `api_whisper/api_gpt4o/api_gpt4o_mini` -> `api` | P (choice set shrinks) |
| api_transcription_model | S | `gpt-transcribe` | valid in `API_MODEL_CHOICES` | U |
| api_transcription_endpoint | S | `openai` | see speech_endpoints.py | U |
| api_transcription_servers | OD* | None | self-hosted server overrides | U |
| audio_input_device | R | — | legacy device index, kept for downgrades [a05] | P |
| window_geometry, compact_window_geometry, host_window_geometry | R | — | geometry blobs [a03] | P |
| compact_mode | B | false | | P |
| host_mode | B | false | | U (remote) |
| auto_paste | B | true | | P |
| macos_accessibility_intro_seen | B | false | | other-platform only |
| copy_clipboard | B | true | | P |
| restore_clipboard | B | false | | P |
| transcript_cleanup_enabled | B | false | | P |
| transcript_cleanup_prompt | S | "" | | P |
| transcript_cleanup_provider | S | `openrouter` | resolved as pair | P |
| transcript_cleanup_model | S | `openrouter/free` | | P |
| transcript_cleanup_model_sort | C | `alphabetical` | TranscriptCleanupModelSort | P |
| transcript_cleanup_reasoning | C | `off` | off..ultra | P |
| text_llm_profiles | L* | [] | custom endpoint profiles [a12] | P |
| transcript_cleanup_rules | L* | [] | <= 50 strings | P |
| transcript_cleanup_profiles | R | — | [a12 cleanup_profiles] | P |
| quick_record_profile | S | "" | | P |
| transcript_batch_relation | C | `separate` | separate/sequential/custom | P |
| transcript_batch_custom_instructions | S* | "" | stripped, cut to 2000 chars | P |
| transcript_batch_custom_combine | B | false | | P |
| minimize_tray | B | true | | P |
| streaming_enabled | B | false | live preview | P |
| streaming_chunk_duration | F | 3.0 | seconds | P |
| streaming_overlay_font_size | I | 16 | clamp 10..48 | P |
| streaming_overlay_layout | C | `header` | header/footer | P |
| recording_overlay_look | C | `bars` | 11 looks + aliases | P |
| recording_overlay_dot / _text / _clock | B | true | | P |
| ui_font_scale | IC | 100 | 90/100/115/130 | P |
| ui_theme | C | `dark` | dark/light/system/omarchy; absent + Omarchy -> omarchy | P |
| guided_setup_state | S* | "" | | P |
| meeting_setup_complete | B* | false | | P |
| streaming_overlay_enabled, streaming_paste_enabled | R | — | legacy, dropped on preview toggle write | P |
| local_asr_models | D* | {} | {backend: model} | P (Parakeet entry only) |
| local_asr_devices | D* | {} | {backend: device} | P (Parakeet entry only) |
| local_asr_language | S | `en` | | P |
| meeting_asr_model | S | `config.MEETING_ASR_MODEL` (platform; None -> fallback) | catalog model with `meeting` flag | P |
| meeting_asr_source | C* | `local` | local/remote | P/U |
| meeting_speech_separate | B | false | decided once at install | P |
| whisper_model | S | `auto` | faster-whisper | X |
| custom_whisper_models | L | [] | | X |
| whisper_device, whisper_compute_type | S | `auto` | | X |
| whisper_gpu_offer_declined | B | false | | X |
| hf_access_policy | C* | `ask` | ask/always/never | P |
| hf_hub_offline | R | — | legacy, migrated | P |
| last_tab_index | R | — | | P |
| developer_mode | B | false | | P |
| recording_retention_mode | C | `custom` | keep_all/custom/size_limit | P |
| max_saved_recordings | I | 20 | clamp >= 1 | P |
| max_saved_recordings_mb | I | 1024 | clamp >= 1 (MiB) | P |
| recording_trigger_mode | C | `toggle` | toggle/push_hold | P |
| confirm_history_entry_delete, confirm_meeting_delete | B | true | | P |
| meeting_whisper_model | S | `auto` | legacy Whisper meeting model | X |
| meeting_language | C | `auto` | 29 codes; normalize strip+lower | P |
| meeting_llm_provider | S | `openrouter` | pair with model | P |
| meeting_llm_model | S | `openrouter/free` | | P |
| meeting_agent_core | C | `pi` | pi/opencode/claude_code/codex/opencode_cli | P |
| meeting_backup_agent | C | "" | "" or installed agent | P |
| meeting_agent_models | D | {} | installed agent -> model | P |
| meeting_live_thinking / meeting_final_thinking | C | `low` / `medium` | reasoning levels | P |
| meeting_end_redecode | B | false | | P |
| meeting_redecode_coverage_guard | B | false | | P |
| meeting_end_polish | B | true | | P |
| meeting_insight_review | B | false | | P |
| meeting_insight_review_consent | S* | "" | must equal `typesafe-text-v1` | P |
| meeting_insight_review_sensitivity | C | `normal` | normal/thorough | P |
| meeting_end_report, meeting_report_ribbon/brief/signal | B | true | | P |
| meeting_cloud_consent_given, meeting_cloud_last_enabled | B | false | | P |
| meeting_speaker_id_backend | C | `local` | off/local/openai (openai -> local after 2027-02-26) | P (openai: U) |
| meeting_audio_upload_consent_given | B | false | | U |
| meeting_unsupported_platform_ack | B | false | | P |
| meeting_linux_preview_ack_version | IC* | 0 | strict; only 1 accepted | P |
| meeting_mode_intro_seen | B | false | | P |
| meeting_past_recall_enabled, meeting_context_folder_enabled | B | false | | P |
| meeting_context_folder_path | S | "" | | P |
| meeting_server_bind | C | `localhost` | localhost/lan | P |
| meeting_server_port | I | 0 | clamp 0..65535 | P |
| mcp_enabled | B* | false | | P |
| mcp_port | I* | 8767 | strict, valid 1..65535 | P |
| mcp_tailscale_enabled, mcp_retitle_transcriptions, mcp_retitle_meetings, mcp_settings_access | B* | false | | P |
| mcp_writable_settings | D* | {} | {key: true} | P |
| remote_engine_client | OD* | None | host address, cert fingerprint, name | U |
| remote_host_enabled, remote_host_model_management | B* | false | | U |
| remote_host_port | I* | 47821 | valid 1..65535 (non-strict) | U |
| remote_host_devices | L* | [] | token digests | U |
| remote_host_tailscale_trust | B* | true | | U |
| remote_host_keep_records, remote_host_manage_mcp | B* | false | | U |
| remote_records_location | C* | `local` | local/host/both | U |
| remote_client_history | B* | false | | U |
| typesafe_enabled | B | false | master | P |
| typesafe_provider | C | `typesafe` | typesafe/openrouter/text_model | P |
| typesafe_text_model | C | `meeting` | meeting/cleanup | P |
| typesafe_citations_enabled, _semantic_search_, _question_radar_, _highlights_ | B | false | | P |
| typesafe_topic_shift_enabled | B | true | | P |
| typesafe_voice_commands_enabled | B | false | | P |
| typesafe_voice_command_names | OL* | None | | P |
| update_check_enabled, update_notify_enabled, update_restart_prompt_enabled | B | true | | P |
| update_beta_track_enabled | B | false | | P |
| update_last_check_at, update_last_failed_check_at, update_api_retry_at | R | — | [a16] | P |
| app_context_enabled | B | true | | P |
| app_context_read_text | B | false | | P |
| app_context_excluded_apps | L | [] | | P |
| app_styles_enabled | B | true | | P |
| app_style_tones | OD* | None | absent = all Formal; new installs seeded | P |
| app_style_overrides | L | [] | [{match, category}] | P |
| transcript_cleanup_level | C | `medium` | light/medium/high | P |
| text_transforms | OL* | None | absent = starters | P |
| command_mode_insert_without_selection | B | true | | P |
| dictation_dictionary | L | [] | [{id, term, starred, heard, learned, new}] | P |
| dictionary_learn_enabled, dictionary_steer_recognition | B | true | | P |
| dictation_snippets | L | [] | [{id, trigger, text, formatted}]; URL-scrub exempt | P |
| snippets_enabled | B | true | | P |
| recording_hands_free_latch | B | true | | P |
| audio_input_priority | L | [] | [{name, hostapi}] | P |
| dictation_languages | L | [] | | P |
| dictation_active_language | S | "" | | P |
| scratchpad_always_on_top | B | true | | P |
| scratchpad_geometry | R | — | {x,y,width,height} | P |
| whats_new_seen | S | "" | date string | P |
| actions | L | [] | sanitized via actions.model on backup | P |
| voice_routing_enabled, voice_routing_on_dictation | B | false | | P |
| cleanup_model_memory, meeting_model_memory | D* | {} | | P |
| cleanup_endpoint_provider | S* | `openrouter` | | P |
| cleanup_agent_offer_answered | B* | false | | P |
| api_live_events | B | true | | P |
| api_run_actions, api_control_recording, api_text_filters, api_agent_prompts | B | false | forced false by backup | P |
| automation_webhooks, automation_programs, automation_filters | L | [] | items get enabled=false by backup | P |
| mcp_events_enabled | B | false | | P |
| walkthrough_title | S | `Walkthrough` | normalize strip | P |
| walkthrough_show_details/_about/_captions | B | true | | P |
| walkthrough_page_size | C | `auto` | auto/letter/a4 | P |
| walkthrough_copy_format | C | `pdf` | pdf/agent_task/both/agent/word | P |
| walkthrough_intro_seen, walkthrough_minimize_to_tray | B | false | | P |
| walkthrough_write_up | B | false | | P |
| walkthrough_write_up_agent | C | `claude_code` | installed agents | P |
| walkthrough_write_up_instructions | S | "" | strip | P |
| walkthrough_write_up_styles | OL* | None | absent = starters | P |
| walkthrough_write_up_notes | B | false | | P |
| walkthrough_snap_mode | C | `region` | region/window/screen/delayed | P |
| walkthrough_auto_mode | B | false | | P |
| walkthrough_capture_delay | IC | 0 | strict; 0,1,2,3,5,10 | P |
| walkthrough_keep_marks | B | false | | P |
| snap_to_edges | B | true | | P |
| snap_box_infill | B | false | | P |
| walkthrough_closed | S | "" | | P |

`SETTING_DEFAULTS` equals every non-`*` row; `test_typed_settings.py:test_the_defaults_table_is_unchanged` pins it
against `tests/legacy_settings_77b63c1.py`. Settings JSON may also hold keys not declared here (written by older or
other code); the store must preserve unknown keys untouched.

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| `json` | settings, prefs, manifest, metadata | `QJsonDocument` in the Qt layer, or nlohmann/json (vcpkg) in a Qt-free core. Must keep insertion order on rewrite (Python dicts are ordered) and `indent=2`; `QJsonObject` sorts keys, so use nlohmann `ordered_json` for byte-compat |
| `tempfile`, `os.replace`, `os.fsync` | atomic writes | `QSaveFile` (Qt) or `std::filesystem` + `MoveFileExW(MOVEFILE_REPLACE_EXISTING|WRITE_THROUGH)` / `rename` + `fsync` |
| `os.stat` (dev, ino, mtime_ns, ctime_ns) | file signature | `std::filesystem::last_write_time` + size; on Windows `GetFileInformationByHandle` for volume serial + file index; POSIX `stat` |
| `threading.RLock`, `Event`, `Thread` | locks, timeouts | `std::recursive_mutex`, `std::condition_variable`, `std::thread` (detached for abandoned keyring calls) |
| `pickle`, `deepcopy` | cheap snapshot copies | value-semantics copy of a JSON value; or `shared_ptr<const Json>` snapshot (copy-on-write) |
| `keyring` (WinVaultKeyring, macOS Keyring, SecretService) | credential store | Windows `CredReadW/CredWriteW/CredDeleteW` (wincred.h); macOS `SecItemCopyMatching/Add/Update/Delete`; Linux libsecret (`secret_password_lookup_sync` etc.) or direct D-Bus via QtDBus. Or the `qtkeychain` library (vcpkg `qtkeychain-qt6`) — but it does its own discovery and creates the default collection, so the lookup-only behavior (A10-29) would need a custom Linux path |
| `secretstorage` | Secret Service search/unlock | QtDBus calls to `org.freedesktop.Secret.Service.SearchItems`, `Unlock`, `Item.GetSecret`; or libsecret `secret_service_search_sync` with `SECRET_SEARCH_UNLOCK` off |
| `python-dotenv` | `.env` parse | small hand-written parser matching dotenv semantics (quotes, `export`, comments, `${VAR}` interpolation) |
| `zipfile` | `.owbackup` | minizip-ng or libzip (vcpkg), needs Zip64, stored + deflate, streaming read/write, external attributes |
| `hashlib.sha256` | checksums | `QCryptographicHash::Sha256` or OpenSSL/BCrypt |
| `sqlite3` (online backup API, PRAGMAs) | DB snapshot, sanitize, verify | SQLite C API `sqlite3_backup_init/step/finish` (vcpkg `sqlite3`); same PRAGMAs |
| `sqlalchemy` (`Base.metadata.create_all`) | fresh schema when no DB | the C++ database layer's own schema creation (a09) |
| `shutil.disk_usage`, `rmtree` | free space, cleanup | `std::filesystem::space`, `remove_all` |
| `secrets.token_urlsafe(32)`, `uuid4` | meeting tokens, temp names | `QRandomGenerator::system()` / BCryptGenRandom + base64url; `QUuid` |
| `ctypes` `LockFileEx`/`UnlockFileEx`, `fcntl.flock` | data lease | Win32 directly; POSIX `flock` |
| `subprocess.Popen` | restart | `QProcess::startDetached` (with `CREATE_NO_WINDOW` not needed for a GUI exe) |
| PyQt6 `QObject`, `QTimer`, `pyqtSignal`; `ThreadPoolExecutor` | coordinator | `QObject`, `QTimer`, signals; `QThreadPool` with max 1 or a dedicated `QThread`; `QtConcurrent::run` + `QFutureWatcher` |

## Platform-specific code

- Credentials: Windows Credential Manager (WinVault); macOS Keychain (access prompts; `request_access` exists so an
  explicit Retry can let macOS show its authorization prompt for an existing item, 361-366); Linux Secret Service only
  (GNOME Keyring or KWallet's Secret Service bridge), with timeouts and lookup-only reads. Backend names shown to the
  user: `"Windows Credential Manager"`, `"macOS Keychain"`, `"Secret Service (GNOME Keyring or KWallet)"`.
  No file fallback anywhere by design (credentials.py:8-11).
- Data lease: Windows `LockFileEx` byte-range; POSIX `flock`. Windows releases a dead process's lock with a delay
  (test notes, test_backup_startup.py:58-65).
- Backup: `_fsync_directory` is a no-op on Windows (646-647); `_is_link` also checks NTFS junctions (119-120);
  `normcase` for root comparisons.
- Restart: Windows `CREATE_NO_WINDOW`; frozen macOS/Windows PyInstaller env cleanup; main.py releases the Windows
  application mutex before respawn (a16).
- Theme: `resolve_ui_theme` returns `omarchy` on an Omarchy (Hyprland) desktop when nothing is saved
  (`services.desktop_session.use_omarchy_ui`, a01/a02).
- `keep_unchosen_engine` Linux special case; `_own_meeting_model` treats darwin as Parakeet-capable (1912).
- `macos_accessibility_intro_seen` is macOS only.

## Engine-specific code

Drop (non-Parakeet engines): keys `whisper_model`, `custom_whisper_models`, `whisper_device`, `whisper_compute_type`,
`whisper_gpu_offer_declined`, `meeting_whisper_model`; `selected_model` choices `local_whisper`, `qwen_asr`, `moonshine`,
`parakeet_mlx`, `parakeet_unified`; the `local_whisper` branches of `resolve_meeting_speech` (1818-1824),
`_own_meeting_model` (1902-1906) and `meeting_model_label` ("Local Whisper ..."); the Linux branch of
`keep_unchosen_engine` (1443-1444); `services.whisper_sources.custom_models` calls.

Undecided: `nemotron` (same NeMo runtime) as a `selected_model` value; `api` + `api_transcription_*` +
`LEGACY_API_MODELS` + retirement helpers (cloud API); `remote` + all `remote_*` keys + `host_mode` + `meeting_asr_source`
remote branch (remote engine); `meeting_speaker_id_backend=openai` and audio upload consent.

Adapt for in-process Parakeet: `local_asr_models["parakeet"]` / `local_asr_devices["parakeet"]` become OpenWhisper_CPP's model
and CPU/GPU device choice (today OpenWhisper_CPP stores `engine/runtimeDir`, `engine/modelPath`, `engine/gpu` in QSettings,
`src/app/main_window.cpp:194-213`; these should move into the ported store). The NeMo runtime directory has no
OpenWhisper equivalent: add a new key. `resolve_meeting_speech` collapses to "dictation's Parakeet model, or the remote
host". `config.DEFAULT_BACKEND` becomes a constant `parakeet`. `hf_access_policy` stays if OpenWhisper_CPP downloads Parakeet
models from Hugging Face.

## Dependencies on other areas

Calls out:
- a01 app shell: `config.config` (defaults, `SETTINGS_FILE`, `DEFAULT_HOTKEYS`, `MODEL_VALUE_MAP`, `DEFAULT_BACKEND`,
  `API_MODEL_CHOICES`, `WHISPER_MODEL_CHOICES`, limits), `config.data_root`, `env_file_path`,
  `parakeet_default_supported`, `_version.__version__`, `services.desktop_session.use_omarchy_ui`.
- a08: `services.batch_upload.BatchRelation`.
- a09: `services.database.SCHEMA_VERSION`, `services.models.Base` (schema), table layout.
- a12: `services.cleanup_prompts.base_prompt`, `services.text_llm` (`known_profile_ids`, `get_profile`,
  `builtin_profile`, `default_model_for_profile`, `snapshot_from_profile`), `services.app_styles.NEW_INSTALL_TONES`,
  `services.openai_retirement`.
- a14: `services.local_asr.catalog` (`BACKENDS`, `MODELS[...].label/.meeting`, `selected_model`),
  `services.whisper_sources.custom_models` (drop).
- a19: `services.actions.model.load_actions`, `ActionKind`.
- `services.process_env.load_for_app` (a04 runtime services or a20; contract: add env vars without overriding,
  remember them as the app's, return refused names).

Called in by (contracts):
- Essentially every area reads `SettingsKey.X.read(settings)` / `settings_manager.*`; `test_setting_key_references.py`
  scans `services, transcriber, ui_qt, meeting, benchmarks, scripts` for undeclared keys.
- a01 `main.py:300-415`: `acquire_startup_data_lease(data_root())` after CLI/worker modes and package self-test, before
  `ui_qt.bootstrap` import; releases at exit; restarts on 73. `services/agent_api/cli.py:39-68` and
  `services/release_checks.py:198-246` also take the lease.
- a02/a03 `ui_qt/bootstrap.py:310-314`: `keep_unchosen_engine()` then `keep_separate_meeting_speech()` before the main
  window reads the engine; `application_controller.py:170` `seed_new_install_settings`; `:741-828` creates
  `BackupCoordinator` with acquire/release/worker_context and exits 73 on `restore_staged` without error.
- a11: Settings Backup page drives the coordinator (`snapshot`, `set_schedule`, `create_backup`, `inspect_backup`,
  `prepare_restore`, `cancel`); API keys page uses `store().set/delete/status/refresh/request_access`, `mask_key`,
  `credential_source`, `environment_shadowed`, `UNRESPONSIVE_KEYRING_STATUS`; `settings_binder.py` reads/writes keys.
- a12/a17/a19/a20/a26/a29: `resolve_credential(name)` for keys and tokens.
- a17: `remote_engine.paused_records_for_backup()`, `record_sync.paused_for_backup()` used as backup worker context.
- `ui_qt/utils/engine_selection.py:52` uses `settings_manager.watch`.

## Tests

| Test file | Pins |
|---|---|
| `tests/test_settings.py` (1112) | manager: copies, `load_settings` subset and single-read caching, outside edits, concurrent writers, failed replace, malformed file not overwritten, update+remove, mutate rollback; HF policy migration; cleanup rules filter/cap/compose; batch settings; meeting resolvers (direct -> pi, port clamp, custom profiles, pair semantics); trigger mode; font scale; update prefs; API backend migration; engine keep rules; meeting separate-engine default; unreadable file loads empty and blocks writes |
| `tests/test_typed_settings.py` (569) | declarations vs legacy snapshot `tests/legacy_settings_77b63c1.py`; defaults table; fresh default copies; key-as-string behavior; `stored`; kind validation; every resolver equals the old one over a JUNK battery; infinity int reads default; port defaults match servers |
| `tests/test_setting_defaults.py` (84) | defaults belong to keys, read-only, match same-named config values; `setting_value` raw |
| `tests/test_setting_key_references.py` (33) | AST scan: every `SettingsKey.X` used exists |
| `tests/test_strict_switch_reads.py` (48) | cleanup and auto-paste switches and consents read strictly (non-bool is not consent) |
| `tests/test_credentials.py` (480) | timeout helper; round trip; unavailable store; refresh; key validation; no secret in logs; mask; failed read cached; hung read marks unusable; `request_access`; Secret Service lookups never prompt for missing keys, prefer unlocked, prompt only for locked, timeouts and prompt extension; resolution order incl. `.env` cache invalidation; status copy `"Requires OpenRouter API key — add it in Settings → App → API keys"` / `"OpenRouter API key found"` (text_llm, a12) |
| `tests/test_backup.py` (468) | restore to another root keeps previous data; text-only clears audio refs; captures optional; live meeting refused; cancel publishes nothing; destination inside not-yet-created recordings rejected; corrupt/traversal archives; checksums cannot hide missing audio; failed apply rolls back whole set and retries; rebased DB, import re-scrubs settings; stale sidecars moved and committed journal recovered; real DatabaseManager WAL round trip; legacy history merge |
| `tests/test_backup_manager.py` (291) | reservation release ordering; cancel; schedule default off and persistence; due-date math with retry; bad destination does not run; retention removes only registered archives; inspect doesn't reserve, restore does; snapshot previous data dir |
| `tests/test_backup_startup.py` (163) | cross-process shared/exclusive leases; pending restore blocked while another process holds the lease; main.py ordering of the gate; restart command has no argv |
| `tests/test_personalize_foundation_backup.py`, `tests/test_automation_fix_pipeline_backup.py` | scratchpad in backup; snippet URLs kept; action kind normalization; active actions off; URL scrub except link template |
| `tests/test_host_backup_recovery.py`, `tests/test_remote_keychain_recovery.py` | remote engine interplay with backup busy/gate and keychain retries (a17/a18) |
| `tests/test_settings_backup.py`, `tests/test_api_keys_settings_page.py`, `tests/test_settings_binder.py` | UI pages (a11) |
| `tests/fakes/settings.py` | `InMemorySettings` stand-in: the minimal manager interface other areas' tests rely on |

Golden-vector candidates (generate in Python, replay in C++):
1. `setting_types` kinds: `parse`/`stored` for every declared key over the `JUNK` battery from `test_typed_settings.py:41-54`
   (dump `{key, raw, read, stored}` rows).
2. `SETTING_DEFAULTS` dumped to JSON (declaration order, values).
3. All `resolve_*` functions over generated settings dicts (single- and multi-key batteries, fixed `today` for retirement).
4. `credentials.validate_api_key` / `mask_key` over edge strings (empty, whitespace, 512/513 chars, smart quotes, NBSP,
   11/12-char keys).
5. `backup._scrub` / `_sanitize_settings` over sample settings (URLs with userinfo/query/fragment, secret-named keys,
   snippets, automation lists; actions need the a19 loader).
6. `backup._member_parts` / `_safe_member` accept/reject table.
7. `backup._validate_manifest` over crafted ZIPs; plus round trip: Python-made `.owbackup` must inspect and restore in
   C++ and vice versa.
8. `BackupCoordinator.snapshot()["next_backup_at"]` and prune selection for given prefs/clock (pure logic once extracted).

## Porting notes

Decomposition:
- Qt-free core (`src/core/settings`, namespace `fv`): `Json` value type (nlohmann `ordered_json`), `Setting<T>` kinds as
  small classes holding key/default/validator, `SettingsKey` as a header of `inline const` declarations, `SettingsStore`
  (file I/O, signature cache, recursive mutex, watchers via `std::function`), resolvers as free functions taking a
  `const Json&` snapshot. A Python-style `int()` parser for non-strict ints.
- `src/core/credentials`: `CredentialBackend` interface, `MemoryBackend`, `WinCredBackend`, `MacKeychainBackend`,
  `SecretServiceBackend` (libsecret or sd-bus/QtDBus behind a timeout worker), `CredentialStore`, dotenv parser,
  resolution functions.
- `src/core/backup`: archive writer/reader (minizip-ng/libzip), SQLite snapshot/sanitize/rebase (SQLite C API), staging,
  apply/journal/rollback, data lease. No Qt so `apply_pending_restore` can run before `QApplication`.
- Qt layer (`src/app`): `BackupCoordinator` (QObject, QTimer, one worker thread, queued signals), a watcher-to-signal
  relay, Settings pages (a11).

Order: setting types + store + declarations (everything depends on them) -> resolvers needed by early areas ->
credentials (Windows first) -> data lease + startup gate -> backup create/inspect -> stage/apply -> coordinator and
schedule -> remaining resolvers as their consumer areas land.

Risks:
- Python `int()`/truthiness and JSON-type fidelity (int vs float vs bool) must be exact; Qt's `QJsonValue` merges int and
  double, so prefer nlohmann. Key order and formatting matter only if byte-compatible files are required.
- Credential store compatibility with existing installs depends on matching keyring's WinVault target naming and
  Secret Service attributes exactly.
- Backup safety logic is long and security-relevant (zip-slip, symlinks/junctions, ratio bombs, rollback): port it
  line by line and port the tests; do not simplify.
- `.owbackup` contains the SQLite DB whose schema belongs to a09; schema version and table names must align, or
  OpenWhisper_CPP must declare its own format id.
- Watcher callbacks run on the saving thread (often a worker); Qt consumers must queue to the GUI thread.
- Abandoned D-Bus worker threads on timeout: in C++ a detached thread touching a destroyed object is a crash; keep the
  backend and its connection alive with `shared_ptr`.

Rough C++ size: setting types 300, store + watchers 450, declarations + enums/labels 700, resolvers 600, credentials
700 (three backends + dotenv), backup core 1500, coordinator 350, lease + restart 250; about 4,850 lines plus tests.

Native gains: the snapshot cache can hand out `shared_ptr<const Json>` instead of copying (OpenWhisper pays 0.1-0.6 ms per
read for a pickle round trip); typed reads compile to direct lookups; startup avoids importing SQLAlchemy for the restore
gate; SHA-256 and ZIP streaming in native code are much faster for multi-GB recording archives; the lease and restore
need no interpreter start.

## Open questions (owner decisions)

1. Must OpenWhisper_CPP read and write OpenWhisper's data root and files as-is (same `openwhisper_settings.json`, keys,
   credential service name `OpenWhisper`, `.owbackup` format id), or use its own names and offer a one-time import?
2. Replace the current `QSettings` storage in `src/app/main_window.cpp` with the ported JSON store?
3. Which `selected_model` values survive (Parakeet only, plus Nemotron, API, remote)? Determines which keys and
   migrations remain.
4. Linux credential backend: libsecret (C, GLib) or QtDBus; or accept qtkeychain and lose lookup-only reads?
5. Restart after restore: re-exec the same binary (no PyInstaller concerns) — confirm exit code 73 contract is kept for
   any external launcher.
