# Speech models, components and runtimes, downloads and verification, GPU, transcriber backends

Area a14. Source: `D:\coding\whisper_local` (OpenWhisper), 54 files, about 13.4k lines
(of which about 2.2k are pinned JSON catalogs). Paths below are relative to that root.
This area has no Qt widgets of its own: the Downloads page, component rows, consent and
"Use this GPU" dialogs live in the UI areas (`ui_qt/dialogs/settings_downloads.py`,
`ui_qt/widgets/component_row_widget.py`, `ui_qt/dialogs/hf_consent_dialog.py`,
`ui_qt/dialogs/required_runtime_dialog.py`, `ui_qt/dialogs/use_gpu_dialog.py`). This area
supplies their data, state machine and every user-visible string they show about a
component or model.

Legend for the "Port" column: **port**, **drop** (engine-specific: faster-whisper,
Moonshine, Qwen, Parakeet MLX, transcribe.cpp / Parakeet Unified, worker-process plumbing),
**undecided** (Nemotron streaming, OpenAI / compatible API, remote engine), **other-platform**
(only matters on macOS or Linux), **other-area** (data owned here but the feature belongs
elsewhere).

## What the user gets

- A **Downloads** catalog that works offline: every downloadable thing (speech runtimes,
  speech models, GPU libraries, the meeting agent) is pinned in the app with an immutable
  URL, exact byte size and SHA-256. Rows show state (Not installed / Installed / Update
  available / Incompatible / Broken / External), sizes, a reason line, and a details
  inspector with origin, license, payload, limitations and source links.
- **Install / update / remove** of components with live progress (resolving, downloading,
  verifying, extracting, finalizing), cancel, resumable downloads (a canceled or cut
  transfer resumes next time), a free-space precheck, antivirus-aware error messages, and
  a crash-safe swap: an interruption leaves the old install or the new one, never a mix.
- **Speech model downloads** (Parakeet TDT 0.6B v3, Orukeet, Nemotron, ...) into a private
  cache, size+SHA-checked, atomic, with a consent policy (Ask / Always / Never, plus the
  `HF_HUB_OFFLINE` hard override) and one-time grants.
- **Runtime choice per device**: Auto / CPU / GPU. Auto picks the installed GPU runtime,
  else the CPU one; explicit GPU never silently falls back. CUDA runtime for Turing+ cards,
  Vulkan runtime for older NVIDIA cards on Linux, Metal on Apple Silicon. A first-use offer
  names the GPU runtime Auto would use once installed.
- **Local transcription** of any audio file in bounded 30 s windows split at the quietest
  0.1 s near 24-30 s, silent windows skipped, segments timestamped in source time; a
  warm-up decode after load; a best-effort live preview that never blocks a real job;
  window-by-window decode for incremental dictation; a timestamped decoder for meetings.
- Optional backends (undecided for FioriVoce): OpenAI / OpenAI-compatible upload with
  hint fields, chunked parallel upload of large files and a pre-warmed upload client; the
  Remote engine (a paired computer's speech engine) with link status, busy retry, model
  switching and timing.

## Feature inventory

| ID | Feature | Exact user-visible behavior | Source | Port |
|---|---|---|---|---|
| A14-01 | Built-in component catalog | Offline catalog, schema 2. Entries per component per platform tag: `version`, `component_api` (1), `platform`, `install_bytes`, `archives[{name,url,sha256,size_bytes,extract,member?,root?}]`, optional `published`, `python_abi`, `macos_min`. Frozen; public API returns deep copies. | services/components.py:_BUILTIN_CATALOG_RAW (368-402), catalog_entry_for_platform | port (NeMo runtimes only; others drop/undecided) |
| A14-02 | Platform tags | `win_amd64`, `linux_x86_64`, `linux_aarch64`, `darwin_arm64`, `darwin_x86_64`; anything else = no components. | components.py:current_platform_tag (505) | port |
| A14-03 | Offered components per platform | Win x64: gpu-accel, meeting-agent, meeting-agent-opencode, all RUNTIME_IDS. Linux x64: gpu-accel, meeting agents, asr-nvidia-cpu, asr-nvidia-cuda, asr-transcribe, plus asr-nvidia-vulkan only if installed or `nvidia_gpu_runtime()` picks it. Linux arm64: meeting agents only. macOS arm64: meeting agents, nvidia cpu+metal, mlx, qwen, moonshine. macOS x64: meeting agents, nvidia cpu. Filtered by `component_is_published` and (host check only) `macos_min`. | components.py:available_component_ids (560-629) | port (subset) |
| A14-04 | "Published" gate | Unpublished if `published: false`, no archives, install_bytes <= 0, any digest not 64 hex or all-zero placeholder, URL not `https://`, or size <= 0. | components.py:component_is_published (632) | port |
| A14-05 | Component state + reason | States `not_installed`, `external`, `installed`, `update_available`, `incompatible`, `broken`. Reasons: "The component manifest is missing or invalid." / "The previous installation did not finish." (dir without sentinel) / "CUDA libraries are already available from your existing setup." (gpu-accel external) / incompatibility text / "OpenCode files are missing or incompatible. Reinstall this component." / "Update frees {size} of disk space." (only when reclaimed > 0). Update = catalog version != installed version (any difference). `is_usable` = external/installed/update_available. Component with no catalog entry still reports installed. | components.py:ComponentCoordinator.describe (2223-2337), ComponentInfo (473) | port |
| A14-06 | Compatibility check | "Built for a different version of OpenWhisper (component API {api}, this app uses 1)"; "Built for {tag}, which this app cannot use"; "Requires macOS {min} or later"; "Built for Python X.Y, this app uses X.Y" (python_abi only). | components.py:check_compatibility (1240) | port (drop python_abi part) |
| A14-07 | Install pipeline | catalog entry must list archives, then `check_compatibility(entry)` (its text is thrown as the install error, before any download) -> resolving -> free-space check (download + install bytes, x1.15) -> download each archive to `.cache/<name>` (reusing cached ones; runtimes re-hash cached archives first) -> extract into `.staging/<id>.<pid>` -> validate payload -> write `manifest.json` (entry, indent 2) then `.installed` (version text) last -> swap: live -> `<id>.old`, staging -> `<id>`, restore on failure -> delete `.old` and cached archives. Errors: "The catalog entry lists no files to download." | components.py:_install_component (1865), _download_archives (1932), _swap_into_place (2045), _write_install_markers (2028) | port |
| A14-08 | Progress | `(phase, done, total)` where phase in resolving/downloading/verifying/extracting/finalizing; download progress is overall across archives (offset_base + done of grand_total). The app layer throttles to ~10 emits/s unless percent changes or final. | components.py:InstallPhase (455), _download_verified (1294); services/runtime/components.py:_install_worker (120) | port |
| A14-09 | Verified resumable download | `<dest>.part` opened O_NOFOLLOW, mode 0600, must be a single-link regular file; existing bytes re-hashed; Range `bytes=N-`; requires 206 + exact `Content-Range: bytes N-(S-1)/S` else restart (if server ignored Range) or error; Content-Length must equal remaining; 416 -> restart from 0; never accept more than size; verify size then SHA-256; fsync; rename; chmod 0600; fsync dir (POSIX). Cancel keeps .part for components and models (`keep_partial_on_cancel=True`). Corrupt -> .part deleted. Network cut -> .part kept. User-Agent `OpenWhisper/<version>`, timeout 30 s, chunks 1 MiB. Messages table MESSAGES (invalid_size ... finalize). | services/verified_download.py:download_verified (368), _PartialFile (221), MESSAGES (49) | port |
| A14-10 | Network error text | Cert failure: "The {server}'s certificate could not be verified. This is usually caused by network security software that inspects HTTPS traffic. Ask your IT team to allow {host}." (host = the URL's own host; list joined "a, b and c"). HTTP: "The download server returned an error ({code} {reason})." URL: "Could not reach the download server ({reason})." | verified_download.py:describe_network_error (127); components.py:_download_verified (1329) | port |
| A14-11 | Free-space precheck | "Not enough disk space: {needed} needed, {free} free on this drive." needed = required x 1.15; creates the folder first. | verified_files.py:free_space_shortfall (55); components.py:_check_free_space (2122) | port |
| A14-12 | Disk error text | ENOSPC / WinError 112: "The drive ran out of space while installing." EACCES / WinError 5: "A file could not be written, which usually means antivirus software blocked it. Try adding an exclusion for {components_root}." Else "Installation failed: {exc}". | components.py:_describe_disk_error (2132) | port |
| A14-13 | Locked-file swap retry | Speech-runtime renames retry up to 15 s every 0.5 s on PermissionError (Windows scanners held new files ~6 s); cancel wakes the wait. | components.py:_replace_speech_runtime (1831); verified_files.py:retry_while_locked (78) | port |
| A14-14 | Safe extractors | `zip` (reject absolute / `..`), `python-wheel` (zip into site-packages), `nvidia-wheel` (only `nvidia/*/bin/*.dll` or `nvidia/*/lib/*.so*`, flattened), `node-exe` (exactly one node.exe), `node-tar` (exact member, regular, non-empty, chmod 0755 via temp+rename), `nemo-tar`/`transcribe-tar` (every member under `root`, renamed to `nemo-speech/` or `transcribe-native/`, hard-link targets rewritten, tarfile "data" filter rejects unsafe links/special files: "Unsafe speech runtime archive: ..."). Cancel checked per member. | components.py:_extract_archive (1988) and _safe_extract* (1350-1635) | port: zip + nemo-tar; drop: nvidia-wheel, python-wheel, transcribe-tar; other-area: node-* |
| A14-15 | Payload validation | Linux: `nemo-speech/lib/libnemo_speech_asr_c.so`. macOS: `nemo-speech/lib/libnemo_speech_asr_c.dylib` (+ `libggml-metal.dylib` for metal). Windows: `python.exe`, `python312.dll`, `python312.zip`, `bin/nemo_speech_asr_c.dll`, then writes `python312._pth`. Message "The speech runtime is missing required files." | components.py:_validate_speech_runtime (1692-1748) | port, adapted (Windows needs only `bin/nemo_speech_asr_c.dll`; see Engine-specific code) |
| A14-16 | Uninstall | Rename `<id>` -> `<id>.old` first so it stops counting, then delete. "Could not fully remove '{id}': ..." / "Could not fully remove '{id}'; leftover files remain." | components.py:_uninstall_component (2079) | port |
| A14-17 | Startup recovery | Delete `.staging`; for each `*.old`: complete (has sentinel) and target missing -> restore; complete and target present -> delete; incomplete -> delete; failed restore keeps `.old`. App only (not benchmarks). | components.py:prune_orphans (1171); main.py:_activate_downloadable_components (242) | port |
| A14-18 | Installed size with cache | Walks tree without following symlinks (counts link itself); cached per directory keyed on (dir ino, mtime_ns, sentinel ino, mtime_ns, size). | components.py:installed_size_bytes (1127), _walk_size_bytes (1148) | port |
| A14-19 | Install coordinator | One install per component (`begin_install` returns cancel event or None), `cancel_install`, `cancel_all` before executor shutdown, `is_installing`, `is_any_installing`. App layer refuses runtime install/remove during a speech job: "Finish the current speech job before installing its runtime" / "...before removing its runtime"; closes backends using that runtime first. Finish messages: "Installed and ready to use — no restart needed." / "Restart OpenWhisper to enable this component." / "Installation canceled." | components.py:ComponentCoordinator (2147); services/runtime/components.py | port |
| A14-20 | Component leases | Use count vs mutation: "This component is being installed or removed. Try again when Downloads finishes." / "This component is in use. Finish the meeting or report job before changing it." Only wired for OpenCode. | services/component_leases.py | port if OpenCode kept, else drop |
| A14-21 | Component details copy | Display name, summary, description, origin, source, maintainer, family, requires, payload, local format, license, best-for, limitations, compact tags, source note, URLs. NVIDIA rows: "NVIDIA Speech CPU" / "NVIDIA Speech GPU" / "NVIDIA Speech GPU (Vulkan)" / "NVIDIA Speech GPU (Metal)"; description + " Models download separately. Runs in a dedicated process." | services/component_catalog.py | port (rewrite "Runs in a dedicated process"; NVIDIA rows only) |
| A14-22 | Startup activation | gpu-accel: Windows add `bin` to DLL dirs + PATH; Linux RTLD_GLOBAL preload of `lib/*.so*`. Speech runtimes need no startup activation, but `NvidiaRecognizer.__init__` itself calls `os.add_dll_directory(<runtime>/bin)` on Windows before loading `nemo_speech_asr_c.dll` (so its sibling DLLs resolve); FioriVoce must do the equivalent (`AddDllDirectory` / `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`) at load. | services/component_runtime.py | drop (gpu-accel is faster-whisper only) |
| A14-23 | NVIDIA GPU detection | NVML (`nvml.dll` / `%ProgramFiles%\NVIDIA Corporation\NVSMI\nvml.dll`; `libnvidia-ml.so.1` / `.so`): first device name, total MiB, compute capability; v2 symbols preferred; queried once per run; never raises; `short_name` strips "NVIDIA ", "GeForce ", "Quadro ". | services/gpu_info.py:nvidia_gpu (145), _query_nvml (99), NvidiaGpu (50) | port |
| A14-24 | CUDA vs Vulkan runtime | "cuda" device = `asr-nvidia-vulkan` when capability known and < (7,5) and Vulkan published on this platform (Linux x64 only), else `asr-nvidia-cuda`. | local_asr/catalog.py:nvidia_gpu_runtime (129) | port |
| A14-25 | Runtime resolution | `resolve_runtime(backend, requested)`: Apple Silicon: cpu -> cpu runtime; Auto keeps installed CPU runtime until Metal installed, else Metal ("metal"); a saved "cuda" means Metal. Else Auto device = `_auto_device`; if Auto and that runtime is not installed (parakeet/nemotron) -> CPU runtime, "cpu". Explicit CUDA never falls back. | local_asr/catalog.py:resolve_runtime (168), runtime_id (154), _auto_device (199) | port (adapt _auto_device) |
| A14-26 | Missing runtime / first-use GPU offer | `missing_runtime`: runtime for the selected device if published, not installed and compatible. `gpu_runtime_offer`: only for parakeet/nemotron, not Apple Silicon, device Auto, CPU runtime not installed, NVML sees a GPU, Auto would use cuda; Vulkan offered only if `libvulkan.so.1` loads; CUDA only to a known capability >= 7.5; not if installed/unpublished/incompatible. | local_asr/catalog.py:missing_runtime (261), gpu_runtime_offer (223) | port |
| A14-27 | Speech model catalog | Keys, backend, label, purpose, license, languages, flags. Parakeet: "Parakeet TDT 0.6B v3" ("Fast dictation and files", CC-BY-4.0, "25 European languages", meeting). Orukeet: "Orukeet TDT 0.6B" (CC-BY-SA-4.0). Nemotron: "Nemotron 3.5 ASR 0.6B" (streaming, meeting). `DEFAULT_MODELS` = first per backend. `selected_model` never crosses families. | local_asr/catalog.py:MODELS (25), BACKENDS (67), selected_model (111) | port (parakeet, orukeet); undecided (nemotron); drop (rest) |
| A14-28 | Model artifact pins | models.json: repo, revision, files[{name,url,sha256,size_bytes}]. parakeet-v3: one 713,975,456-byte q8_0 GGUF; orukeet: 714,456,704; nemotron: 741,548,352. | services/local_asr/models.json | port (copy verbatim) |
| A14-29 | Speech model cache | `<local_app_dir>/speech-models/<key>/`; cached iff `installed.json` parses equal to the spec AND every file size matches. Load path = the `.gguf` file, else the folder. Missing: "{label} is not downloaded. Open Downloads." | local_asr/cache.py:is_cached (21), load_path (32) | port |
| A14-30 | Model download | Refuses under `HF_HUB_OFFLINE`: "Model downloads are disabled by HF_HUB_OFFLINE." Per-key thread lock + cross-process file lock `<target>.lock`; restores `.previous` if target missing; downloads each file into `<key>.partial/` via the verified downloader with overall progress; writes `installed.json`; swaps `target -> .previous`, `partial -> target`, rollback on failure, delete `.previous`. | local_asr/cache.py:download (40), _swap_into_place (82) | port |
| A14-31 | Model delete / inventory | Delete removes target, `.partial`, `.previous` under the same locks. Inventory reports `{repo: (repo, total bytes, path, (revision,))}` for cached keys; replaces HF-cache rows for the same repo. | local_asr/cache.py:delete (99), inventory (114); hf_access.py:scan_cached_models (332) | port |
| A14-32 | Download consent policy | `evaluate_access`: cached -> load_cached; `HF_HUB_OFFLINE` env (1/on/true/yes) -> blocked_by_env (beats grants); one-time grant -> download_allowed (consumed unless advisory); policy always -> allowed; else needs_consent. Dialog results: cancel / download_once / always_allow / open_settings. `begin_request`/`end_request` dedupe one consent+download per model; `claim_batch` skips cached, duplicates, in-flight; grants issued just before each fetch. | services/hf_access.py:HuggingFaceAccessCoordinator (444), AccessDecision (41), ConsentAction (49) | port |
| A14-33 | Model download error text | "Download of '{m}' was canceled." / "Not enough disk space for '{m}' (about {size}). Free up space, then retry in Downloads." / "Couldn't reach the download server for '{m}'. Check the internet connection, then retry in Downloads." / "Couldn't download '{m}' (details in openwhisper.log). Retry in Downloads." Marker lists _OFFLINE_MARKERS, _DISK_FULL_MARKERS; chain walked up to 8 causes. | hf_access.py:describe_model_download_error (94) | port (log name "fiorivoce.log"?) |
| A14-34 | Model details for Downloads | For speech models: description = purpose, origin = label, repo URL `https://huggingface.co/<repo>`, maintainer = repo owner, params "600 million" default (Orukeet "627 million"), relative_performance "Measure on your hardware; published throughput is not dictation latency.", memory text, download MB = round(sum/1e6), format "GGUF Q8", limitations (runtime note + device note), license URL (Orukeet NOTICE.md), revision, hosts, verification "Pinned version; size and SHA-256 checked during download". Orukeet overrides at 536-545. Size text `~N MB` / `~N.N GB` (>=1000 MB). Notices MODEL_SECURITY_NOTICE. | services/model_catalog.py:_speech_model_details (494), format_download_mb (41), MODEL_SECURITY_NOTICE (31) | port (parakeet, orukeet; nemotron undecided) |
| A14-35 | Languages | Presets: en English, ru Russian, es Spanish, fr French, pt Portuguese, zh Mandarin, auto Auto. Parakeet: en, ru, es, fr, pt, auto. Nemotron: all seven. `selected_language`: stored if allowed, else "en" when only English, else "auto". Nemotron "zh" -> "zh-CN". Default setting "en". | local_asr/languages.py | port (parakeet); undecided (nemotron) |
| A14-36 | Parakeet recognizer options | Default options from runtime; language_code null for auto; `enable_word_time_offsets`, `enable_automatic_punctuation`, `interim_results` all true; times in ms -> s. Errors: runtime `last_error` text, else "Speech runtime error {status}". GPU index: -1 for CPU, else 0 (or Vulkan NVIDIA index). | local_asr/nvidia.py:NvidiaRecognizer (147-255) | port (already partly in src/engine) |
| A14-37 | Segments from words | New segment when a word ends with `.`, `?` or `!` (after rstrip) or reaches 24 words; text = words joined by one space; start/end from first/last word. No words: one segment 0..duration if text non-blank, else none. | local_asr/nvidia.py:segments_from_words (300) | port (exists: src/engine/segments.cpp; verify parity) |
| A14-38 | Vulkan device pick (Linux) | Enumerate ggml GPU + iGPU devices from `libggml.so`; first whose description contains nvidia/geforce/quadro/tesla/titan (case-insensitive). "The Vulkan speech runtime found no NVIDIA GPU. Check the NVIDIA driver, or choose CPU." Log names the GPU: "{name} is running on {gpu} through Vulkan". | nvidia.py:_vulkan_gpu_index (125), nvidia_device_index (114) | other-platform (Linux) |
| A14-39 | Linux load errors | Preload system `libstdc++.so.6` RTLD_GLOBAL first. Missing `libcuda.so`: "The GPU speech runtime needs the NVIDIA driver (libcuda.so.1 was not found). Install the NVIDIA driver, or choose CPU." Missing `libvulkan.so`: "The Vulkan speech runtime needs the Vulkan loader (libvulkan.so.1 was not found). Install it and the NVIDIA driver, or choose CPU." macOS: "Install NVIDIA Speech GPU (Metal) in Downloads, or choose CPU." | nvidia.py:_load_linux (81), NvidiaRecognizer.__init__ (153-158) | other-platform |
| A14-40 | Word boosting | Disabled (`WORD_BOOSTING = False`): Nemotron GGUF lacks an embedded tokenizer. When used: <= 50 non-blank phrases, boost 2.0, one speech context; on failure decode again without it ("Word boosting failed; decoding without it"). | nvidia.py:speech_context (62); worker.py:boost_phrases (74), transcribe_native (203) | undecided (Nemotron) |
| A14-41 | Native streaming | `streaming_recognize`, push f32 at 16 kHz, `finish`, drain `stream_next`, close; per-session keys; `cancel_stream`. Events `{text, words, final, end}`. Parakeet TDT rejects it. | nvidia.py:stream (257), cancel_stream (284) | undecided (Nemotron) |
| A14-42 | File windows | Decode any file (PyAV/FFmpeg) -> s16 mono 16 kHz -> float /32768; windows <= 30 s; when >= 30 s pending, cut at 24 s + (argmin energy over 0.1 s blocks in 24-30 s)+1 block; offsets in source seconds; identical result however the stream is chunked. Mono PCM16 WAV opens with `probesize=4096`. | services/local_asr/audio.py:split_point (20), WindowSplitter (39), windows (97) | port |
| A14-43 | Window decode loop | Inside one window, re-split with split_point; skip sub-window whose peak <= 0.00025; offset segment times by start/16000; join non-empty texts with one space. Example: 61 s of constant 0.1 -> segments at 0.0, 24.1, 48.2. | transcriber/optional_backend.py:_transcribe_audio (472), _audible (468), join_texts (459) | port |
| A14-44 | Backend load flow and messages | reload: cleanup, resolve runtime; not on platform: "{Name}'s {DEVICE} runtime is not available on this platform."; incompatible: "{Name} is not available on this computer. {reason}."; missing: "Install {Name}'s {GPU|CPU|MLX} runtime in Downloads."; model missing: "Download {label} in Downloads."; device_info "{label} \| {device}" or last_error or "{Name} is not loaded". Name = BACKENDS label ("Parakeet"). Load failure logs worker output. | optional_backend.py:reload_model (100), _explain_missing_runtime (136), device_info (79) | port |
| A14-45 | Generation and cancel | `generation` increments on every cleanup; stale load results are discarded; cancel closes the engine; next job reloads. "Transcription canceled". | optional_backend.py:generation (91), cleanup (210), cancel_transcription (218) | port (in-process: see Engine-specific) |
| A14-46 | Warm-up decode | After load, one decode of 1 s seeded noise (numpy default_rng(0) standard normal x 0.01); skipped if another decode holds the lock or engine unloaded; non-fatal; logs "{name} warmed up in {ms} ms". Backends warmed: `config.SPEECH_WARMUP_BACKENDS` = parakeet, nemotron, parakeet_mlx, parakeet_unified (only parakeet and nemotron matter here; qwen and moonshine are not warmed). Constants `_WARMUP_SAMPLES` 16000, `_WARMUP_LEVEL` 0.01 (about -40 dBFS, above the 0.00025 skip peak so it reaches the engine; yields no text). Warm-up aborts if the generation changed, and a failure while superseded or canceled logs at info ("{name} warmup stopped by a reload or cancel"), otherwise warning ("{name} warmup failed (non-fatal)"). Measured 314 ms cold vs 85 ms warm (RTX 2060). | optional_backend.py:warmup (222) | port |
| A14-47 | Preview / incremental / meeting decode | `preview_audio` returns None if busy or lock taken (never queues); `decode_window` queues but leaves is_transcribing; `transcribe_windows` marks busy; `SpeechDecoder.transcribe` marks the engine busy, decodes with language `language or "auto"` (not the engine setting), returns segments with avg_logprob=0, no_speech_prob=0, words=None and an info language of `language or "en"`, ignoring any Whisper options; `recognize` for remote hosting; `recognition_inputs` maps context language through engine choices. | optional_backend.py:preview_audio (267), decode_window (448), transcribe_windows (379), SpeechDecoder (493), recognition_inputs (319) | port |
| A14-48 | Auto GPU -> CPU fallback | If an Auto load fails with "unsupported_gpu", reload on CPU and set `cpu_fallback_note` "{Name} is using the CPU. {reason}". Python uses it for Qwen/transcribe.cpp only. | optional_backend.py:_start_worker (154-191) | port, adapted (NeMo GPU init failure on Auto) |
| A14-49 | Backend interface | `on_progress(message, transcribing)`, `supports_recognition`, `prepares_for_transcription`, `recognition_support` ("model"/"after"), `large_file_size_mb` (None = whole file), `is_transcribing`, `should_cancel`, `cleanup`, `name`. | transcriber/base.py | port |
| A14-50 | Worker-process plumbing | SpeechProcess JSON-lines (stdin/stdout or authenticated loopback socket), 2 s reaping, timeouts "Speech engine timed out and was stopped. Reload the engine.", crash "The speech engine stopped unexpectedly. Details are in openwhisper.log.", `close_all_workers` on quit, orphan preview-audio cleanup. | services/local_asr/process.py, worker.py, services/isolated.py, isolated_worker.py | drop (keep the two strings for an in-process equivalent) |
| A14-51 | Local Whisper backend | faster-whisper load, compute type fallback, GPU fallback causes and notes ("GPU unavailable, using CPU" etc.), hotwords. | transcriber/local_backend.py | drop |
| A14-52 | Whisper GPU planning | VRAM estimates, plan_cuda, memory guidance, "Use this GPU" plan. | services/gpu_info.py (161-364), services/gpu_setup.py | drop |
| A14-53 | GPU Acceleration component | CUDA 12.9 wheels (cuBLAS, NVRTC, runtime) for CTranslate2; Linux preload. | components.py:_BUILTIN_GPU_ARCHIVES*, native_libraries.py | drop |
| A14-54 | Whisper catalog and custom sources | 17 Whisper models with pinned revisions, custom CT2 folders and HF repos, discovery. | model_catalog.py (20-435, 548-599), whisper_models.json, whisper_sources.py | drop |
| A14-55 | Other optional engines | Qwen3-ASR (+batching, Silero gate), Moonshine, Parakeet MLX, transcribe.cpp / Parakeet Unified and their runtime JSONs. | local_asr/{qwen_batch,qwen_speech,moonshine,mlx,transcribe_engine}.py, *_runtime.json | drop |
| A14-56 | OpenAI / compatible API backend | Hints: OpenAI `gpt-transcribe` gets `keywords` + `languages:[code]`; others get `prompt` + `language` (endpoint.auto_language fallback). response_format "text" for OpenAI non-gpt-transcribe, else "json". 400/422 with hints -> retry once without. Retiring OpenAI model gone (404 or code model_not_found) -> retry with DEFAULT_API_MODEL. Large file split via audio_processor, upload 3 at a time, first failure cancels rest; progress "Transcribing {n} chunks...". Not available: "{endpoint} transcription is not available (no API key or client initialization failed)". | transcriber/openai_backend.py | undecided |
| A14-57 | Pre-warmed upload client | `warm()` on recording start readies a client (no network); idle standby closed after 600 s; upload waits up to 60 s for a warm-up in progress; read timeout = endpoint timeout, connect 10 s, no retries. | services/isolated.py:IsolatedOpenAIClient (176); isolated_worker.py:PreparedUpload (36) | undecided (much simpler natively) |
| A14-58 | OpenAI retirement | Shutdown 2027-02-26 ("Feb 26, 2027") for gpt-4o-transcribe, gpt-4o-mini-transcribe, whisper-1; "OpenAI has retired its speaker-identification model. On-device speaker labels were kept." | services/openai_retirement.py | undecided |
| A14-59 | Remote engine backend | Link states unpaired/connecting/connected/offline with host, route ("Tailscale" / "local network"), latency, busy, replies, beat, detail, engine tooltip fields; busy retry up to 90 s, delay x1.5 to 5 s, honoring retry_after; one reconnect for an idle-dead link; switch host model (timeout 300 s); recording reservations; timing split host vs network; messages "Pair with a host in Settings → Remote engine.", "{host} switched engines. Reconnecting...", "{host} stopped answering.", "Update OpenWhisper on {host} to choose its model from here.", "... It stayed busy for 90 seconds." | transcriber/remote_backend.py | undecided |
| A14-60 | Meeting agent component (Pi) | Node 22.23.2 + `bundle.cjs` sidecar, per-platform archives, version `node22-pi2`; validated by `node --version` exact "v22.23.2" and `node --check bundle.cjs` (10 s each); bundle revision < 2 is outdated (warn once per version "meeting-agent {v} is out of date; update it from Downloads"). Payload resolution: installed tree, else source `sidecar/dist` when not frozen. | components.py:_BUILTIN_MEETING_AGENT_BY_PLATFORM (199), meeting_agent_payload_dir (807), _validate_meeting_agent_payload (1768) | other-area (meeting agent; keep-or-not is open) |
| A14-61 | OpenCode component | Bun 1.3.14 + OpenCode SDK 2.0.18; all platforms `published: False` today, so never offered. Payload inventory SHA-256 check, offline self-test printing `OPENWHISPER_OPENCODE_OK`, isolated env allow-list. | services/opencode_catalog.py, opencode_component.py | other-area (not offered; likely skip) |
| A14-62 | Speaker-ID model | WeSpeaker ResNet34-LM ONNX `Wespeaker/wespeaker-voxceleb-resnet34-LM` / `voxceleb_resnet34_LM.onnx`, SHA-256 7bb2f06e...c068; resolution env `OPENWHISPER_SPEAKER_MODEL` (file or dir), `<local_app_dir>/models/speaker-id`, source tree; one download at a time; blocked texts for `HF_HUB_OFFLINE` and policy Never; last error kept until a download works; error sentences (timeout, network, HTTP status, save failure). | components.py:speaker_model_path (876), ensure_speaker_model (1041), _describe_speaker_download_error (945) | other-area (meeting diarization), port |
| A14-63 | Database schema models | SQLAlchemy tables for history, stats, meetings, record sync (listed under Data). | services/models.py | other-area (database), port |

### Added by check (feature inventory)

| ID | Feature | Exact user-visible behavior | Source | Port |
|---|---|---|---|---|
| A14-64 | Component row data | `ComponentInfo{component_id, display_name, summary, state, installed_version, available_version, download_bytes (sum of archive sizes), install_bytes, reason}`. `install_bytes` is the catalog's when not installed or broken-by-manifest, the measured on-disk size when installed. `display_name`/`summary` fall back to the id / "" when the id has no copy. `list_components()` = `describe` for each `available_component_ids()`; empty on unsupported platforms so the UI hides the whole section. `fetch_catalog()` returns the shared frozen catalog (no per-call copy). | components.py:ComponentInfo (474), ComponentCoordinator.describe (2223), list_components (2339) | port |
| A14-65 | Archive path errors | zip extraction: "Archive contains an unsafe path: {name}" (absolute or `..`); native tar: "Archive contains an unsafe path: {name}" and "Archive contains an unsafe link: {linkname}" (link target outside the release root); both check cancel per member. Install always removes the `.staging/<id>.<pid>` folder in a `finally`, and checks cancel again after payload validation, before FINALIZING. | components.py:_safe_extract (1350), _safe_extract_native_tar (1596), _install_component (1865) | port |
| A14-66 | Download error strings (verbatim) | invalid_size "The download has an invalid size."; unsafe_partial "The download could not be created safely."; partial_not_regular "The partial download is not a regular file."; partial_unreadable "The partial download could not be read."; canceled "The download was canceled."; bad_resume "The download server returned an invalid resume response."; bad_length "The download server returned an invalid download size."; unexpected_length "The download server returned an unexpected download size."; too_much_data "The download server returned more data than expected."; incomplete "The download did not complete ({done} of {total})."; integrity "The download failed its integrity check and was discarded. Please try again."; finalize "The verified download could not be finalized. Please retry." Exceptions: DownloadError, DownloadInterrupted (prefix kept for resume), DownloadCanceled. Progress phases `downloading` / `verifying`. | verified_download.py:MESSAGES (49), DownloadError (68-77) | port |
| A14-67 | Speaker-model strings (verbatim) | Blocked: "Hugging Face downloads are disabled by HF_HUB_OFFLINE, so the speaker model can't download." / "Hugging Face access is set to Never connect, so the speaker model can't download." Failures: "Hugging Face did not respond in time, so the speaker model did not download." / "Could not reach Hugging Face to download the speaker model (network error)." / "Hugging Face refused the speaker model download (HTTP {status})." / "Could not save the speaker model: {strerror}." / "Could not download the speaker model: {first line, 160 chars or exception type}." / "The speaker model download produced no file." / "The speaker model failed its integrity check and was discarded." (file also deleted). Success clears the kept error. Note: unlike every other component, the bytes come from `hf_hub_download(repo, filename, revision="main", local_dir=...)`, then SHA-256 is verified after the fact; revision "main" is not pinned. `_first_onnx_file` walks the folder (sorted names, first `.onnx`, case-insensitive); an env path that is a file must end `.onnx` else warning and ignored; an env directory with no `.onnx` logs a warning. | components.py:896-1073 | other-area (diarization); port the strings |
| A14-68 | Speech-runtime row copy (verbatim, NVIDIA rows) | All share: origin_label/source_label "Project ↗", source_name "Pinned upstream archives", origin/source URL `https://github.com/NVIDIA/NeMo-Speech.cpp`, maintainer "Upstream publishers; packaged by OpenWhisper", family "Speech runtime", compact_tags "Local speech", limitations ("Model weights are a separate download.",), best_for = summary, source_note = `_SOURCE_NOTE` + " Speech runtimes install third-party executable software. Integrity checks do not guarantee security; keep runtimes updated." where `_SOURCE_NOTE` = "Download URLs and SHA-256 pins ship with the app. Archives are verified before extract, and an interruption leaves either the previous install or the new one — never a mixture." Per row (summary; license): CPU "CPU runtime shared by Parakeet and Nemotron." (Apache-2.0; Python PSF); CUDA "NVIDIA GPU runtime shared by Parakeet and Nemotron." (Apache-2.0; NVIDIA CUDA; Python PSF); Vulkan "Vulkan GPU runtime shared by Parakeet and Nemotron, for NVIDIA GPUs older than Turing, such as the GTX 10 series." (Apache-2.0); Metal "Apple GPU runtime shared by Parakeet and Nemotron on Apple Silicon Macs. Auto uses it once installed." (Apache-2.0). `requires`: cpu "Windows x64, Linux x86_64 (glibc 2.31+), or macOS (Apple Silicon or Intel)"; cuda "Windows x64 or Linux x86_64 (glibc 2.31+), and an NVIDIA GPU (Turing or newer) with its driver"; vulkan "Linux x86_64 (glibc 2.31+), and an NVIDIA GPU older than Turing with its driver and the Vulkan loader"; metal "Apple Silicon Mac, macOS 14 or newer". `payload`: cpu/cuda "Verified native libraries; portable Python on Windows"; vulkan/metal "Verified native libraries". `source_urls` also lists the Python 3.12.10 download page (drop in FioriVoce). `get_component_details` raises KeyError for unknown ids. | component_catalog.py:196-297 | port (rewrite payload / local_format / "Runs in a dedicated process"; drop the Python line and PSF license) |
| A14-69 | Speech-model copy (verbatim) | Parakeet: description/best_for "Fast dictation and files", language "25 European languages". Orukeet: "Community adaptation of Parakeet for multilingual transcription", languages "25 European languages, including Russian and English", origin_name "Orukeet, based on NVIDIA Parakeet TDT v3", maintainer "Oruk AI (adaptation); NVIDIA (base model)", relative_performance "Publisher reports improvements over Parakeet; results depend on datasets and decoding.", extra limitations "Community model; independently evaluate quality on your recordings." / "Validated on Windows/Linux CPU and CUDA; Orukeet on Apple Silicon has not been tested." / "One reported benchmark split was also used for training and checkpoint selection." / "CC BY-SA 4.0 weights retain attribution and applicable ShareAlike terms." Nemotron: "Nemotron 3.5 ASR 0.6B", purpose "Live speech recognition", license "OpenMDW-1.1", languages "Multilingual; coverage varies by locale". Shared detail fields: task_support "Speech transcription"; memory_guidance "CPU RAM / GPU memory use depends on model and audio length."; limitations pair for parakeet/nemotron: "Requires its optional runtime from Downloads (Windows x64, Linux x86_64, or Apple Silicon Mac CPU)." + default device note "CPU and NVIDIA GPU use separately installed runtimes."; license_url `<repo URL>#license` (Orukeet: `https://huggingface.co/oruk/orukeet/blob/main/NOTICE.md`); source_urls = (repo URL, `https://github.com/NVIDIA/NeMo-Speech.cpp`); download_hosts = sorted unique URL hostnames; `download_size` shows "Unknown" when the size is 0. `MODEL_SECURITY_NOTICE`: "Models are provided by third-party publishers. Model files and speech runtimes can have security vulnerabilities. Keep OpenWhisper and its runtimes updated, and review important transcripts for errors." (rename the app). `SpeechModel` also carries `streaming`, `meeting` and `stream_final` flags (parakeet and orukeet: meeting only; nemotron: streaming + meeting). | local_asr/catalog.py:MODELS (25), model_catalog.py:31-35, 494-545 | port (parakeet, orukeet); undecided (nemotron) |
| A14-70 | Consent / error classification tables | `_OFFLINE_MARKERS` (lowercased substring match on the joined cause chain): "name resolution", "getaddrinfo", "name or service not known", "nodename nor servname", "network is unreachable", "no route to host", "connection refused", "connecterror", "failed to establish a new connection", "max retries exceeded", "could not reach the download server", "11001"; typed matches `socket.gaierror` / `ConnectionError` / `TimeoutError`. `_DISK_FULL_MARKERS`: "no space left on device", "[errno 28]", "not enough space on the disk", "[winerror 112]"; typed: errno ENOSPC or winerror 112. Order of checks: canceled/cancelled, then disk full, then offline, else generic. Disk message size comes from the bundled download size text with the leading `~` stripped ("about 714 MB"). The error text ends with "in Downloads." so the status line links there. Persisted policy values "ask"/"always"/"never"; `hf_access_coordinator` is a process-wide singleton; `requests_in_flight` counter; `grant_once` logs. | hf_access.py:82-137, 444-543 | port |
| A14-71 | Native result decode | The transcript is `result_transcript(result, 0)` UTF-8 (not the join of words); words come from `result_word_count/text/start_time/end_time(result, 0, i)` with times in ms divided by 1000; `final` = `result_is_final`, `end` = `result_audio_processed`. `transcribe` passes `recognize_f32(handle, &options, samples, n, 16000, &result)`. The sample rate constant is 16000 and every result is destroyed even on error. Language `"auto"` or empty becomes a null `language_code`. | nvidia.py:_options (216), _result (229), transcribe (244) | port (verify src/engine matches) |
| A14-72 | macOS recognizer errors | "The Mac speech runtime supports the Apple GPU or CPU." for any device other than cpu/metal; a "metal" load with no `libggml-metal.dylib` beside the library: "Install NVIDIA Speech GPU (Metal) in Downloads, or choose CPU." | nvidia.py:153-158 | other-platform |
| A14-73 | Cancel and reload flags | `reload_model` returns immediately if the cancel event is already set, then `cleanup()` (generation++) and `reset_cancel_flag()`; clears `cpu_fallback_note`; `ValueError("Model does not belong to this backend")` if the chosen key is another family's. `cancel_transcription` sets `should_cancel` then `cleanup()`. `_request_audio` raises "Transcription canceled" before the request and again after it returns; a missing engine raises `last_error` or "Speech engine is not loaded". `download_and_load(progress)` = `cache.download` then `reload_model`. `last_loaded_model` = model name only while available. A load failure keeps `last_error = str(exc)` only if the generation is still current, and logs "{Name} failed to load on {device}: {exc}" only when not canceled. `is_transcribing` is cleared in `finally`. `transcribe(audio_path, recognition)` with no recognition joins plain `windows()` decode, with recognition uses `recognition_inputs` language and phrases. `transcribe_windows` returns per-window texts only (no segments); window decode there calls `_transcribe_audio` per 30 s window. | optional_backend.py:100-220, 339-400; base.py | port |
| A14-74 | `windows()` details | `frame.pts = None` before resampling; the resampler is flushed with `resample(None)` after the last frame, then `finish()` emits the remainder; samples are int16 / 32768 float32; offset reported in seconds = samples / 16000; remaining pieces are concatenated only once per window. `WindowSplitter.total` (samples pushed) and `.offset` are public. | audio.py:39-105 | port |
| A14-75 | Remote backend extra strings (undecided) | Backend id `"remote"`. `device_info` falls back to "Remote engine is not connected". "This computer's pairing with {host_name} is missing its token. Pair again in Settings → Remote engine." (pairing present, token missing). "The host changed engines. Reconnect before changing its runtime." and "Update OpenWhisper on the host to change its runtime from here." (model/runtime switch preconditions). Busy wait: `delay` starts 1 s, x1.5 per retry capped at 5 s, honors `retry_after_s`, clamped to remaining budget; the wait checks Cancel every 0.1 s. `ENGINE_RELOADING` error drops the link and reports decode-not-started. A `RemoteConnectionLost` that was `sent` is never resent. Timing counters (requests, round trip total, host total, timed count) accumulate per request. | remote_backend.py:402-423, 715-810 | undecided |

## Modules

### services/components.py (2348 lines)
- **Responsibility**: the only catalog of component download URLs; install, verify,
  extract, validate, swap, uninstall, recover; component state for the UI; meeting-agent
  payload resolution; speaker-model download.
- **Public surface**: `COMPONENT_API`, `GPU_COMPONENT_VERSION`, `MEETING_AGENT_*`,
  `PLATFORM_*`, `SPEAKER_MODEL_*`, `ComponentId`, `ComponentState`, `InstallPhase`,
  `ComponentError` (message shown verbatim), `ComponentCanceled`, `ComponentInfo`,
  `current_platform_tag`, `catalog_entry_for_platform`, `available_component_ids`,
  `component_is_published`, `gpu_runtime_available`, `component_dir`, `staging_dir`,
  `cache_dir`, `is_installed`, `read_manifest`, `check_compatibility`,
  `installed_size_bytes`, `prune_orphans`, `install_component`, `uninstall_component`,
  `meeting_agent_payload_dir`, `meeting_agent_needs_update`, `speaker_model_path`,
  `ensure_speaker_model`, `speaker_model_error`, `speaker_model_download_blocked`,
  `ComponentCoordinator` / `component_coordinator`. Also `_download_verified` is reused by
  `local_asr/cache.py` (private import across modules).
- **Threading**: installs on a worker (`controller.component_executor`), UI on Qt thread;
  progress callback called off the Qt thread. Coordinator, size cache and speaker-model
  lock are thread-safe.
- **State machine (install)**: RESOLVING -> (DOWNLOADING <-> VERIFYING per archive) ->
  EXTRACTING -> validate -> FINALIZING -> swap. Cancel checked per chunk, per member and
  before the swap; the swap itself retries while locked and restores the old tree.
- **State machine (on disk)**: absent -> `.staging/<id>.<pid>` -> `<id>` with
  `manifest.json` + `.installed`; during swap `<id>.old`; recovery in `prune_orphans`.
- **Edge cases to keep**: sentinel is written last (2032) and is the only "installed"
  signal (705); manifest rejects non-dict, non-string platform, bool/non-int
  `component_api`, non-string `python_abi`, bool/non-scalar `version` (1076-1101);
  never delete the only complete rollback after a failed restore (1196-1202); cached
  archives deleted only after commit (1917-1920); runtimes re-check cached archive hash
  (1940-1949, `_discard_mismatched_archive`); tar members outside `root` or with `..`
  rejected and hard-link names rewritten (1620-1630); node member name rejects `/`, `~/`,
  `..` (1491); exactly one node binary (1477, 1515); `describe` survives missing catalog
  entry (2231-2290); update reason only when reclaimed > 0 (2315-2323).

### services/component_catalog.py (297)
Copy only. `ComponentDetails` dataclass (21 fields), `COMPONENT_CATALOG`,
`get_component_details`. Shared `_SOURCE_NOTE` (47) and `_SPEECH_RUNTIME_SOURCE_NOTE`
(260). Speech runtime rows are generated from `_SPEECH_RUNTIMES` (197) with
`_SPEECH_RUNTIME_REQUIRES` (243) and `_speech_runtime_payload` (266). Port the NVIDIA
rows; rewrite `payload` ("Verified native libraries; portable Python on Windows") and
`local_format` ("Isolated worker process") since FioriVoce runs the runtime in process.

### services/component_leases.py (43)
`acquire_component(id)` returns an idempotent release; `component_mutation(id)` context
manager. RLock-guarded counters. Only OpenCode uses it (components.py:1849-1862). A C++
port would apply it to speech runtimes too, since an in-process engine holds the runtime
library loaded (and FioriVoce never unloads it: see Engine-specific code).

### services/component_runtime.py (195)
Startup activation of installed components (Windows `os.add_dll_directory` + PATH prepend,
Linux RTLD_GLOBAL preload). Runtime IDs return success with no action (145-147).
`activate_components` never raises and never mutates the store. Only gpu-accel needs it:
**drop**. Keep the idea "a damaged component must not block startup".

### services/gpu_info.py (364)
- Port: `NvidiaGpu`, `_nvml_candidates`, `_load_nvml`, `_query_nvml` (96-byte name
  buffer, `nvmlInit_v2`, `nvmlDeviceGetCount_v2`, `nvmlDeviceGetHandleByIndex_v2`,
  `nvmlDeviceGetName`, `nvmlDeviceGetMemoryInfo`, optional
  `nvmlDeviceGetCudaComputeCapability`, always `nvmlShutdown`), `nvidia_gpu` (cached once,
  lock, never raises), `format_gb` ("{x:.1f} GB" of MiB/1024).
- Drop: `ctranslate2_cuda_types`, `CUDA_COMPUTE_PREFERENCE`, `AUTO_GPU_MODELS`,
  `estimate_vram_mib`, `fits`, `plan_cuda`, `describe_plan`, `memory_guidance`,
  `_ARCHITECTURES` (Whisper only).

### services/gpu_setup.py (88)
`plan_gpu_setup` for the "Use this GPU" offer (install GPU Acceleration + switch Whisper
model + model download). Whisper only: **drop**. The NeMo equivalent is A14-26.

### services/hf_access.py (543)
- Port: `AccessDecision`, `ConsentAction`, `HuggingFaceAccessCoordinator` (thread-safe
  grants and request slots), `describe_model_download_error`, `format_download_size`,
  `CachedModelInfo`, snapshot cache (`peek_cached_models`, `invalidate_cached_models_snapshot`,
  `scan_cached_models` with `max_age_seconds`, a condition variable so concurrent scans
  share one walk, and an epoch so a scan that raced a mutation is discarded (380-394)).
  For FioriVoce the "scan" is just `cache.inventory()` over speech-models; no HF hub
  cache walk.
- Drop: `resolve_model_repo`, `_progress_tqdm_class`, `download_model_files` (worker),
  `_download_model_files_in_process` (snapshot_download), `get_hf_cache_dir`,
  `delete_model_from_cache` HF branch, `custom_model_cache_info`, the Whisper branch of
  `is_model_cached`.
- Note: the consent flow is named after Hugging Face but for speech models the bytes come
  through `cache.download` (pinned URLs on huggingface.co); the policy still gates it.

### services/isolated.py (313), services/isolated_worker.py (197)
Process isolation for faster-whisper and for OpenAI uploads (so cancel can kill a stalled
DNS/upload), worker auth via a 64-hex token over loopback, parent-death watch, stdout
redirected to stderr. **Drop** the plumbing. If the API backend is kept, reproduce the
behavior, not the mechanism: cancel must abort in-flight HTTP immediately
(`QNetworkReply::abort`), uploads never retry automatically, a client prepared for one
(base_url, key, timeout) is never used for another (isolated_worker.py:53-63).

### services/local_asr/catalog.py (304)
Model and runtime catalog and the device/runtime resolution rules (A14-24..A14-27).
`RUNTIME_IDS` order matters for Downloads display order (85-88).
`CUDA_MIN_COMPUTE_CAPABILITY = (7, 5)`. `selected_device`: parakeet_mlx {auto,cpu};
moonshine always cpu; others {auto,cpu,cuda}, default auto (118-126). `runtime_catalog`
merges per-platform JSON files (277-304; Windows files read with `utf-8-sig`).

### services/local_asr/cache.py (124)
Speech model cache (A14-29..A14-31). Locks: per-key `threading.Lock` plus `filelock`
`<target>.lock` (cross-process, because downloads ran in killable workers). Keep a
cross-process lock in C++ (`QLockFile`) only if two FioriVoce processes or an OpenWhisper
install may share the directory (open question).

### services/local_asr/audio.py (105)
`SAMPLE_RATE 16000`, `MAX_SAMPLES 30 s`, `MIN_SAMPLES 24 s`, block 1600 samples (0.1 s).
`split_point` (20-25): length < 30 s -> whole; else MIN + (argmin of mean square per block
over [24 s, 30 s)) + 1 block. `WindowSplitter.push` joins pieces only when >= 30 s pending
(perf note: 133 ms saved on 88.8 s); `finish` emits the remainder. `resampler()` is shared
with incremental dictation, which must match `windows()` bit for bit (28-36): in C++ the
same resampler instance type and settings must back both paths. `_MONO_PCM_OPTIONS`
`probesize=4096` when `wav_metadata.is_mono_pcm16` (17 ms saved per dictation >= 5 s).

### services/local_asr/languages.py (55)
A14-35. Also `_QWEN_LANGUAGES` / `qwen_language_name` (drop).

### services/local_asr/nvidia.py (317)
ctypes binding of the NeMo-Speech.cpp v1 C ABI (struct layouts at 15-46 mirror the
vendored `asr.h`). `NvidiaRecognizer`: load per platform, bind 18 functions, create with
`BackendConfig{size, gpu}` (gpu -1 for CPU), `ModelConfig{size, path, name=NULL}`,
`transcribe` (`recognize_f32` at 16 kHz), `stream`/`cancel_stream`/`close`, `_result`
(always destroys the result). Speech-context keepalive must outlive the request or stream
(65-78, 180-183): FioriVoce CLAUDE.md already states this rule. Constants: `_SEGMENT_MAX_WORDS
24`, `WORD_BOOSTING False`, `BOOST_SCORE 2.0`, `MAX_BOOST_PHRASES 50`, ggml device types
GPU=1, IGPU=2.

### services/local_asr/process.py (293), worker.py (287)
Worker process (Windows: the runtime's embedded `python.exe`; elsewhere the app's
interpreter or the frozen app with `--local-asr-worker`), env `PYTHONUTF8=1`,
`HF_HUB_OFFLINE=1`, `TRANSFORMERS_OFFLINE=1`; one serialized request at a time; replies
matched by id (stale ids ignored); progress lines; last 25 stderr lines kept and logged on
crash; `close` = terminate, wait 1.5 s, kill, wait 0.5 s. **Drop**. Behaviors to carry
over in-process: serialized engine access (FioriVoce: `EngineWorker` thread), load timeout
300 s and request timeout 300 s are replaced by cancel flags, crash isolation is lost (see
Risks).

### services/local_asr/{mlx,moonshine,qwen_batch,qwen_speech,transcribe_engine}.py
Engine adapters for dropped engines. **Drop**. Notable only as reference:
`transcribe_engine.STREAM_GEOMETRY` (Parakeet Unified live preview, 5.6 s / 1.04 s /
1.04 s) shows how OpenWhisper got streaming-quality live text; FioriVoce previews Parakeet
TDT by window re-decode instead.

### services/local_asr/*.json
- `models.json`: port parakeet-v3, orukeet-v0.1 (and nemotron-3.5 if kept) verbatim.
- `nvidia_*_runtime.json` (8 files): port the NeMo-Speech.cpp archive entries. Windows
  entries also pin the Python 3.12.10 embeddable zip (11,133,606 bytes): drop that
  archive in FioriVoce and recompute `install_bytes`.
- `mlx_runtime.json`, `moonshine*_runtime.json`, `qwen*_runtime.json`,
  `transcribe_*_runtime.json`: drop.

### services/model_catalog.py (599)
`ModelDetails` (21 fields + `download_size`, `compact_tags`), `format_download_mb`,
`MODEL_CATALOG`, `MODEL_REPOSITORIES`, `MODEL_DOWNLOAD_SIZE_MB`, `get_model_details`,
`custom_model_details`, notices. Port the speech-model part (437-546) and the notices;
drop the Whisper entries and custom models.

### services/models.py (418)
SQLAlchemy ORM schema. Belongs functionally to the database area; listed under Data.
`HISTORY_CONTEXT_COLUMNS` and `history_context` (30-52) raise `TypeError` for unknown
keys; `TranscriptionHistory.create` sets uuid4 id and UTC ISO timestamp;
`preview_text` = "Empty transcript" or first 100 chars cut at last space + "...".

### services/native_libraries.py (151)
CUDA pip-wheel DLL registration (Windows), Linux CUDA preload, Qt 6.11 ICU (`icuuc.dll`)
directory registration for PyQt6. **Drop**: a C++ Qt build deploys its own ICU or uses
`windeployqt`; note the ICU dependency for packaging.

### services/openai_retirement.py (69)
A14-58. `retired(today)`, `is_model_gone_error(exc)` (status 404 or code
`model_not_found`). Undecided.

### services/opencode_catalog.py (42), services/opencode_component.py (214)
A14-61. Env allow-list `_ALLOWED_ENV` (20-27), required files `_required()` (44-51),
payload schema 1, SHA inventory, self-test (10 s version, 45 s test),
`clean_stale_runtime_dirs` for `openwhisper-opencode-<pid>-<id>` temp roots. Other area;
not offered (unpublished).

### services/verified_download.py (437), services/verified_files.py (103)
A14-09..A14-13. `sha256_file(path, regular_only)` refuses symlinks / multi-link files;
`free_space_shortfall`; `retry_while_locked` (lock WinErrors 5, 32, 33); `CHUNK_BYTES` 1 MiB;
`SPACE_MARGIN` 1.15. Also used by the app updater (other area): keep this a standalone,
Qt-free module in C++ (`verified_files` is imported by a windowless updater helper).

### services/whisper_sources.py (211), services/whisper_models.json (53)
Custom CTranslate2 Whisper sources and pinned Whisper revisions. **Drop**.

### transcriber/base.py (83)
Abstract `TranscriptionBackend` (A14-49). Port as the C++ backend interface.

### transcriber/local_backend.py (618)
faster-whisper backend. **Drop** (A14-51). `GpuFallbackCause` names are re-exported from
`transcriber/__init__.py` and used by the application controller: the controller port
must not depend on them.

### transcriber/optional_backend.py (507)
`LocalSpeechBackend` + `SpeechDecoder`. The heart of what FioriVoce keeps (A14-42..A14-48).
- **Locks**: `_state_lock` (RLock) guards process/model/generation; `_decode_lock` makes
  one decode at a time; preview and warm-up use non-blocking acquire.
- **Generation protocol**: a load captures `generation`; after the worker answers, it
  publishes only if generation unchanged and not canceled (126-131). Incremental
  dictation compares `generation` across windows so text from before a reload is never
  joined with text after (90-98).
- **Qwen branches** (`_qwen_has_speech`, `_transcribe_batched`, `_decode_batch`): drop.
- **Temp files** `openwhisper-asr-*/audio.f32` to pass audio to the worker (348-352): drop.

### transcriber/openai_backend.py (397)
A14-56. `CHUNK_UPLOAD_CONCURRENCY = 3`; `_HINT_REJECTIONS = {400, 422}`; client rebuilt
when (base_url, key, timeout) changes; `is_available` never builds the client (UI thread)
and returns false after a failed build for the same connection; cancel closes the client.

### transcriber/remote_backend.py (856)
A14-59. Subclass of `LocalSpeechBackend` whose "process" is a `RemoteConnection`.
Constants `BUSY_RETRY_S 90`, `BUSY_RETRY_MAX_DELAY_S 5`, `SWITCH_REQUEST_TIMEOUT_S 300`.
Dataclasses `RemoteLink`, `RemoteTiming` (`network_s = max(0, round_trip - host)`),
`HostModel`, `RemoteModels`; `parse_host_models` (None for hosts that do not list).
`restore_link` returns "connected" / "changed" / "offline" / "unpaired". Cancel keeps the
connection (`abandon()`), unlike a local engine. No warm-up.

## Data and formats

### Settings keys (openwhisper_settings.json; defined in services/settings.py)
| Key | Type | Default | Read where | Port |
|---|---|---|---|---|
| `local_asr_models` | dict backend -> model key | {} (not in settings table) | catalog.py:selected_model | port |
| `local_asr_devices` | dict backend -> "auto"/"cpu"/"cuda" | {} -> "auto" | catalog.py:selected_device | port |
| `local_asr_language` | str | "en" | optional_backend.py:request_language via languages.selected_language | port |
| `hf_access_policy` | "ask"/"always"/"never" | "ask"; migrated from legacy `hf_hub_offline` (true -> never) and invalid values rewritten | settings.py:load_hf_access_policy (1341) | port |
| `hf_hub_offline` | legacy bool | removed on migration | same | port (migration only, if reading OpenWhisper settings) |
| `whisper_model`, `whisper_device`, `whisper_compute_type`, `custom_whisper_models`, `whisper_gpu_offer_declined` | | | local_backend, gpu_setup, whisper_sources | drop |
| meeting keys (`meeting_asr_model`, `meeting_speech_separate`, `meeting_whisper_model`) | | | settings.py:resolve_meeting_* | other-area |

### Environment variables
`HF_HUB_OFFLINE` (truthy "1"/"on"/"true"/"yes", case-insensitive, stripped) blocks every
model download, including non-HF hosts; `OPENWHISPER_SPEAKER_MODEL`;
`OPENWHISPER_WORKER_PORT`/`_TOKEN` (drop); `OPENWHISPER_DATA_DIR` (other area).

### Files on disk
- `local_app_dir()`: Windows `%LOCALAPPDATA%\OpenWhisper`, macOS
  `~/Library/Application Support/OpenWhisper`, Linux `$XDG_DATA_HOME/OpenWhisper` or
  `~/.local/share/OpenWhisper` (config.py:122).
- Components: `<local_app_dir>/components/` with `<id>/`, `<id>/manifest.json` (the
  catalog entry, `json.dump(indent=2)`), `<id>/.installed` (version string),
  `<id>.old/`, `.staging/<id>.<pid>/`, `.cache/<archive name>` and `.cache/<name>.part`.
- NeMo runtime layouts inside `<id>/`: Windows `bin/nemo_speech_asr_c.dll` (+ embedded
  Python files in OpenWhisper installs); Linux and macOS `nemo-speech/lib/libnemo_speech_asr_c.{so,dylib}`
  plus `libggml*.{so,dylib}`. FioriVoce's `find_runtime_library` already accepts both.
- Speech models: `<local_app_dir>/speech-models/<key>/<files>` + `installed.json`
  (`json.dumps(spec)`; equality compared after parsing), siblings `<key>.partial/`,
  `<key>.previous/`, `<key>.lock`.
- Speaker model: `<local_app_dir>/models/speaker-id/voxceleb_resnet34_LM.onnx`.
- Temp (drop): `%TEMP%/whisper-worker-<pid>-*.npy`, `openwhisper-asr-*/audio.f32`,
  `openwhisper-opencode-verify-*`, `openwhisper-opencode-<pid>-<id>/`.
- Hugging Face hub cache (Whisper only): drop.

An existing OpenWhisper install already has these trees. FioriVoce can reuse them byte
for byte (same layout and `installed.json` rule) if it points at the same folder; see
open questions.

### Catalog JSON shapes
Runtime entry: `{version, component_api, platform, install_bytes, archives:[{name, url,
sha256, size_bytes, extract, root?, member?}], published?, python_abi?, macos_min?}`.
Extract kinds: `zip`, `python-wheel`, `nvidia-wheel`, `node-exe`, `node-tar`, `nemo-tar`,
`transcribe-tar`. Model entry: `{repo, revision, files:[{name, url, sha256, size_bytes}]}`.
NeMo-Speech.cpp v0.1.0 archive pins: see `services/local_asr/nvidia_*_runtime.json`
(Windows cpu 4,730,421 B zip; Windows cuda 106,044,768 B; Linux cpu/cuda/vulkan tar.gz with
`root` `nemo-speech-0.1.0-linux-x86_64-<variant>`; macOS archives have root `nemo-speech`).

### Wire protocols
- **Worker JSON lines** (drop): request `{id, op, ...}`; reply `{id, result}` or
  `{id, error, code?, status_code?}`; progress `{id, progress:[done, total]}`. Ops:
  `load{backend, model, model_path, runtime, device}` -> `{device, gpu?}`;
  `transcribe{audio_path(.f32 raw little-endian float32), language, phrases?}` ->
  `{text, segments:[{text,start,end}]}`; `stream{session, finish}` -> `{events}`;
  `transcribe_batch`; `cancel_stream{session}`; `shutdown`. Isolated ops: `whisper_load`,
  `whisper_transcribe`, `download_model`, `discover_whisper_models`, `openai_prepare`,
  `openai_transcribe`, `ping`. `code: "unsupported_gpu"` drives the Auto CPU fallback.
- **OpenAI transcription** (undecided): `POST {base_url or https://api.openai.com/v1}/audio/transcriptions`,
  multipart `file`, `model`, `response_format` ("text" or "json"), optional `prompt`,
  `language`, `languages[]`, `keywords[]`; response plain text or `{"text": ...}`. Read
  timeout = endpoint `timeout_s` (default 120 s), connect 10 s, `max_retries=0`.
- **Remote engine** (undecided, protocol owned by `services/remote_asr`): requests
  `transcribe` / `stream` / `cancel_stream` with `audio` (float32 samples), `language`,
  options; `select_model{family, model}`, `configure_runtime{family, model, settings}`;
  `recording_activity("recording_begin"|"recording_end", id)`. Ready payload fields read
  here: `engine{family, model, label, device, compute_type, status, available}`,
  `host{name}`, `models[{family, model, label}]`, `runtime{selected{language},
  gpu{name, total_mib}}`, `capabilities{recording_activity, engine_controls,
  recognition_hints}`. Errors: `RemoteRequestError{retryable, retry_after_s, code}`
  (`protocol.ENGINE_RELOADING`), `RemoteConnectionLost{sent}`, `RemoteDecodeNotStarted`,
  `RemoteEngineError`.

### Database tables (services/models.py)
`schema_version(version PK)`; `transcription_history(id PK, text, raw_text, timestamp,
model, audio_file, transcription_time, audio_duration, file_size, cleanup_provider,
cleanup_model, source_name, title, origin_device_id, origin_device_name, app_id, app_name,
app_category, cleanup_level, entry_kind, cleaned_text, language; idx timestamp)`;
`dictation_stats(id PK autoinc, entry_id, timestamp, entry_kind, app_name, app_category,
cleanup_level, spoken_words, final_words, words_edited, audio_seconds, language, target,
snippets, snippet_words, corrections, shots; idx timestamp)`; `meeting_sessions(id PK,
title, status, started_at, ended_at, paused_total_s, host_token, guest_token,
cloud_enabled, asr_model, asr_remote_json, agent_provider, agent_model,
agent_endpoint_json, spool_dir, state_json, state_seq, app_pid, app_heartbeat_at,
origin_device_id, origin_device_name)`; `meeting_audio_chunks(id, meeting_id FK cascade,
channel, seq, file_path, start_s, duration_s, sample_rate, asr_status, asr_attempts,
asr_error; unique(meeting_id, channel, seq))`; `meeting_segments(id, meeting_id, chunk_id,
channel, start_s, end_s, text, speaker_participant_id, speaker_source, speaker_pinned,
embedding BLOB, created_at)`; `meeting_segment_delivery(seq autoinc, segment_id unique,
meeting_id)`; `meeting_agent_delivery_cursors(meeting_id, consumer PK, seq)`;
`meeting_participants`; `meeting_state_items`; `meeting_questions`; `meeting_events`;
`record_sync(kind, record_id PK, host_fingerprint, action, state, attempts, last_error,
content_digest, created_at, updated_at)`. The database area owns migrations; an existing
`openwhisper.db` depends on these exact names.

## External dependencies

| Python / OS API | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| `urllib.request` + `ssl` (system trust, Windows proxy) | component and model downloads | `QNetworkAccessManager` (system proxy via `QNetworkProxyFactory::setUseSystemConfiguration(true)`, system CA store), or libcurl with schannel/OS trust |
| `hashlib.sha256` | archive and file digests | `QCryptographicHash::Sha256` (incremental) |
| `zipfile` | zip extraction | minizip-ng or libzip (vcpkg), or libarchive |
| `tarfile` (gz, xz, "data" filter) | NeMo tar.gz, Node tar.xz | libarchive (vcpkg) with explicit path, link and type checks (no "data" filter exists: implement it) |
| `shutil.disk_usage` | free space | `QStorageInfo::bytesAvailable` / `std::filesystem::space` |
| `os.replace`, `os.fsync`, `O_NOFOLLOW`, `os.chmod` | atomic swaps, durable writes | `std::filesystem::rename` / `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, `QSaveFile` for small files, `fsync`/`FlushFileBuffers`, `open(O_NOFOLLOW)` on POSIX, `FILE_FLAG_OPEN_REPARSE_POINT` check on Windows |
| `filelock.FileLock` | cross-process model cache lock | `QLockFile` |
| `ctypes` + NVML | GPU name, VRAM, compute capability | `SharedLibrary` (exists in src/engine) + NVML function pointers |
| `ctypes` + libggml | Vulkan device list (Linux) | same `SharedLibrary`, `ggml_backend_dev_*` |
| `ctypes` NeMo-Speech.cpp | recognizer | `NemoApi` (exists; add `streaming_recognize`, `stream_*`, `result_is_final` to `FV_NEMO_ASR_FUNCTIONS` if streaming is kept) |
| `av` (PyAV / FFmpeg) | decode any audio/video, resample to 16 kHz mono s16 | FFmpeg libavformat/libavcodec/libswresample (vcpkg `ffmpeg`), or `QAudioDecoder` (limited formats; not bit-exact with FFmpeg) |
| `numpy` | windows, energy, noise | plain C++ (`std::vector<float>`), `std::mt19937` for warm-up noise (bit-exact noise not required) |
| `huggingface_hub`, `faster_whisper`, `ctranslate2`, `tqdm` | Whisper models, speaker model download, CUDA count | drop; speaker model: direct pinned HTTPS URL through the verified downloader |
| `openai` SDK + `httpx` | API backend | `QNetworkAccessManager` + `QHttpMultiPart` (if kept) |
| `websockets` (via remote_asr) | remote engine | `QWebSocket` (if kept) |
| `subprocess`, `socket`, `secrets` | worker processes, node/bun probes | drop for speech; `QProcess` for Node probes if the meeting agent is kept |
| `sqlalchemy` | DB schema | SQLite C API or `QSqlDatabase` (database area) |
| Windows `kernel32` OpenProcess/WaitForSingleObject | dead-PID check for temp cleanup | drop (only for worker temp files) |

## Platform-specific code

- **Windows**: NeMo runtime DLL in `bin/`; OpenWhisper's runtime bundles an embedded
  Python 3.12 and writes `python312._pth` (components.py:1747): FioriVoce needs neither.
  NVML at `nvml.dll` or `%ProgramFiles%\NVIDIA Corporation\NVSMI\nvml.dll`. No Vulkan
  release, so a pre-Turing card gets no GPU runtime offer (test_local_asr.py:193-200).
  Locked-file retries (WinError 5/32/33, and 15 s swap retry for runtimes). No directory
  fsync. `CREATE_NO_WINDOW` for child processes. Winerror 112 / 5 disk messages.
- **Linux x86_64**: tar.gz releases renamed to `nemo-speech/`; preload system
  `libstdc++.so.6` before the release library (release bundles an older one); Vulkan
  release for pre-Turing cards with the NVIDIA device picked by name among ggml GPUs;
  driver and loader error strings (A14-39); NVML `libnvidia-ml.so.1`; executables need
  0755. glibc 2.31+ stated in requires copy. X11 / Wayland / Hyprland: nothing in this
  area differs.
- **Linux aarch64**: no speech runtimes (only meeting agents).
- **macOS arm64**: Metal and CPU NeMo releases (`libggml-metal.dylib` proves Metal),
  Auto means Metal, saved "cuda" means Metal, Auto keeps an installed CPU runtime until
  Metal is installed; relative symlinks inside the release must be preserved (and
  counted once by size); `macos_min` gating (only Moonshine and Qwen use it).
- **macOS x86_64**: NeMo CPU only.

## Engine-specific code

**Drop (other engines or worker plumbing)**: local_backend.py; gpu_info.py Whisper
planning; gpu_setup.py; native_libraries.py; component_runtime.py (gpu-accel only);
gpu-accel catalog entries, `_REQUIRED_GPU_DLLS`, `_validate_gpu_payload`,
`_safe_extract_nvidia_wheel`, `gpu_runtime_available`; whisper_sources.py;
whisper_models.json; Whisper parts of model_catalog.py and hf_access.py; isolated.py;
isolated_worker.py; local_asr/process.py; local_asr/worker.py; mlx.py; moonshine.py;
qwen_batch.py; qwen_speech.py; transcribe_engine.py; their runtime JSON files; macOS
wheel-tree validation (`_MACOS_WHEEL_RUNTIME_FILES`, `_validate_mlx_runtime`);
`python-wheel` and `transcribe-tar` extractors; Qwen branches in optional_backend.py.

**Must adapt to an in-process Parakeet engine**:
1. **Runtime component layout**: Windows OpenWhisper archives pair the NeMo zip with the
   Python embeddable zip; FioriVoce pins only the NeMo zip. If FioriVoce reuses an
   OpenWhisper component tree, the extra Python files are harmless; the manifest
   `version` ("0.1.0-py312") would then differ from a FioriVoce catalog version and show
   "Update available" unless the versions are aligned (decide).
2. **Never unload**: FioriVoce cannot `FreeLibrary` the runtime (crashes on 0.1.0), so an
   update or removal of the runtime that is loaded must be refused or deferred until
   restart ("Restart FioriVoce to finish removing ..."), unlike Python, which killed the
   worker first (services/runtime/components.py:74-76, 104-111). The component lease
   (A14-20) is the natural guard.
3. **Cancel**: Python cancels by killing the worker (2 s budget). In process there is no
   kill: cancel must set a flag checked between windows and drop results of a stale
   generation; a decode already inside `recognize_f32` runs to completion (one window,
   <= 30 s audio). The generation protocol (A14-45) carries over unchanged.
4. **Crash isolation is lost**: a native crash in the runtime now takes the app down. The
   "The speech engine stopped unexpectedly" path becomes unreachable; consider an
   optional out-of-process host later (risk).
5. **Auto device without CTranslate2**: `_auto_device` counts CUDA devices with
   CTranslate2. Replace with NVML presence (or `cuDeviceGetCount` from `nvcuda.dll` /
   `libcuda.so.1`), keeping: Vulkan runtime selected for a pre-Turing card even when CUDA
   cannot count it (catalog.py:206-209).
6. **GPU load failure on Auto**: map a NeMo GPU `create` failure on Auto to a CPU retry
   with `cpu_fallback_note`, the same contract as `unsupported_gpu`.
7. **Audio to engine**: no temp `.f32` files; pass `std::span<const float>` directly.
8. **Streaming / live preview**: Parakeet TDT is offline-only; preview = periodic
   `preview_audio` window decode (non-blocking lock). Nemotron streaming (A14-41) only if
   Nemotron is kept.
9. **Word boosting** is off in OpenWhisper for the only engine that would use it; Parakeet
   `recognition_support` is "after" (dictionary applied after decode).

## Dependencies on other areas

**Calls out**:
- `config`: `local_app_dir`, `components_root`, `bundle_root`, `is_frozen`,
  `host_machine`, `config.SPEECH_WARMUP_BACKENDS`, `config.API_MODEL_CHOICES`,
  `config.DEFAULT_API_MODEL`, Whisper defaults (drop).
- `services/settings`: `SettingsKey.*`, `settings_manager.load_all_settings`,
  `load_hf_access_policy`, `save_hf_access_policy`, `is_hf_hub_offline_env_set`,
  `serving_api_model`, `HuggingFaceAccessPolicy`.
- `services/format_utils`: `format_size_bytes`, `format_timestamp`.
- `services/http_tls.verified_context` (TLS context for components).
- `services/process_env.prepend_for_app` (PATH changes; drop).
- `services/wav_metadata.is_mono_pcm16` (fast-open check).
- `services/recognition_context`: `hotwords`, `language_code`, `keywords`,
  `vocabulary_prompt`, `engine_support`, `SUPPORT_MODEL`, `SUPPORT_AFTER`.
- `services/speech_endpoints`: `selected_endpoint`, `endpoint_model`,
  `configured_endpoint`, `api_key`, `OPENAI_ENDPOINT_ID`, `SpeechEndpoint{name, base_url,
  timeout_s, is_openai, auto_language}`.
- `services/audio_processor`: `check_file_size`, `split_audio_file`,
  `combine_transcriptions`, `cleanup_temp_files` (API backend).
- `services/remote_asr`: `client.RemoteConnection` (`connect`, `request`,
  `request_timed`, `recording_activity`, `abandon`, `close`, `alive`, `closed`,
  `latency`, `beats`, `via_tailscale`, `where`, `ready`, `closed_for_engine_change`),
  `settings.load_client_pairing/load_client_token` (pairing `host, port, fingerprint,
  alternates, host_name, name_for()`), `protocol.ENGINE_RELOADING`, errors.
- `meeting.recovery._pid_alive` (OpenCode temp cleanup).
- `_version.__version__` (User-Agent).

**Called in by**: `main.py` (worker entry flags, `prune_orphans`,
`activate_components`, native library bootstrap); `ui_qt/bootstrap.py` (log preloaded
libraries); `services/application_controller.py` (`close_all_workers` on quit, backends);
`services/runtime/{components,models,transcription,streaming,remote,meeting}.py`
(install/uninstall worker, backend construction and reload, warm-up, preview, cleanup of
orphan preview audio); `services/incremental_dictation.py` (`resampler`,
`WindowSplitter`, `decode_window`, `generation`); `meeting/asr/engine.py`,
`meeting/asr/remote.py`, `meeting/refinalize.py` (`SpeechDecoder`, speaker model);
`meeting/agent/opencode_sidecar.py` (OpenCode payload, leases);
`services/remote_asr/{engines,runtime,model_management,dependencies}.py` (host side
serves `LocalSpeechBackend.recognize`/`stream_audio`, model catalog, runtimes);
`services/guided_setup.py`, `services/meeting_setup.py`, `services/dictation_language.py`,
`services/diagnostics.py`, `services/app_update*.py` (verified download/files),
`services/database.py`, `services/history_manager.py`, `services/dictation_stats.py`,
`services/backup.py`, `services/remote_records/*`, `services/agent_api/store.py`,
`services/agent_mcp/controls.py` (ORM models); UI: `settings_downloads.py`,
`settings_models.py`, `settings_search.py`, `component_details_dialog.py`,
`component_row_widget.py`, `model_row_widget.py`, `local_model_picker.py`,
`local_engine_controls.py`, `speech_backend_picker.py`, `hf_consent_dialog.py`,
`required_runtime_dialog.py`, `use_gpu_dialog.py`, `custom_whisper_dialog.py`,
`remote_models.py`, `remote_engine_controls.py`, `history_*`.

## Tests

| Test file | Pins | Golden-vector candidate |
|---|---|---|
| tests/test_components.py (1518) | sentinel semantics, prune_orphans restore/preserve, manifest shape rejection (incl. bool), compatibility, coordinator exclusivity and cancel_all, describe states incl. external/update/incompatible/missing entry, availability per platform (206-301), published gate, swap rollback on second rename failure (399), uninstall leftovers, safe extractors (traversal, nvidia flattening, node exe/tar), Linux release folder rename and outside-folder rejection (1424-1466), mac symlink handling (1382), installed size cache (1472-1508), free-space check, catalog immutability, speaker model resolution/download/blocking/errors | `check_compatibility`, `component_is_published`, `available_component_ids(tag)`, `read_manifest` acceptance table, `describe` state matrix |
| tests/test_component_download.py (107) | overall progress across archives, cancel keeps `.part`, bad Content-Range discarded, corrupt download discarded, network failure keeps partial, certificate message names the host | resume/restart decision table (status, Content-Range, Content-Length, existing size) |
| tests/test_component_catalog.py (83) | every component id has copy, https sources, links, immutability | copy strings |
| tests/test_local_asr.py (854) | pins have integrity metadata; Linux offers; CUDA vs Vulkan by capability (87-105); `resolve_runtime` matrix (107-127); `gpu_runtime_offer`/`missing_runtime` matrix (145-223); Vulkan NVIDIA index (226-233); Linux libstdc++ order and driver message; settings never cross families (341); cache atomicity, offline block, rollback (349-391); missing runtime messages (394-440); cancel during load (453); windows bit-exactness vs faster_whisper decode, mono fast-open (480-560); chunk offsets 0/24.1/48.2 (562); worker reply ids, cancel, timeout, crash (582-620) | `split_point`, `WindowSplitter` vs `windows()`, `_transcribe_audio` offsets, `resolve_runtime`, `runtime_id`, `nvidia_device_index`, `selected_model`/`selected_device` |
| tests/test_local_asr_languages.py (196) | language choices, selected_language fallbacks, Nemotron zh -> zh-CN for file and stream, Qwen names (drop) | `selected_language`, `native_language_code` |
| tests/test_hf_access.py (400) | policy decisions incl. env beats grant, grant consumed once, advisory keeps grant, per-model grants; error-text classification (offline, disk full, other, boundaries); snapshot reuse and peek; request slots and claim_batch | `evaluate_access` decision table, `describe_model_download_error` |
| tests/test_model_download_safety.py (94) | pinned revisions, Orukeet license/notice, verification wording per host | strings |
| tests/test_model_catalog.py (90) | catalog completeness, https sources, sizes match download config, parameter counts | `_speech_model_details` output for parakeet/orukeet |
| tests/test_engine_warmup.py (514) | warm-up input is quiet non-silent noise through transcribe; failure non-fatal; skips unloaded/busy; cleanup/reload during warm-up; only SPEECH_WARMUP_BACKENDS warmed; controller ordering (warm then clear busy then preview) | behavior only |
| tests/test_gpu_info.py (161) | NVML absent -> None; rest is Whisper planning (drop) | `NvidiaGpu.short_name` |
| tests/test_isolated_cancellation.py (327) | cancel reaps stalled worker, quit during load, deadline, chunk rejection interrupts others, orphan preview cleanup | behavior only (adapt to in-process cancel) |
| tests/test_openai_backend.py (339), test_api_upload_warmup.py (441), test_openai_retirement.py | chunk order, overlap, cap 3, cancel paths, temp cleanup, response format, mid-upload model stability; warm standby rules; retirement dates and retry | `transcription_hints`, `response_format`, `retired(date)`, `is_model_gone_error` (undecided) |
| tests/test_remote_engine.py (1953) | link states, busy retry, reconnect once, cancel keeps connection, model switching, timing split; plus protocol/pairing (remote area) | `parse_host_models`, `RemoteTiming.network_s` (undecided) |
| tests/test_linux_gpu_component.py, test_use_gpu_offer.py, test_local_backend_fallback.py, test_custom_whisper.py, test_native_libraries.py, test_parakeet_mlx*.py, test_parakeet_unified.py, test_qwen_*.py, test_macos_runtimes.py | dropped engines and gpu-accel | none |
| tests/test_local_transcription_contracts.py (146) | local engine takes over-limit files whole (`large_file_size_mb` None), worker job shows split a backend reports | behavior |

Golden vectors to generate from Python: (1) `split_point` and `WindowSplitter` window
boundaries for synthetic signals (constant, sine, silence gaps, random) fed in varying
frame sizes; (2) `windows()` sample output for reference WAV/MP3/M4A files (tolerance
1e-4 if a different resampler is used); (3) `segments_from_words` on word lists; (4)
`resolve_runtime`/`runtime_id`/`gpu_runtime_offer`/`missing_runtime` over a grid of
(platform, installed set, GPU capability, CUDA count, device setting); (5)
`check_compatibility` / `component_is_published` / `read_manifest` on manifest fixtures;
(6) `evaluate_access` grid; (7) `describe_model_download_error` and
`describe_network_error` on error descriptors; (8) the verified-download resume decision
table against a scripted HTTP server.

## Porting notes

**Decomposition**
- Qt-free core (`src/engine`, namespace `fv`):
  - `component_catalog` (embedded JSON resources for NeMo runtimes and models.json;
    platform tag; published gate; compatibility),
  - `verified_download` core logic split from transport: a `.part` manager + resume
    decision function + SHA-256 (transport injected as an interface so the Qt layer
    provides `QNetworkAccessManager` and tests a fake),
  - `archive_extract` (zip + tar.gz via libarchive with explicit safety checks),
  - `component_store` (install/swap/uninstall/prune/describe/size cache),
  - `model_cache` (speech-models),
  - `gpu_probe` (NVML, ggml Vulkan device index),
  - `runtime_resolver` (A14-24..A14-26),
  - `audio_windows` (FFmpeg decode + resample + `WindowSplitter`; or keep decode in an
    `audio` area module and only the splitter here),
  - `local_speech_backend` logic (window loop, silence gate, join, warm-up input,
    generation counter) on top of the existing `Recognizer`.
- Qt layer (`src/app`): `ComponentRuntime`-style QObject driving installs on a worker
  thread with throttled progress signals, consent coordinator glue, `EngineWorker`
  integration (all recognizer calls stay on its thread), network transport.

**Order of work**
1. Verified download + SHA + free space + error strings (also needed by the updater area).
2. Catalog data + platform tag + published/compat + describe states.
3. Extractors and install/swap/prune/uninstall for NeMo runtime archives.
4. Speech model cache + consent coordinator + download error text.
5. NVML probe + runtime resolver + first-use GPU offer.
6. Audio windows + window decode loop + warm-up + preview/decode_window/SpeechDecoder on
   `EngineWorker`.
7. Undecided backends (API, remote, Nemotron streaming) once decided.

**Risks**
- In-process native crashes now kill the app (no worker isolation).
- Runtime cannot be unloaded: updating or removing the loaded runtime needs a restart path.
- tar "data" filter semantics must be reimplemented by hand (symlink targets inside the
  tree, no device files, no absolute links, safe modes); macOS releases rely on relative
  symlinks.
- Bit-exactness of `windows()` with FFmpeg's resampler: incremental dictation relies on
  the live path and the file path producing identical windows; use one resampler
  implementation for both.
- Sharing folders with an installed OpenWhisper: concurrent writers (needs `QLockFile`),
  version strings causing spurious "Update available", and OpenWhisper deleting a tree
  FioriVoce has loaded.
- Proxy / enterprise TLS behavior differs between urllib and Qt networking; the cert error
  text must still trigger on verification failures.

**Rough C++ size**: kept scope about 3,500-4,500 lines (core ~2,800: download 450,
extract 400, component store 700, catalog/compat 300, model cache 250, consent 200, GPU
probe/resolver 350, windows + backend logic 500; Qt glue ~600; tests ~1,000 extra). Add
~600 for the API backend and ~900 for the remote backend if kept.

**Native gains**
- No worker processes: no 0.16 s process start, no 0.7 s SDK import, no temp `.f32`
  round trip per window, no 2 s reap on cancel; load and first decode faster.
- Audio passed as spans; WindowSplitter without numpy concatenation copies.
- NVML probe and catalog are cheap enough to run at startup synchronously.
- Install size walk can use `FindFirstFileExW` with `FIND_FIRST_EX_LARGE_FETCH` for speed.
- Warm standby for uploads (A14-57) becomes unnecessary: a pre-opened TLS connection via
  `QNetworkAccessManager::connectToHostEncrypted` gives the same benefit.
