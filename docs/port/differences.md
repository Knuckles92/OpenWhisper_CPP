# Deliberate differences from OpenWhisper

## a01-app-shell

### a01-no-checkout-data-root

OpenWhisper. A source run keeps its data in the working directory or the checkout when that holds openwhisper_settings.json or .git, and .env falls back to <checkout>/.env.

OpenWhisper_CPP. The data root is OPENWHISPER_CPP_DATA_DIR or the per-user folder however the program was started, and .env is only <data_root>/.env.

The checkout rule exists for `python main.py`; a compiled program has no checkout, and a working-directory rule would let a launch from another folder open different data. (docs/port/ARCHITECTURE.md 3.1 'A source checkout never becomes the data root (... deliberate difference)'; docs/port/packages/M0.md Scope rows A01-01 and A01-03)

### a01-packaged-registered-only

OpenWhisper. The diagnostics report's 'packaged' is sys.frozen: true for every frozen build, installed or unpacked by hand.

OpenWhisper_CPP. 'packaged' is true only for a copy that runs from the folder its installer registration names; a copy unpacked by hand or run from a build tree reports false.

A native executable has no frozen flag, and OpenWhisper_CPP ships through its setup only, so the registration is what marks an installed copy. (docs/port/audit/a01-app-shell.json A01-29's note; src/update/install_probe.cpp, src/ui_shell/early_cli.cpp, src/cli/commands.cpp; tests/ui_shell/test_diagnostics_packaged.cpp ('the diagnostics report is packaged only in its registered install folder'), tests/cli/test_diagnostics_packaged.cpp ('openwhisper-cpp-cli in the build tree reports itself not packaged'))

### a01-single-instance-every-platform

OpenWhisper. Only Linux enforces one UI per data folder (flock in $XDG_RUNTIME_DIR plus an abstract socket, both named with the uid); Windows and macOS get a no-op primary and rely on the frozen build's mutex or the Dock.

OpenWhisper_CPP. Every platform enforces it with a QLockFile in the temporary folder and a QLocalServer (a named pipe on Windows) keyed by the same data-folder hash; a second launch brings the first window forward. The lock and server names carry the account between the prefix and the key: the uid on POSIX, as OpenWhisper's Linux names do, and on Windows the first eight hex digits of the SHA-256 of the lower-case account name, so two accounts sharing one data folder do not collide.

The updater's application mutex does not bring the running window forward and does not separate data folders; one mechanism serves every platform. Windows has no uid, and its pipe names are one namespace for the whole machine. (docs/port/packages/M0.md Scope row A01-31 ('M0.11 (all platforms, deliberate difference)') and the map's porting note for services/ui_instance.py; the shell sweep of 2026-11-19 (open item ui_shell/single_instance.cpp): src/ui_shell/posix/instance_user_posix.cpp, src/ui_shell/win/instance_user_win.cpp (not yet compiled on Windows), tests/ui_shell/test_sweep_instance_user.cpp ('the instance name carries the account between the prefix and the key', "another account's lock on the same data folder does not keep this one out"))

## a02-ui-foundation

### a02-local-stylesheet-helper

OpenWhisper. An application-wide event filter adopts any stylesheet set on a widget at its StyleChange event as the designed source and resolves its tokens and font sizes at once.

OpenWhisper_CPP. Widgets set token sheets through setLocalStyleSheet, which stores the source and resolves it; a lint forbids raw setStyleSheet in ui_* code, and a sheet set directly anyway is adopted as source at the next theme or scale change.

One resolution pass per theme change and no filter on every widget's StyleChange. (docs/port/ARCHITECTURE.md section 13 decision 39 'Widget-local stylesheets'; docs/port/packages/M0.md Scope row A02-11)

## a03-ui-controller

### a03-cpu-runtime-offer-after-crash

OpenWhisper. OpenWhisper makes no offer at start after its speech engine crashed in the previous session.

OpenWhisper_CPP. When the last session's crash log names a module in the speech runtime's folder and the runtime setting is not the CPU, the next start offers to switch to the CPU runtime. The offer is made once per crash, remembered in '<crash log>.offered' whether it is taken or declined; a later crash is offered again.

The runtime is loaded into OpenWhisper_CPP's own process, so a GPU runtime that crashes ends the app; the CPU runtime is the way back, and a declined offer must not come back at every start. (The shell sweep of 2026-11-19 (open item ui_controller/engine_setup.cpp); src/ui_controller/engine_setup.cpp, src/ui_controller/controller_feature.cpp; tests/ui_controller/test_sweep_crash_offer.cpp ('a declined crash offer does not come back at the next start', 'crashAlreadyOffered matches the time, code and module'))

### a03-quit-aborts-http

OpenWhisper. At quit OpenWhisper leaves requests, an installed-agent probe and a model adoption still running on its daemon threads; the process exits without waiting for them.

OpenWhisper_CPP. Quit's first cancellation step aborts every HTTP transfer (http::AbortableClient::abort_all), stops the installed-agent scan and kills its probe's process tree, and cancels an adoption of OpenWhisper's runtime or model, whose copy or hash stops and whose staging is removed; Quit still waits for the io task that ran each, so a request to a server that never answers holds Quit for about a second, not for its idle timeout.

A C++ task that uses the application context cannot be abandoned while that context is destroyed, so Quit waits for running io work; cancelling that work bounds the wait. (The shell sweep of 2026-11-19 (open item ui_shell/bootstrap.cpp waitForRunningTasks); src/ui_shell/bootstrap.cpp (ShutdownStep::CancelRoot), src/http/abortable_client.cpp; tests/ui_shell/test_sweep_quit_http.cpp ('a request to a server that never answers does not hold Quit'), tests/http/test_sweep_abortable_client.cpp ('abort_all ends a request that waits on a silent server'); tests/ui_shell/test_sweep_quit_agent_scan.cpp ('an agent scan waiting on a probe that never answers does not hold Quit'), tests/ui_controller/test_sweep_adoption_quit.cpp ('sweep: Quit cancels the adoption the engine setup is running'), tests/models/test_sweep_adoption_cancel.cpp ('sweep: Quit during a model's adoption leaves nothing half adopted'))

## a04-runtime-services

### a04-agent-cleanup-scope

OpenWhisper. Command Mode and transforms can use the selected installed-agent cleanup provider to rewrite selected text with dictionary and caret context.

OpenWhisper_CPP. An approved CLI can clean live dictation started with the local UI or a hotkey. It receives the spoken words and cleanup instructions. The app keeps surrounding text, selections, learned global rules and dictionary entries local. Command Mode and transforms use the selected text-model endpoint.

The person dictating chooses which installed agent may receive their speech. Rewriting text selected in another application requires separate authorization. (src/ui_controller/personalize_wiring.cpp, src/dictation/cleanup_stage.cpp, tests/dictation/test_agent_cleanup_scope.cpp and tests/ui_controller/test_agent_cleanup_consent.cpp)

### a04-meeting-insights-per-meeting

OpenWhisper. Turning on AI insights, at a meeting's start or with the toggle during it, asks once for informed consent and stores meeting_cloud_consent_given; later starts and toggles go ahead without asking, and the toggle reports 'AI insights on' / 'AI insights off' ('AI insights stay off' when declined).

OpenWhisper_CPP. AI insights consent is asked for every meeting and names its destination (the agent route and its backup); none is stored and an earlier answer approves nothing. A declined start runs transcript-only; during a meeting AI insights turn on only with a desktop approval for that meeting's destination. The guest link ('No guest link available yet') is unchanged.

Sending a meeting's transcript to a hosted model or an installed agent is authorized per meeting and per destination, never by a stored grant. (docs/port/open-items.md, the 'SSD integration branch' section (per-meeting external destinations); tests/meeting/test_runtime_consent.cpp ('every start with AI insights asks again and carries only its own approval', 'test_toggle_cloud_without_consent_reprompts'))

### a04-meeting-speaker-pass-desktop-only

OpenWhisper. The post-meeting card offers a speaker re-run when the saved speaker backend is openai and audio-upload consent is stored (naming what blocks it otherwise), and retrying the speaker_id step uploads the meeting audio to OpenAI; the other retry gates (backup, local model first, one claim per generation, agent writes for the run, busy refusal) apply.

OpenWhisper_CPP. The retry gates are kept (tests/meeting/test_runtime_retry.cpp), but the card never offers a speaker re-run from saved settings and a retried speaker_id step uploads nothing; the optional OpenAI speaker pass runs only from the explicit Past Meetings action, approved for that one meeting each time, and sends only the system-audio recording.

Saved settings, stored consent, End and retries must not start an upload of meeting audio; each upload needs the user's fresh, explicit action. (docs/port/open-items.md, the 'SSD integration branch' section and 'The optional OpenAI speaker pass runs from the desktop Past Meetings action'; tests/meeting/test_cloud_speakers.cpp ('each saved speaker pass asks again and sends only system audio after approval'), tests/meeting/test_runtime_consent.cpp ('the card never offers a speaker re-run from saved settings'), tests/meeting/test_runtime_retry.cpp ('test_desktop_speaker_retry_never_uploads_when_ineligible'))

### a04-refused-preview-no-fallback

OpenWhisper. When a recording's live-preview start is refused because the previous recording's window is still decoding, an empty final transcript falls back to the preview text the previous recording kept and shows it with 'No speech detected — showing the live preview (not saved)'.

OpenWhisper_CPP. A recording whose preview never started has no preview to fall back on and gets the empty-transcript message; the previous recording keeps its own fallback.

The kept text belongs to another recording. (The dictation sweep of 2026-11-19 (open item dictation/streaming_runtime.cpp); src/dictation/streaming_runtime.cpp; tests/dictation/test_sweep_refused_preview.cpp ('a recording whose preview start was refused has no preview to fall back on', 'a refused recording stopped as busy leaves the previous recording its fallback'))

### a04-refused-stop-ends-preview

OpenWhisper. A stop whose job claim is refused (a backup, a host update or another job holds the slot) publishes recording_canceled, reports the busy notice and returns with the live preview still streaming over the stopped recording. Nothing stops it later, so the next recording's start_streaming refuses ('Streaming worker is still active') while start_streaming_session still shows the preview overlay, which then follows the old worker and its text.

OpenWhisper_CPP. The refused stop also stops the window preview (StreamingRuntime::stop_session), so the next recording starts a preview of its own and its overlay shows only its own text. The busy notice, the recording_canceled event and the empty overlay state are unchanged.

OpenWhisper's behaviour is a fault: no worker owns the refused recording, so its preview is never stopped and the next recording's preview is lost or shows the old text. No user relies on it. (The lead's decision of 2026-11-19 on the A04-10 audit note; src/dictation/transcription_runtime.cpp stop_recording (the commented stop_session on a refused claim); tests/dictation/test_streaming_quit.cpp ('a stop refused as busy stops the preview for the next recording') and tests/dictation/test_streaming_runtime.cpp ('a stop that claims no job leaves the next recording its preview'))

## a10-settings-services

### a10-fvbackup-format

OpenWhisper. A backup is an .owbackup ZIP with format id openwhisper-backup whose core members are data/openwhisper_settings.json and data/openwhisper.db; scheduled ones are named OpenWhisper-auto-*.owbackup.

OpenWhisper_CPP. A backup is an .fvbackup ZIP with format id openwhisper-cpp-backup and members named from identity() (data/openwhisper_cpp_settings.json, data/openwhisper-cpp.db), with OpenWhisper's layout, limits and validation rules otherwise. OpenWhisper_CPP inspects and restores both kinds; OpenWhisper cannot read an .fvbackup.

OpenWhisper_CPP is a separate product with its own data root and file names; OpenWhisper's names are read only to import its data or restore its backups. (docs/port/ARCHITECTURE.md, the identity names (AppIdentity: settings_file, restore_dir, backup_extension) and the backup format paragraph ('.fvbackup has OpenWhisper's layout and validation rules'))

### a10-import-unreadable-settings

OpenWhisper. OpenWhisper imports no other installation. Its own start runs on default settings when its settings JSON cannot be read and skips the legacy history migration when transcription_history.json cannot be merged; its backup create refuses both.

OpenWhisper_CPP. Importing an OpenWhisper installation whose settings JSON cannot be read brings the default settings with the warning "OpenWhisper's settings could not be read; the default settings were imported", and a legacy transcription_history.json that cannot be merged is left out with the warning 'Could not merge transcription_history.json: <reason>'. A folder in place of the settings file still refuses the import. The warnings are logged.

The import brings what OpenWhisper's own next start would run on, rather than refusing data the user can still use. (The import sweep of 2026-11-19 (open item backup/import.cpp (M3.7)); src/backup/import.cpp; tests/backup/test_sweep_import.cpp ('settings OpenWhisper cannot read import as its defaults', "a folder in place of OpenWhisper's settings still refuses the import", 'a legacy history OpenWhisper cannot read is left out of the import'))

### a10-meeting-speech-stays-local

OpenWhisper. When meetings follow the dictation engine and dictation runs on the paired computer, resolve_meeting_speech answers source remote, so meeting audio goes to that computer too.

OpenWhisper_CPP. The same settings resolve to this computer's Parakeet model (source local); a meeting uses the paired computer only when the meeting has its own remote source and that host is confirmed for the meeting.

A dictation preference does not authorize sending meeting audio to another computer; it is the settings half of difference a25-remote-meeting-consent. (src/models/meeting_speech.cpp and tests/models/test_meeting_speech.cpp ('Remote dictation never chooses the paired computer for meeting speech'); docs/port/differences.md a25-remote-meeting-consent)

### a10-openai-speakers-read-local

OpenWhisper. A saved meeting_speaker_id_backend of openai is used until OpenAI retires the model on 2027-02-26 and reads as local from that day.

OpenWhisper_CPP. A saved openai reads as local from the start; the optional OpenAI speaker pass runs only from the Past Meetings action, with a fresh confirmation for one saved meeting.

End-of-meeting processing, dashboard retries and stored grants must not start an upload of meeting audio on their own. (docs/port/packages/M7.md 'Changes to the cut' item 4 (lead decision 4) and docs/port/open-items.md ('The optional OpenAI speaker pass runs from the desktop Past Meetings action'); src/models/meeting_speech.cpp resolve_meeting_speaker_id_backend, tests/models/test_meeting_speech.cpp)

## a11-settings-dialog-ui

### a11-no-staged-settings-build

OpenWhisper. SettingsDialog(staged=True) builds the window in generator steps of about 100 ms that the controller runs 2500 ms after start, one per 60 ms, retrying each second while recording or in a meeting.

OpenWhisper_CPP. The window is built when first opened and each page is built on its first visit; there is no idle builder.

Lazy pages already keep the first opening fast, and an idle builder adds a second construction path for no gain in the C++ app. (docs/port/ARCHITECTURE.md 8.5 ('The Settings window's staged idle build is dropped; pages stay lazily built on first visit') and docs/port/packages/M2.md 'Changes to the cut' item 6)

## a13-personalize-ui

### a13-cleanup-agent-apply-and-consent

OpenWhisper. In Settings > AI cleanup the agent tiles are always shown above the Cleanup model tile, and clicking a tile saves that agent, or the built-in model, at once.

OpenWhisper_CPP. The tiles appear after choosing Installed agent… in the provider dropdown, or while an agent is saved. A click only stages a tile; Apply runner commits it, and an agent is saved only after a confirmation that names it as the destination. Applying the built-in API / local model tile clears the approval, and a model picked for a staged agent is saved with Apply.

An installed agent receives the dictated words, so choosing one is confirmed for the named agent before anything is saved or sent. (README.md, AI cleanup for dictation (the flow of commit 139a97b); src/ui_personalize/cleanup_model_section.cpp; tests/ui_personalize/test_cleanup_agent_choice.cpp and tests/ui_controller/test_agent_cleanup_consent.cpp)

### a13-rule-dictation-no-journal

OpenWhisper. Rule dictation ends each take with clear_recording_data(), which leaves one recording journal folder per rule dictation under the temporary folder.

OpenWhisper_CPP. Rule dictation ends each take with clear_recording_data(true), which discards the take's journal, so no folder is left behind.

A rule's spoken words are not a dictation to recover. (The final sweep of 2026-11-19 (open item ui_personalize/cleanup_page.cpp addCleanupHomeRows); src/ui_personalize/rule_dictation.cpp; tests/ui_personalize/test_sweep_rule_journal.cpp ('sweep: a rule dictation leaves no recording journal behind'))

## a14-models-components-services

### a14-auto-device-fallback

OpenWhisper. The optional backend implements automatic CPU fallback for selected engines, including Qwen and transcribe.cpp.

OpenWhisper_CPP. A Parakeet model on Auto retries on the CPU when GPU initialization fails and reports the reason. An explicitly requested GPU reports its load error.

The same device-selection rule applies to the engine shipped with OpenWhisper_CPP. (transcriber/optional_backend.py, src/speech/parakeet_backend.cpp and tests/speech/test_parakeet_backend.cpp)

### a14-in-process-runtime-copy

OpenWhisper. The NVIDIA Speech rows say the runtime is shared by Parakeet and Nemotron, 'Runs in a dedicated process', local format 'Isolated worker process', payload 'portable Python on Windows', add the Python PSF licence and link the Python 3.12.10 download page.

OpenWhisper_CPP. The rows say 'Parakeet models', 'Runs inside OpenWhisper_CPP', local format 'Native libraries loaded by OpenWhisper_CPP', payload 'Verified native libraries', and drop the Python licence and link; every other field is OpenWhisper's, with the product named by identity().

OpenWhisper_CPP loads the runtime into its own process without an embedded Python and ships only the Parakeet family, so the upstream wording would describe software it does not install. (docs/port/packages/M2.md M2.5 'Traps' (map a14 A14-21, A14-68); src/models/component_details.h and tests/models/test_component_details.cpp openwhisper_cpp_row)

### a14-local-engine

OpenWhisper. Offers multiple local speech backends. Its model catalog includes Nemotron alongside Parakeet and Orukeet.

OpenWhisper_CPP. Uses NeMo-Speech.cpp for its one local Parakeet engine. The catalog lists Parakeet TDT and its Orukeet adaptation.

The project selected this engine family for the C++ rewrite. Runtime selection permits separate CPU and GPU builds. (AGENTS.md and src/models/speech_catalog.cpp)

### a14-runtime-folder-checked

OpenWhisper. OpenWhisper has no setting for a runtime folder of the user's choosing; its runtimes come from its components only.

OpenWhisper_CPP. A runtime folder chosen in fv_runtime_dir is checked for the runtime library and its build. Without the library the engine is a missing runtime with 'No speech runtime in <dir>: <library> was not found.'; a CPU build keeps device Auto on the CPU and refuses an explicit GPU with '<dir> holds a CPU build of NeMo-Speech.cpp, so Parakeet can't use the GPU with it.'

A folder the user picks may hold no runtime or a CPU-only build; saying which before the load fails tells the user what to change. (The models sweep of 2026-11-19 (open item models/engine_resolution.cpp); src/models/engine_resolution.cpp, src/engine/runtime.cpp find_runtime_library; tests/models/test_sweep_runtime_dir.cpp ('runtime_dir_serves needs the library, and a GPU build for a GPU runtime', 'a chosen folder without the runtime library is a missing runtime', 'a chosen CPU build keeps Auto on the CPU and refuses an explicit GPU'))

### a14-staged-component-change

OpenWhisper. A component is installed, updated or removed at once; no component row waits for a restart.

OpenWhisper_CPP. An update or removal of the speech runtime that is loaded is staged and applied at the next start: its Downloads row shows 'Restart needed' and offers nothing more, and describe() gives 'Restart OpenWhisper_CPP to finish installing the component.' or 'Restart OpenWhisper_CPP to finish removing the component.'

The NeMo-Speech.cpp runtime runs inside OpenWhisper_CPP's process and cannot be unloaded (FreeLibrary crashes the process), so a change to the loaded runtime waits for the next start. (The models sweep of 2026-11-19 (open item describe() and is_installed()); src/models/coordinator.cpp describe, src/models/downloads_model.cpp; tests/models/test_component_lifecycle.cpp ('describe() names a change staged for the next start, and the row offers nothing more', 'removing the loaded runtime is staged and done at the next start'))

## a16-updater-setup-packaging

### a16-cleanup

OpenWhisper. The reference prunes abandoned directories by age, including older transactions without journals.

OpenWhisper_CPP. OpenWhisper_CPP cleans verified files from terminal transactions, and the downloaded archive their journal names (as the reference does in every terminal cleanup), and leaves unknown contents and unauthenticated legacy backups for inspection; only an archive a crash left behind waits for the pruning of stale downloads. A later setup can recover its own interrupted backup.

Age alone cannot establish that the files belong to this updater. (docs/port/map/a16-updater-setup-packaging.md, A16-27)

### a16-code-identity-installed

OpenWhisper. The code-changed prompt compares the installed executable's inode, size and mtime, or in a git checkout the HEAD commit read from .git.

OpenWhisper_CPP. Only the installed executable's identity (file index, size, modification time) is compared; there is no git-checkout identity.

OpenWhisper_CPP never runs from a Python source tree, so the lazily imported code a git checkout could change under a running app does not exist. (docs/port/packages/M3.md, Changes to the cut 9; tests/update/test_code_identity.cpp; tests/ui_controller/test_code_change_restart.cpp)

### a16-failure-reported-once

OpenWhisper. updater_helper.py shows a native message box for every update failure, and after a rolled-back commit the relaunched application shows "Update failed" again from apply_error.txt, so the user is told twice.

OpenWhisper_CPP. openwhisper-cpp-updater stays silent only when a rolled-back commit started the previous version again and the failure was left in apply_error.txt for it to show; every other failure (no journal, a rollback that itself failed, a relaunch or an apply_error.txt write that failed, any recovery) gets the helper's box.

One failure is reported once, by the window the user returns to; the helper still speaks whenever the application would not. (The lead's decision of 2026-11-19 on audit A16-28 (for the owner to confirm); tests/update_native/test_updater_cli.cpp ('the helper shows an update failure nobody else will show') and tests/update_native/test_transaction_runner.cpp ('a failure the application was not told about stays the helper's to report'))

### a16-health-database

OpenWhisper. A health launch records a startup failure as update_readiness_error and the acknowledgement then fails with exit code 1, so the helper rolls back. The acknowledgement runs 1500 ms after the event loop starts and reads update_readiness_error then, so a database still being initialized at that moment is acknowledged.

OpenWhisper_CPP. The database opens on first use, and one that will not open leaves the app running with failing saves, as in OpenWhisper. The acknowledgement task, 1500 ms after the event loop starts, opens the database first; when it cannot, it logs 'Update readiness failed: <error>', writes no token and exits 1, so the helper rolls back.

An update is acknowledged only once the database it runs on has opened, however long the opening takes. (tests/ui_shell/test_bootstrap.cpp ('update health waits for database and the running event loop', 'update health is never acknowledged when the database cannot be opened', 'a database that cannot be opened leaves the app running with failing saves'); the shell sweep of 2026-11-19 (open item ui_shell/bootstrap.cpp openDatabase): src/ui_shell/bootstrap.cpp, Database::deferred (tests/db/test_sweep_deferred.cpp))

### a16-helper-command-line

OpenWhisper. The helper commits with --transaction-id <id>, recovers with --recover [--parent-pid], and a cleanup copy of the helper runs --cleanup-transaction --parent-pid after the commit to delete the transaction once the helper has exited.

OpenWhisper_CPP. The command line is --apply --transaction-id <id> or --recover --transaction-id <id> [--wait-for-launcher <pid> <creation time>]; there is no cleanup copy and no --cleanup-transaction: the helper collects a finished transaction's trees and the downloaded archive its journal names in its own run, and the next start of the app collects the rest once the update gates are free.

A native helper does not need a second copy of itself to outlive its own folder: what it cannot delete while it runs from the transaction folder is authenticated and collected at the next start. (tests/update_native/test_updater_cli.cpp ('the helper leaves its launch directory and then commits the journal', 'the helper recovers the transaction it is given'); tests/update_native/test_prepare.cpp (the recovery command line); tests/update_native/test_transaction_runner.cpp ('the helper commits a prepared journal and then collects its leftovers'); tests/update_native/test_startup_recovery.cpp ('a finished transaction is collected by the next start'); src/update_native/terminal_cleanup.cpp remove_transaction_archive, tests/update_native/test_sweep_archive_cleanup.cpp ('sweep A37: the helper removes the archive once the update is healthy'))

### a16-helper-without-transaction

OpenWhisper. OpenWhisperUpdater.exe started without a transaction shows the box "OpenWhisper updater was started without a transaction." and exits 2.

OpenWhisper_CPP. openwhisper-cpp-updater started with a command line it does not know (no transaction among them) prints its usage to standard error and exits 2 without a box; --help prints the usage and exits 0.

openwhisper-cpp-updater is also the console tool the build scripts run (--write-manifest, --pack-archive, --verify-archive), and a modal box would hang an unattended build. (tests/update_native/test_updater_cli.cpp ('the helper started without a transaction does nothing and exits 2'); tests/updater/test_program.cpp; docs/port/packages/M10.md M10.11 and M10.17 (the updater writes the manifest and packs the archive))

### a16-installer-recovery

OpenWhisper. The reference installer invokes its backup helper on the Python distribution tree.

OpenWhisper_CPP. The per-user installer authenticates its staged Qt payload, snapshots managed files and can restore a failed upgrade. Setup checks the named application and update locks again immediately before writing. An elevated installation uses Inno Setup's built-in recovery.

An elevated process must not restore executable files from a per-user writable backup. (docs/port/map/a16-updater-setup-packaging.md, A16-23)

### a16-parent-exit

OpenWhisper. OpenWhisper can terminate a registered parent that outlasts its update timeout.

OpenWhisper_CPP. OpenWhisper_CPP checks the parent executable and creation time and leaves a still-running application open when the deadline expires.

The user can close the application and retry without losing an active recording. (docs/port/map/a16-updater-setup-packaging.md, A16-18)

### a16-prepared-recovery-superseded

OpenWhisper. Recovering a PREPARED transaction whose installation was never touched goes through the rollback path: it ends rolled_back, writes "The previous update was interrupted and was rolled back." and relaunches the app.

OpenWhisper_CPP. Recovering a prepared, never-started transaction ends superseded: the installation and data are left alone, nothing is reported and nothing is relaunched.

Nothing was rolled back, so reporting a rollback would be untrue, and superseded is the state a helper can never act on again. (tests/update_native/test_transaction.cpp ('recovering a prepared transaction retires it without touching the installation'); tests/update_native/test_transaction_runner.cpp)

### a16-two-channels

OpenWhisper. OpenWhisper has three install channels: installer when frozen, git when a .git folder sits beside the source, else source, with labels including "Source checkout (git)".

OpenWhisper_CPP. OpenWhisper_CPP has two: Installer when a registration names the running folder, else Source (a development build, labelled "Source copy"); there is no git channel or git hint.

OpenWhisper_CPP never runs from a Python source tree, so a git checkout of the app does not exist at run time. (docs/port/packages/M3.md, M3.2 API (enum class InstallChannel { Installer, Source }) and the test_app_update.py row of its Tests to port table (git channel and git hint dropped); change 9)

### a16-updater-log-and-unknown-command

OpenWhisper. The updater helper opens updater.log for every command line, and one without a transaction shows the box 'OpenWhisper updater was started without a transaction.' and exits 2.

OpenWhisper_CPP. openwhisper-cpp-updater opens no log for --help or the packaging commands (--write-manifest, --pack-archive, --verify-archive); a command line it does not know logs a WARNING, prints the usage to standard error and exits 2, with no box.

Build scripts run the same program: its packaging commands must not write the user's updater log, and a command line it does not know must not stop a script with a box. (docs/port/audit/a16-updater-setup-packaging.json A16-47's note; src/update_native/updater_cli.cpp; tests/update_native/test_updater_log.cpp ('the packaging commands and --help open no updater log'), tests/update_native/test_updater_cli.cpp ('the helper started without a transaction does nothing and exits 2'))

## a17-remote-engine-services

### a17-backup-transfer

OpenWhisper. The client worker waits until backup releases its transfer lock and resumes automatically.

OpenWhisper_CPP. The desktop backup holds both client and host record gates. A new manual transfer receives a busy response during backup and can be retried when the backup finishes. The gates release on cancellation and failure.

The native client transfers the specific records requested in the current session. (docs/port/map/a17-remote-engine-services.md, A17-51)

### a17-direct-history

OpenWhisper. Upstream supports searching a connected client's open history channel.

OpenWhisper_CPP. A named peer can view the latest 20 local dictations and meeting metadata during an explicitly opened 15-minute session. Disconnecting or closing the session revokes the grant.

Time and record limits bound the data exposed by a live pairing. (docs/port/map/a17-remote-engine-services.md, A17-52)

### a17-host-mcp-link

OpenWhisper. HostMcpLink polls the host's MCP state every 2 s (5 s when not ready) and HostMcpSettings/HostMcpServer let the client's MCP page change the host's settings optimistically.

OpenWhisper_CPP. There is no link or facade: the MCP page's paired-computer frame asks remote_mcp_request for the state and starts or ends the host's read-only, 15-minute session.

Pairing does not let a client change the host's MCP settings, so there is nothing to edit remotely; follows a17-paired-mcp. (docs/port/differences.md a17-paired-mcp (docs/port/map/a17-remote-engine-services.md, A17-43); src/remote_host/mcp_session.cpp)

### a17-no-precision-control

OpenWhisper. A paired computer can change the host engine's precision (compute_type) with its device and language through configure_runtime.

OpenWhisper_CPP. A OpenWhisper_CPP host refuses a compute_type change with 'This engine manages its own precision on the host.' and keeps its runtime; device and language changes are applied as in OpenWhisper.

OpenWhisper_CPP's one local engine, Parakeet on NeMo-Speech.cpp, has no precision setting to change. (src/remote_host/runtime_state.cpp validate_runtime; tests/remote_host/test_remote_runtime_client.cpp ('test_device_change_reloads_same_model_and_adopts_host_precision'))

### a17-paired-mcp

OpenWhisper. A paired operator can request changes to the host's MCP configuration, including network settings.

OpenWhisper_CPP. A host can grant one paired connection a read-only local listener for 15 minutes. Its bearer token is visible on the host and is omitted from responses to the peer. A computer without the grant is refused with '<host> hasn't granted this computer MCP control. Turn on "Grant session MCP control to one paired computer" in Settings → Remote engine there and choose this computer.'; a host built with OpenWhisper's opt-in keeps OpenWhisper's sentence.

Granting access to one paired device does not authorize changes to the host's usual settings or exposing a reusable credential. The refusal names the switch the host's Remote engine page shows. (docs/port/map/a17-remote-engine-services.md, A17-43; the remote sweep of 2026-11-19 (open item remote and remote_host: remote_client_history): src/remote_host/dispatch.cpp, tests/remote_host/test_sweep_mcp_refusal.cpp ('sweep A38: a host that grants MCP by session names the grant when it refuses', 'sweep A38: a host with OpenWhisper's MCP opt-in keeps its refusal'))

### a17-retention

OpenWhisper. Upstream can enqueue records for host retention and deliver them on reconnect.

OpenWhisper_CPP. A user selects a finished meeting, destination and whether to include its audio before a transfer. Reconnecting does not replay the transfer. Bring back returns dictations only: a meeting is shared as a copy and never leaves this computer, so the host holds none to return. A record checked out or shared is marked done, and nothing sends it back automatically.

An existing pairing does not identify which finished meeting the user wants to share. (docs/port/map/a17-remote-engine-services.md, A17-48; src/remote_records/sync.cpp RecordSync::bring_back; tests/remote_records/test_host_restore.cpp ('test_restored_host_recovery_requires_local_assignment_and_preserves_both_owners'))

## a18-remote-engine-ui

### a18-mcp-tile-grants-a-session

OpenWhisper. 'Allow paired computers to manage MCP' lets a paired computer change the host's MCP settings.

OpenWhisper_CPP. With a PeerMcpSession (the app) the tile is 'Grant session MCP control to one paired computer': it asks which computer, confirms and calls grant_mcp_session for a 15-minute read-only local listener.

Granting one paired device access does not authorize changes to the host's settings or a reusable credential. (docs/port/audit/a17-remote-engine-services.json a17-paired-mcp)

### a18-pairing-request-ends-with-page

OpenWhisper. A pairing request waiting for the host's owner keeps its worker thread waiting after Settings closes, until the host answers or the request ends.

OpenWhisper_CPP. Closing the Remote engine page cancels a pairing request still waiting.

A request whose page is gone has nowhere to show its code or its result. (The remote sweep of 2026-11-19 (open item Remote engine page: a pairing request waiting on the host's owner); src/ui_remote/remote_engine_page.cpp ~RemoteEnginePage; tests/ui_remote/test_remote_page.cpp ('closing the page cancels a pairing request still waiting'))

### a18-records-transfer-by-hand

OpenWhisper. The records-location tile keeps new records on the host automatically and describes it so.

OpenWhisper_CPP. The shipped RecordSync reports manual_transfers(), so the tile and its notes use the manual-transfer wording ('Copy or move existing dictations…', 'Choose the transfer button when you are ready.'); OpenWhisper's wording shows only without it and is pinned there.

Record sharing is explicit and per session. (docs/port/audit/a17-remote-engine-services.json a17-retention and a17-backup-transfer)

### a18-use-for-meetings-opens-the-page

OpenWhisper. 'Use for meetings' saves meeting_speech_separate and meeting_asr_source remote (use_remote_for_meetings) and opens Meeting voice; 'Use for dictation' selects the Remote computer engine.

OpenWhisper_CPP. 'Use for meetings' opens Settings on Meeting voice (useForMeetingsRequested has no receiver), where the paired host is chosen and confirmed for meeting audio. 'Use for dictation' saves the remote model selection and reloads (remote_feature.cpp); the page's half is pinned by 'settings page pairs with a host and lists the device', the wiring has no test.

A dictation preference does not authorize sending meeting audio to another computer: each remote meeting route needs its own confirmation of the named host. (docs/port/differences.md a25-remote-meeting-consent; src/ui_remote/remote_engine_page.cpp useForMeetings)

## a19-actions-automation-services

### a19-confirmed-actions

OpenWhisper. An action's words run as soon as the recording ends: a missing or turned-off action copies them with 'That action no longer exists' / 'That action is turned off', and 'Send to my scripts' publishes action.ran with the words for whatever listens.

OpenWhisper_CPP. The running app shows each action's destination and exact material (webhook body, link, program and arguments, agent prompt with its context) and sends only after the user accepts; Copy only needs no confirmation; a declined or canceled run sends nothing ('Action canceled.'), a missing or turned-off action reads 'That action is unavailable.', 'Send to my scripts' is refused, only jobs the user started are delivered, and action shaping is sent raw. Settings' Try it asks the same way before a run other than Copy only.

Dictated words leave the computer only for a destination the user has just seen; a listener count names no recipient, so script delivery fails closed. ActionRuntime itself keeps OpenWhisper's dispatch and copy and is tested as such. (src/automation/confirmed_automation.h and .cpp, src/ui_controller/action_wiring.cpp, src/dictation/transcription_runtime.cpp ('consent_declined'); tests/automation/test_confirmed_automation.cpp, tests/ui_controller/test_action_wiring.cpp; docs/port/open-items.md summary ('explicit hook and action tests ... are available', 'startup restoration of external hooks and unattended sharing remain differences'))

### a19-explicit-routing

OpenWhisper. With 'Also listen to normal dictations' on, plain dictations that start with an action's name are judged too, and the judge is asked without a prompt.

OpenWhisper_CPP. Only the Route by voice shortcut routes (ConfirmedAutomation::route returns no decision for route 'auto'); before the judge is asked the user sees its route and the exact request and confirms, and declining cancels the job instead of pasting.

The judgment sends the dictation to a model; it happens only on the explicit shortcut and after the user has seen where it goes. (src/automation/confirmed_automation.h and .cpp, src/ui_controller/action_wiring.cpp, src/dictation/transcription_runtime.cpp ('consent_declined'); tests/automation/test_confirmed_automation.cpp, tests/ui_controller/test_action_wiring.cpp; docs/port/open-items.md summary ('explicit hook and action tests ... are available', 'startup restoration of external hooks and unattended sharing remain differences'))

### a19-hooks-not-started

OpenWhisper. After the window shows, AutomationService installs the AppControl, subscribes saved webhooks and program hooks to app events and starts MCP Events delivery; saved text filters (and API leases) rewrite each dictation before and after cleanup.

OpenWhisper_CPP. Nothing constructs AutomationService: Settings > Hooks' Send test and Run test drive the engines on a private bus, no saved webhook, program hook or MCP Events subscription receives app events, and no text filter runs on dictation (ConfirmedAutomation::filter_stage is empty). The engines, filters and the service are complete and tested on their own.

External hooks are not restored at startup without the user's action; only explicit hook tests send anything. (src/ui_automation/automation_feature.cpp ('neither engine's start() is called here'), src/automation/confirmed_automation.h ('No saved filters run through this port'); tests/ui_automation/test_manual_controls.cpp; docs/port/open-items.md summary ('Persisted startup restoration of external hooks and unattended sharing remain differences'))

### a19-live-api-not-served

OpenWhisper. When API & MCP is on, the in-app listener also serves the live API (/v1/live/state, /v1/events SSE, actions, recording start/stop/cancel, filter leases, notify) through a Qt AppControl, which the channel bridge and 'Tell my Claude Code session' use.

OpenWhisper_CPP. The local MCP session is built with allow_live_tools=false; mount_live_api is never called and no UiAppControl is installed, so programs cannot read live state, stream live events, run actions, control recording, lease filters or show notices. LiveApi, SseStream, UiAppControl and FilterLeases are complete and tested on their own.

The local MCP session serves saved content; its tools exclude live dictation text, microphone control and action requests. (src/ui_automation/session_mcp.cpp (deps.allow_live_tools = false); docs/port/open-items.md summary ('Its tool list excludes live dictation text, microphone control and action requests'))

## a20-agent-api-mcp-services

### a20-host-mcp-read-only

OpenWhisper. A paired client can read and configure the host's MCP (enabled, port, Tailscale, write grants) through host_control's state() and configure().

OpenWhisper_CPP. A host grants one paired connection a read-only local listener for 15 minutes; the peer cannot change the host's MCP settings, and the token is shown on the host only.

Granting access to one paired device does not authorize changes to the host's usual settings; matches the decision recorded as a17-paired-mcp. (docs/port/differences.md a17-paired-mcp (docs/port/map/a17-remote-engine-services.md, A17-43))

### a20-listener

OpenWhisper. OpenWhisper can restart its MCP listener when a stored preference is enabled.

OpenWhisper_CPP. Starting a desktop listener requires confirmation of its loopback address and access. Its switch reflects whether the current session is running.

Reviewing access at start lets a user choose when local assistants can read saved text or change selected preferences. (docs/port/map/a20-agent-api-mcp-services.md, A20-17)

### a20-listener-interface

OpenWhisper. The upstream MCP server can bind directly to a Tailscale interface.

OpenWhisper_CPP. The desktop session listens on loopback. A separate paired-session workflow can start a temporary read-only loopback server on the host. A host can stop that session at any time.

Local desktop access and permission granted to a paired operator have separate audiences and lifetimes. (docs/port/map/a20-agent-api-mcp-services.md, A20-20)

### a20-local-mcp-saved-content

OpenWhisper. The desktop MCP server offers the live tools (get_live_state, list_actions, run_action, start/stop/cancel_dictation, recent_events, notify_user) behind the API & MCP permissions, reports those permissions in get_capabilities, and serves MCP Events (events/list, subscribe, unsubscribe and signed deliveries to subscribers' URLs).

OpenWhisper_CPP. The local MCP session is built with allow_live_tools=false: it lists and runs only the history and control tools, its instructions say it reads saved dictations and meetings, get_capabilities reports every live permission and mcp_events as false, and no agentapi::McpEvents is built, so events/* and deliveries are not offered (the API & MCP page reads 'MCP is not available in this session' for its Events controls). The live tools and McpEvents are complete and tested on their own.

The local MCP session serves saved content; its tool list excludes live dictation text, microphone control and action requests, and nothing sends events to external URLs unattended. (src/ui_automation/session_mcp.cpp (deps.allow_live_tools = false), src/agentapi/mcp_tools.cpp McpTools::listed/instructions, src/agentapi/agent_controls.cpp capabilities; tests/agentapi/test_mcp_protocol.cpp ('desktop history session excludes live tools even when live permissions were saved'); docs/port/open-items.md summary ('The local MCP session serves saved content ... Its tool list excludes live dictation text, microphone control and action requests'))

### a20-peer-history-session

OpenWhisper. The History API and MCP history tools merge paired clients' history into pages with include_clients / device_id (parallel fetch, server-side clients- cursors, per-client status).

OpenWhisper_CPP. No server is given a client history source, so include_clients and device_id read only this computer's database; a paired client's recent history is viewed instead in an explicitly opened, time-limited session (ui_remote's client history dialog). FederatedHistory exists and is tested on its own.

Time and record limits bound the data a live pairing exposes; matches the decision recorded as a17-direct-history. (docs/port/differences.md a17-direct-history (docs/port/map/a17-remote-engine-services.md, A17-52); src/agentapi/history_routes.cpp HistoryApi (federation only with a ClientHistorySource), src/ui_remote/client_history_dialog.cpp)

## a21-actions-automation-ui

### a21-history-tile

OpenWhisper. API & MCP has a 'Share this computer's history through the paired host's MCP' tile (mcpShareHistoryTile, disabled with ' Pair a host in Remote engine to enable this setting.' when unpaired) that persists remote_client_history through service.set_share_history, plus the gate bodies naming 'Allow paired computers to manage MCP', the per-computer host note and the tabs' 'Whose MCP to manage' name.

OpenWhisper_CPP. There is no persistent history-sharing consent: history is shared only in a session opened from Remote engine's 'Share history for this session…' (remote_history::Publication, at most 15 minutes, ended by closing it or disconnecting), which Settings search finds for 'share history'. RemoteEngineControl::set_share_history, which nothing called, was removed. The gate bodies and note belong to the replaced host tab (a21-paired-host-frame).

A named peer views this computer's latest dictations and meeting metadata only during an explicitly opened, time-limited session; a standing opt-in would expose history for as long as the pairing lasts. (docs/port/differences.md a17-direct-history; docs/port/audit/a20-agent-api-mcp-services.json a20-peer-history-session; tests/ui_remote/test_history_publication.cpp, tests/ui_settings/test_search_history_session.cpp)

### a21-live-cards-hidden

OpenWhisper. Settings > API & MCP shows 'Live events and automation' under the MCP cards: the Live API card (permissions, address, endpoints, curl), MCP Events (subscribe switch, count, Clear) and the Claude Code channel card.

OpenWhisper_CPP. The running app's MCP control is the local session (session_mcp.cpp, session_only() true), and McpSettingsView hides LiveAutomationSection over it; the section and its cards exist and are tested against a full listener (test_live_section.cpp, test_mcp_extras.cpp).

The local MCP session serves saved content only: the live API is never mounted and no McpEvents is built, so the cards would offer switches that do nothing. (docs/port/audit/a19-actions-automation-services.json a19-live-api-not-served and docs/port/audit/a20-agent-api-mcp-services.json a20-local-mcp-saved-content (owner commit 8cf2b8e))

### a21-paired-host-frame

OpenWhisper. When a host is paired, Settings > API & MCP shows a segmented 'This computer / <host>' bar (details 'MCP on/off', 'Host · not allowed', 'Host · needs update', 'Host · unreachable', 'Host · checking…'); the host tab is a second McpSettingsPage over HostMcpSettings/HostMcpServer/HostMcpLink that polls the host every 500 ms and changes its MCP settings, with a gate card ('<host> hasn't allowed this yet', '<host> needs an update', 'Can't reach <host>') and Check again, a note that the live cards are per computer, and it opens on the host tab when only the host's MCP runs.

OpenWhisper_CPP. McpSettingsView::bindRemote adds a 'Paired computer: <host>' frame (mcpPairedHost) below this computer's page, shown only while paired: 'Check host MCP status' reads the granted session's state once per click, 'Start the authorized host session…' asks first and sends only {enabled: true} for the fixed loopback session, 'End host session' disconnects (the host then revokes the grant). Refusals read '<host> hasn't allowed this yet.' (naming the host's grant), '<host> needs an update.' and 'Can't reach <host>.' in the frame's status line. There are no tabs, host page, gate card or note: this computer's page and cards never hide.

A peer may not change the host's MCP configuration; it only starts and ends the read-only 15-minute session the host granted it, whose token stays on the host, so there is no host page to manage and nothing to poll. (docs/port/differences.md a17-paired-mcp; docs/port/audit/a20-agent-api-mcp-services.json a20-host-mcp-read-only; tests/ui_automation/test_mcp_paired_host.cpp, tests/remote_host/test_mcp_grant.cpp, tests/remote_host/test_mcp_session.cpp)

### a21-route-status-key-part-later

OpenWhisper. Route by voice's status line is computed at once on the GUI thread, including the parts that depend on the judge's API key.

OpenWhisper_CPP. The status that needs no key (Route by voice off, for example) shows at once; a status that depends on the key is worked out on the io executor, and until that answer arrives the line is empty and the privacy caption shows.

Reading the credential store must not block the GUI thread. (The lead's decision of 2026-11-19 on open item ui_automation (M5.13); src/ui_automation/actions_page.cpp requestStatus and automation::status_without_key (src/automation/routing.h); tests/ui_automation/test_sweep_relay_status.cpp ('the route by voice status that needs no key shows before the full answer'))

## a24-meeting-engine

### a24-insights-on-after-approval

OpenWhisper. MeetingEngine.set_cloud_enabled(True/False) toggles AI insights while the meeting is active or paused: on restarts the agent with the send cursor seeded (forward-only), off stops the scheduler and core and sets finalization disabled.

OpenWhisper_CPP. set_cloud_enabled(true) is refused ('Start the meeting with AI insights to authorize its destination.'); turning insights on mid-meeting is enable_live_intelligence(route, backup) after the provider authorizes that exact destination, with the delivery cursors advanced to the high water so nothing spoken while off is sent. Turning off is set_cloud_enabled(false).

Sending a meeting's transcript to a hosted model or an installed agent needs approval of that meeting's destination each time; no stored or browser-side toggle can grant it. (Differences a04-meeting-insights-per-meeting (docs/port/audit/a04-runtime-services.json) and a30-dashboard-fresh-approval (docs/port/audit/a30-meeting-web-server-and-dashboard-contract.json); src/meeting/detail/live_intelligence.cpp set_cloud_enabled, enable_live_intelligence; tests/meeting/test_engine_end.cpp ('live intelligence can resume only with the approved exact destination', 'live note requests share the approved scheduler and refuse after opt-out'), tests/meeting/test_engine_intelligence.cpp ('test_backup_only_gets_transcript_since_insights_were_turned_back_on').)

### a24-local-fast-features

OpenWhisper. Live highlights, citations and the topic-shift judge ask the configured Fast judgments model, local or hosted; with a hosted judge and AI insights off the status is 'needs_insights'.

OpenWhisper_CPP. They ask only a judge on this computer: without one the status is 'unavailable' and nothing is sent; with one they run with or without AI insights. Status values, the heartbeat retry and the final highlights after End are otherwise OpenWhisper's.

Background judgments of meeting text stay on the device; sending text off it is reserved for the per-meeting AI insights destination. (Difference a29-local-meeting-judgments (docs/port/audit/a29-meeting-intelligence.json); src/meeting/detail/voice_control.cpp refresh_live_highlights_status; tests/meeting/test_fast_features.cpp ('fast-feature gate matrix', 'local workers start without ai insights and stop sending after a switch', 'heartbeat retries workers and updates the highlight status without new speech', 'highlight status reaches snapshots and live clients').)

### a24-native-agent-cores

OpenWhisper. The meeting agent is a TypeScript sidecar (Pi or OpenCode); End reuses the live scheduler and core for the cleanup and report passes, then _wrap_up_end stops the scheduler and shuts the core down.

OpenWhisper_CPP. The agent is the native in-process loop. The live core and scheduler are shut down when End drains capture; when the live agent was online and healthy, each final pass runs through a throwaway core of the provider, otherwise the card reads 'Meeting intelligence is offline; final cloud insights could not run. <why>' as OpenWhisper does. Wrap-up keeps final highlights (complete meetings only) and the optional insight review.

The sidecars are replaced by a native agent loop, and a live core is not kept open past the meeting. (docs/port/packages/M8.md 'Changes from section 11' item 2; the class comment of src/meeting/engine.h and MeetingEngine::end_worker (src/meeting/detail/engine_end.cpp); tests/meeting/test_engine_end.cpp ('an opted-in meeting schedules committed transcript and stops the live core at End', 'End routes opt-in meetings to the injected final intelligence provider'), tests/meeting/test_engine_intelligence.cpp ('End runs the final passes when checkpoints failed and the live core is healthy', 'End runs cleanup and the report through the app's provider and repairs the record', 'test_final_highlights_run_after_transcript_edits').)

### a24-redecode-parakeet-wording

OpenWhisper. The Audio Re-transcription step's default detail reads 'High-accuracy full session Whisper decode'.

OpenWhisper_CPP. It reads 'High-accuracy full session Parakeet decode'; every other step name, detail, headline and progress string is OpenWhisper's.

OpenWhisper_CPP keeps only the Parakeet engine, so the card names the engine that runs. (docs/port/packages/M7.md lead decision 6 ('Wording that names Whisper') and the M7.9 traps; src/meeting/finalization.cpp kSteps; pinned by tests/meeting/test_golden.cpp ('golden meeting.finalization steps') and tests/meeting/test_end_run.cpp.)

### a24-speaker-pass-desktop-only

OpenWhisper. With the speaker backend openai, consent and a key, End and the dashboard/desktop speaker re-run upload the system-audio recording ('Uploading system audio for speaker labels…') and report 'Updated N speaker label(s)'.

OpenWhisper_CPP. The gate and its skip sentences are kept, but End and rerun_speakers never upload: an open gate reports the pass unavailable and a stored openai backend reads as local. The upload runs only from the desktop Past Meetings action with fresh confirmation for one meeting.

Meeting audio leaves the computer only on an explicit, current request. (Differences a25-openai-speakers-desktop-only and a04-meeting-speaker-pass-desktop-only; docs/port/packages/M7.md change 4; src/meeting/refinalize.cpp run_speaker_pass, rerun_speakers; tests/meeting/test_refinalize.cpp ('TestRespeaker::test_refused_gate_uploads_nothing', 'TestSpeakerGate::*'), tests/meeting/test_engine_end.cpp ('test_refused_gate_never_uploads_or_claims_to'), tests/meeting/test_cloud_speakers.cpp.)

## a25-meeting-capture-asr-diarize

### a25-loopback-surround-mix

OpenWhisper. The WASAPI '[Loopback]' stream is mixed to mono by averaging every channel; the SoundCard fallback records at most two channels.

OpenWhisper_CPP. A quad, 5.1 or 7.1 loopback is mixed with ITU-R BS.775 weights: the front pair averaged, the centre and the surrounds added at -3 dB, the LFE dropped. A loopback with any other channel count is mixed from its front pair.

Averaging the six channels of a 5.1 endpoint cuts stereo content to a third, below the quiet and signal thresholds, and the front pair alone loses speech played only from the centre. (The meeting sweep of 2026-11-19 (open item meeting_capture (M7.8)); src/meeting_capture/sources.cpp loopback_mix_weights; tests/meeting_capture/test_mic_source.cpp ('stereo content on a surround loopback keeps its level', 'a surround loopback mixes its centre and surrounds in at -3 dB'))

### a25-native-wasapi-loopback

OpenWhisper. Tries a PortAudio WASAPI '[Loopback]' input matched to the default render device by name, then ScreenCaptureKit on macOS, then the SoundCard library (the Windows fallback and the Linux main path).

OpenWhisper_CPP. Opens the default render endpoint (else the first) through one native WASAPI loopback stream that takes the mix format or asks the engine to convert, converting float to int16; ScreenCaptureKit, PulseAudio and SoundCard are not built.

vcpkg's PortAudio has no loopback devices, and OpenWhisper_CPP is a Windows product with one capture path. (docs/port/packages/M7.md 'Changes from section 11 and earlier specs' item 5 (One Windows loopback path))

### a25-openai-speakers-desktop-only

OpenWhisper. After End (and from dashboard and desktop re-runs) the system-audio recording is uploaded as MP3 to gpt-4o-transcribe-diarize when the speaker backend is openai, and the returned turns relabel the transcript.

OpenWhisper_CPP. The same pass runs only from the desktop Past Meetings action with a fresh confirmation for one saved meeting; End, dashboard retries and stored grants never upload, and the audio goes as FLAC/AAC because the FFmpeg build has no MP3 encoder. When the transcription request fails inside, the pass reports OpenWhisper_CPP's own sentence 'OpenAI speaker identification could not finish.' rather than the error's text (OpenWhisper shows str(exc)), since a provider's error body can quote what was sent.

Meeting audio leaves the computer only on an explicit, current request (decision 4 of M7, re-scoped by lead amendment 19 and M7.21). (docs/port/packages/M7.md 'Changes from section 11 and earlier specs' item 4 and section M7.21; docs/port/open-items.md (The optional OpenAI speaker pass runs from the desktop Past Meetings action); tests/meeting/test_cloud_speakers.cpp; src/meeting/detail/cloud_pass_run.cpp (tests/meeting/test_cloud_speakers.cpp 'a decoder that fails inside is asked once and the pass fails'))

### a25-remote-meeting-consent

OpenWhisper. Can follow the dictation engine for meeting audio and reuse a saved host route to transcribe recorded meeting audio during recovery.

OpenWhisper_CPP. Keeps dictation and meeting routing separate. Each live remote meeting requires confirmation of the named, paired host. The worker verifies that host and model before capture and on reconnect. Automatic recovery leaves saved remote audio on this computer.

A dictation preference or a grant for live microphone audio does not authorize retransmitting stored meeting recordings. (src/remote/meeting_route.cpp, src/meeting/detail/runtime_engine.cpp, tests/remote/test_meeting_route.cpp and tests/remote_host/test_service.cpp)

### a25-stereo-fallback-for-surround-only

OpenWhisper. When the WASAPI '[Loopback]' input does not open, OpenWhisper tries SoundCard's loopback of the default speaker at up to two channels, whatever the endpoint's channel count.

OpenWhisper_CPP. The same endpoint is opened again as stereo only when it has more than two channels; a stereo or mono endpoint that does not open has no second path, and the watchdog retries it.

On an endpoint of two channels or fewer the fallback would be the same open again, which the watchdog already repeats. (The meeting sweep of 2026-11-19 (open item meeting_capture (M7.8)); src/meeting_capture/capture_session.cpp start_fallback_loopback; tests/meeting_capture/test_capture_session.cpp ('a surround loopback that does not open is opened again as stereo', 'a stereo loopback that does not open has no second path'))

## a26-meeting-agent

### a26-native-agent-loop

OpenWhisper. The packaged Pi (Node) and OpenCode SDK (Bun) agents run as sidecar processes supervised from Python: payload checks with their own install messages, hello handshake, ping, restarts within a budget, and timeouts that read "RPC 'checkpoint' timed out after Ns" or "... stalled after Ns without agent progress"; a missing key reads "... meeting sidecar agent offline".

OpenWhisper_CPP. pi, opencode and the legacy direct all build the in-process NativeAgentCore, which keeps the key resolution, pass budgets, stall clock, cancel and request-scoped authority; there is no process to supervise or install, a pass that fails fails only itself, and the messages read "Agent pass timed out after Ns", "Agent pass stalled after Ns without agent progress" and "No {provider} API key found; meeting agent offline".

The owner chose a native agent loop (amendment 1); everything that only supervised a sidecar process goes with the process. (docs/port/packages/M8.md 'The native agent loop (owner decision, amendment 1)' and changes 2 and 6; docs/port/ARCHITECTURE.md 'One agent loop (M8)'; tests/meeting_agent/test_native_core.cpp, test_native_core_budgets.cpp, test_native_core_passes.cpp)

## a27-meeting-sidecars-ts

### a27-cancel-every-pass

OpenWhisper. runner.ts's cancel names one request id: the active pass is aborted, any other id is pre-canceled so that queued checkpoint returns canceled without running; sidecar.py's cancel() sends one per in-flight request and a timed-out request is canceled alone.

OpenWhisper_CPP. AgentCore::cancel cancels every pass of the core at once through its cancel generation (the running pass and every pass waiting for it); there is no per-request cancel, and a pass that runs out of time cancels only itself through its own deadline.

With no process there is no request id on the wire; the scheduler only ever cancels everything in flight. (docs/port/packages/M8.md 'The native agent loop' table ('cancel of every in-flight pass, pre-cancel of a queued pass, cancel generation ... M8.8'); tests/meeting_agent/test_native_core_passes.cpp 'cancel ends the running pass and every pass waiting behind it')

### a27-native-provider-requests

OpenWhisper. Pi registers the provider in a temporary models.json (base URLs, authHeader, compat flags, thinking map) and OpenCode routes through its SDK's provider adapters with its own body shaping (Anthropic budget_tokens, Google thinkingConfig, store:false, 4096 output tokens by default).

OpenWhisper_CPP. The loop builds each request with the same llm request builders as text generation over the meeting's profile and model spec: reasoning at medium when the spec reasons (as OpenWhisper sent it: sidecar.py's model_metadata always carried the spec's bool reasoning, so Pi's openrouter/openai kind default never applied), max_tokens from the spec else 16,384; SDK-only URL fixups and OpenCode's body rules are not carried.

One loop replaces both harnesses and follows Pi, the default core; the provider requests are M4.5's. (docs/port/packages/M8.md changes 4 and 5, M8.8 Scope ('agent_request.{h,cpp}'), 'The native agent loop' table ('Provider registration ... SDK-only URL fixups dropped'); tests/meeting_agent/test_audit_a26.cpp, test_native_requests.cpp)

### a27-pass-usage-summed

OpenWhisper. Pi returns the last usage it saw in the pass's session events as the checkpoint's usage.

OpenWhisper_CPP. A pass's usage is the sum of every model call it made (merge_usage, with a request count).

The native loop sees each response's usage and sums the pass as the scheduler does. (docs/port/packages/M8.md 'The native agent loop' table ('Usage capture (A27-20), merge_usage (A26-45): M8.8 sums a pass with M8.2's merge_usage'); tests/meeting_agent/test_native_core.cpp)

### a27-provider-error-text

OpenWhisper. Pi's turnFailure surfaced the SDK's errorMessage for a failed provider turn (which can carry the provider's body), so the user could read e.g. "Final insights failed: 503 ..."; the OpenCode sidecar read "OpenCode model turn failed (HTTP <status>)" and never surfaced the provider's text.

OpenWhisper_CPP. A failed model turn with an HTTP status reads "{profile name} model turn failed (HTTP {status})", followed by ": {message}" when the provider's error body or stream error names one, cleaned (error.message or message, first line, keys, bearer tokens, key-shaped runs and URL queries removed, at most 200 code points); the body itself is never shown. A transport failure without a status keeps its own sentence ("Connection error.", "Model stream ended before the reply was complete.").

Lead decision: one failure rule for both former sidecars, never a provider body, with the cleaned provider message as the gain M10.22 scoped; the corrupted-signature sentence stays in the message so the polish retry works. (docs/port/ARCHITECTURE.md amendment 20 'Provider errors (M8)'; docs/port/packages/M8.md lead decision 4 and change 4; docs/port/packages/M10.md M10.22 Scope (gain row) and Contracts (StreamError::provider_message))

### a27-queued-pass-budget

OpenWhisper. sidecar.py starts a checkpoint's wall and stall clocks when it sends the RPC, so a checkpoint queued behind another in the sidecar's chain spends its budget waiting.

OpenWhisper_CPP. A pass waiting for the core's previous pass starts its wall and stall budget when it runs; the scheduler's own bound (run_agent_call's wall for the consolidation) still covers the wait.

Passes are serialized in process by a mutex (one pass at a time per core), so the budget measures the pass's own model and tool work. (docs/port/packages/M8.md 'The native agent loop' table ('checkpoint one pass at a time ... M8.8: one pass at a time per core (mutex), queued passes run in order'); tests/meeting_agent/test_native_core_passes.cpp, test_native_core_budgets.cpp)

## a28-meeting-state-persist-export

### a28-delete-keeps-host-copy

OpenWhisper. Deleting a meeting of this computer calls record_sync.record_deleted, which deletes the copy the paired host keeps.

OpenWhisper_CPP. Deleting a meeting removes only this computer's rows and spool; RecordSync::record_deleted is a no-op and a copy shared to the host stays until it is deleted there or from the host-kept list.

Record transfers are user-initiated: nothing is copied to the host automatically, so nothing is removed from it automatically either. (docs/port/audit/a17-remote-engine-services.json a17-retention and a17-backup-transfer; src/remote_records/sync.h (RecordSync: 'User-initiated dictation transfers', record_deleted empty); src/meeting/detail/runtime_past.cpp forget_deleted)

## a29-meeting-intelligence

### a29-local-meeting-judgments

OpenWhisper. Uses a configured judge to annotate citations and identify live and final highlights from meeting text.

OpenWhisper_CPP. Runs those optional judgments against an explicitly configured loopback judge. Live highlights retain the cited segment text and its timestamp. Final catch-up includes the partial last minute. Voice commands without such a judge say 'Voice commands need Fast judgments on a model that runs on this computer.' (phase unavailable), replacing OpenWhisper's NEEDS_CONSENT and its 'Add {key_phrase} in Settings to use voice commands.'; with highlights on and a hosted Fast judgments model, live_highlights_status reads 'unavailable' where OpenWhisper shows 'needs_insights'. The insight review after a meeting also checks only with such a judge: without one it reads 'Choose a local Fast Judgments model to check this meeting.' (status unavailable), replacing OpenWhisper's 'Add {key_phrase} in Settings → App → API keys or {credential_env} in your environment, then retry.'; its judge waits max(12 s, the route's budget) per question, as OpenWhisper's does.

These background judgments stay on the device. Users can independently choose an agent destination for meeting insights. (src/meeting_intel/citation_verifier.cpp, src/meeting_intel/live_highlights.cpp and their fake-judge tests; src/meeting_intel/voice_commands.h VOICE_NEEDS_LOCAL_JUDGE and src/meeting/detail/voice_control.cpp refresh_live_highlights_status (tests/meeting_intel/test_audit_a29.cpp, tests/meeting/test_fast_features.cpp 'fast-feature gate matrix', 'highlight status reaches snapshots and live clients'); src/meeting_intel/insight_review.cpp run_insight_review and build_review_judge (tests/meeting_intel/test_review_worker.cpp 'insight review without a local judge names what to choose'))

### a29-revoked-agent-reads

OpenWhisper. reinsight.py's StoreToolHost gates only the write tools on revoked agent writes (inactive / agent_writes_revoked); search_past_meetings and search_context_files keep answering a revoked or cancelled agent.

OpenWhisper_CPP. Once the host's writes_allowed() is false (the pass cancelled, agent writes revoked, or its destination no longer authorised) search_past_meetings and search_context_files answer {ok: false, disabled: true, hits: [], text: 'This meeting agent no longer has access to meeting sources.'}; the write tools refuse as OpenWhisper's do.

Past meetings and the knowledge folder are disclosed to an agent only under the current, per-meeting consent for its destination; an agent whose access was revoked or whose pass was cancelled must not keep reading other meetings. Added deliberately with its own test in the commit that connected the meetings (73bec5e); to be confirmed by the owner. (src/meeting_intel/store_tool_host.cpp (search_past_meetings, search_context_files); tests/meeting_intel/test_store_tool_host.cpp 'a revoked meeting agent cannot read the past or the knowledge folder')

### a29-wake-names-from-identity

OpenWhisper. The default wake names are 'note taker', 'notetaker', 'assistant', 'openwhisper' and 'open whisper', and the guide shows the last two as 'OpenWhisper'.

OpenWhisper_CPP. The last two defaults are the spoken forms of identity()'s product name and the guide shows the product name; names stored in imported settings, OpenWhisper's included, still match and display as 'OpenWhisper'. Matching (whole words, case-insensitive, spaces matching hyphens or nothing, no name never judged) and the guide's dedupe and order are OpenWhisper's: test_voice_command_parity.cpp TestWakeNames, tests/meeting_intel/test_intel_settings.cpp, tests/meeting_web/test_golden.cpp 'voice_command_guide matches OpenWhisper' (replayed with legacy_identity()) and tests/meeting_web/test_voice_help.cpp.

Participants call the note taker by the product's name; OpenWhisper_CPP is not OpenWhisper. (docs/port/ARCHITECTURE.md ('Wake names (M7, M8)'), docs/port/packages/M7.md decision 7 (legacy wake names) and docs/port/packages/M8.md ('Wake names from identity() (amendment 19)'))

## a30-meeting-web-server-and-dashboard-contract

### a30-dashboard-fresh-approval

OpenWhisper. Once the desktop consent was given, the host dashboard can turn on AI insights, start a custom report or re-run insights, and the request reaches the hosted model or installed agent at once.

OpenWhisper_CPP. A dashboard request that would send meeting text to a hosted model or an installed agent (turning on AI insights, a custom report, a re-run that reaches saved AI steps) waits for fresh approval on the desktop naming the meeting and the destination; loopback and device-local work, deletes, downloads and local-only retry steps do not ask.

A browser tab holding the host link must not be able to send meeting text off the device on a consent given earlier, possibly in another session. (docs/port/open-items.md ('The host dashboard supports ...', 'A desktop or dashboard retry that reaches saved AI steps needs fresh native approval ...'); pinned by tests/meeting_web/test_dashboard_api.cpp ('host browser opt-in requests local approval and opt-out cancels it', 'host report requests claim persisted work before the report worker completes') and tests/meeting/test_runtime_retry.cpp ('a restarted runtime requires a fresh saved-meeting approval for every retry', 'saved report destinations ask for fresh consent before starting their writer'); routes in src/meeting_web/detail/routes_control.cpp and routes_meeting.cpp, approval in src/meeting/detail/runtime_retry.cpp post_meeting_passes.)

### a30-dashboard-local-judgments

OpenWhisper. Insight review and semantic history search from the dashboard call the configured cloud LLM judge.

OpenWhisper_CPP. Both run only on a device-local judge: without one, review answers 409 'Turn on AI insights in an active meeting first.' and semantic search returns the keyword results with the settings' message. The review route keeps OpenWhisper's op check (400 'invalid review action'), host rule and 'Wait for final insights to finish' claim.

History review and search read every past meeting, which the port keeps on the device; the dashboard offers them only where no text leaves it. (docs/port/open-items.md ('The host dashboard supports local semantic search and local insight review ...'); src/meeting/detail/runtime_retry.cpp post_meeting_passes (review and semantic_search only with local_fast_judge); pinned by tests/meeting_web/test_dashboard_api.cpp ('the review route admits a host to review and skip a saved question', 'only a host semantic search reaches the configured meeting ranker', 'semantic search falls back to keyword results with the settings' message', 'the intelligence routes refuse in plain words without the extensions') and tests/meeting_web/test_audit_a30.cpp.)

### a30-dashboard-speakers-local

OpenWhisper. POST /api/meetings/{id}/respeakers uploads the meeting's system audio to OpenAI for speaker identification when the backend is openai, upload consent was given and a key is set.

OpenWhisper_CPP. The route keeps its guards and wording (409 active, 400 no system audio, the gate's refusal, 409 busy), but the gate reads a saved openai backend as local, so the dashboard never starts an upload; the OpenAI pass runs only from the desktop Past Meetings action with fresh confirmation.

End-of-meeting processing, dashboard retries and stored grants must not start an upload of meeting audio on their own (the a30 half of a10-openai-speakers-read-local). (docs/port/packages/M7.md 'Changes to the cut' item 4 and docs/port/open-items.md ('The optional OpenAI speaker pass runs from the desktop Past Meetings action ...'); src/meeting/rerun_options.cpp resolve_speaker_pass; pinned by tests/meeting/test_rerun_options.cpp, tests/meeting_web/test_dashboard_api.cpp (test_non_openai_backend_is_400, test_meeting_without_system_audio_is_400) and the transcript tests/fixtures/transcripts/meeting_api.json.)

## a31-meeting-qt-ui

### a31-ai-insights-consent-per-meeting

OpenWhisper. The AI insights switch starts at meeting_cloud_last_enabled; the consent dialog is asked once, meeting_cloud_consent_given then lets later meetings send text without asking, and the dialog names the chosen backup agent in the same consent.

OpenWhisper_CPP. The switch starts off for every meeting. Turning it on for a meeting (at its start or live) shows the consent naming that meeting's destination, and the grant covers that route only; nothing stored lets a later meeting send text without asking. The backup agent is named in the consent and authorized by its own dialog for its own route.

Installed agents and hosted models are destinations the user must have named to them before transcript text leaves the machine, every time. (tests/meeting/test_runtime_consent.cpp; tests/ui_meeting/test_meeting_presenter.cpp ('Meeting Mode starts and ends through the desktop presenter': a fresh live enable asks even when the grant store remembers the route); src/ui_meeting/meeting_presenter.cpp (buildTab, askIntelligenceConsent, askBackupConsent))

### a31-meeting-judge-stays-local

OpenWhisper. With Fast judgments on and a remote judge (TypeSafe, OpenRouter or a hosted text model), the consent dialog says short transcript excerpts also go to that judge, and the tab's line for a local model reads 'the model runs on this computer' instead of 'transcript text stays on this computer'.

OpenWhisper_CPP. Meetings use only a Fast judgments judge on this computer (local_fast_judge: the live topic judge, insight review and semantic search), so no judge is named in the consent and the local line keeps 'transcript text stays on this computer'.

A hosted judge never receives meeting excerpts in OpenWhisper_CPP, so naming it would be untrue. (src/ui_meeting/meeting_feature.cpp (deps.local_fast_judge returns a judge only for a local route); src/meeting/detail/live_intelligence.cpp (topic_judge uses only a local judge); docs/port/differences.md (review and semantic search only with local_fast_judge); tests/ui_meeting/test_ai_insights_copy.cpp ('test_local_fast_judgments_keep_the_privacy_claim', 'the consent names a local endpoint and says the text stays'))

### a31-meetings-folder-failure-logged

OpenWhisper. Open meetings folder creates the folder with os.makedirs; when that fails, the exception leaves the Qt slot unhandled and the folder is not opened.

OpenWhisper_CPP. A meetings folder that cannot be created is logged as 'Could not create the meetings folder <path>: <reason>' and nothing is opened; the user is shown no message.

A failure must not escape a Qt slot in C++. Whether the user should also see a message is left to the owner (docs/port/open-items.md, Waiting on the owner). (The meeting sweep of 2026-11-19 (open item ui_meeting (M7.15)); src/ui_meeting/past_meetings_panel.cpp openMeetingsFolder; tests/ui_meeting/test_sweep_meetings_folder.cpp ('a meetings folder that cannot be made is not opened'))

### a31-speaker-pass-explicit

OpenWhisper. Choosing OpenAI for speaker identification in Settings shows MeetingAudioConsentDialog ('Upload system audio for speaker identification'); after it the system-audio recording is uploaded automatically after each meeting.

OpenWhisper_CPP. The OpenAI speaker pass runs only from Past Meetings' explicit 'Identify speakers with OpenAI' action, which asks each time, names the meeting and says what is sent (the system-audio track) and what stays (microphone track, transcript, names), with Cancel as the default.

Meeting audio leaves the computer only on an action the user takes for that meeting. (tests/meeting/test_cloud_speakers.cpp; tests/ui_meeting/test_meeting_readiness.cpp ('the explicit speaker pass names what is sent to OpenAI and defaults to Cancel'); src/ui_meeting/meeting_presenter.cpp identifySavedSpeakers)

## a32-user-facing-features

### a32-no-update-source

OpenWhisper. The update check is on by default and asks GitHub (api.github.com, the Knuckles92/OpenWhisper repository) 8 s after every launch and every 4 hours while running.

OpenWhisper_CPP. The release identity names no update source ({"kind": "none"} in src/update/data/release_identity.json) until the lead records a OpenWhisper_CPP repository: automatic checks never run and a manual check says the build has no update source, without touching the network; the scheduling, the request and its fallbacks are ported and tested against a configured repository and the loopback feed (OPENWHISPER_CPP_UPDATE_FEED_URL).

OpenWhisper_CPP has no release feed of its own yet, and OpenWhisper's repository constant must not be ported (it would offer OpenWhisper releases to OpenWhisper_CPP). (docs/port/packages/M3.md, Changes to the cut 3 (No update source by default) and Open decisions 3; tests/update/test_schedule.cpp (a build without an update source never checks), tests/update/test_runtime.cpp (a build without an update source says so and never schedules))

## a34-packaging-ci-dependencies

### a34-archive

OpenWhisper. Packs its Windows release directory as a tar.xz with a SHA-256 manifest.

OpenWhisper_CPP. Packs the staged Qt application, CLI, helper and runtime libraries into a Zip64 archive with a SHA-256 manifest. The native helper verifies every payload file.

The project already ships minizip-ng for Zip64. Both packaging and verification share the native manifest code. (docs/port/packages/M10.md, Changes to the cut, items 2 and 4)
