# Actions and automation: routing, agents, launch, events, webhooks, programs, local API

Area `a19-actions-automation-services`. Source (relative to `D:\coding\whisper_local`), 9,376 lines:

| File | Lines | One line |
|---|---|---|
| `services/actions/__init__.py` | 4 | docstring only |
| `services/actions/model.py` | 464 | Saved actions: kinds, params per kind, presets, load/validate/save/delete, link expansion |
| `services/actions/routing.py` | 915 | Route by voice: code gate (names, mishearings, lead-ins), judge request/answer, `decide`, `status` |
| `services/actions/runtime.py` | 905 | `ActionRuntime`: runs an action's words per kind, worker pool, cancel, output routing, `action.ran` |
| `services/actions/agents.py` | 817 | Agent actions: handoff (app deep link or terminal) and headless "ask" for Claude Code / Codex / OpenCode |
| `services/actions/launch.py` | 611 | Open a URI (Explorer ShellExecute / `open` / `xdg-open`), handler lookup, open a terminal safely |
| `services/automation/__init__.py` | 5 | docstring only |
| `services/automation/events.py` | 379 | In-process event bus: seq numbers, replay buffer, gaps, dispatch thread |
| `services/automation/catalog.py` | 278 | Every public event type + payload builders; job sources |
| `services/automation/permissions.py` | 56 | Five API/MCP permission switches, denial copy |
| `services/automation/signing.py` | 73 | Standard Webhooks HMAC-SHA256 sign/verify, `whsec_` secrets |
| `services/automation/models.py` | 364 | Saved webhooks, programs, text filters: parse, validate, save, delete |
| `services/automation/webhooks.py` | 852 | Signed webhook delivery: per-webhook lanes, retries, records; one-off signed POSTs |
| `services/automation/programs.py` | 644 | Program hooks on events; `run_once` (no shell, process tree kill, pipe handling) |
| `services/automation/filters.py` | 577 | Text filters (saved + API leases) at before/after cleanup; budget, cancel |
| `services/automation/control.py` | 88 | `AppControl` protocol, `LiveState`, `ControlError` codes |
| `services/automation/app_control.py` | 281 | Qt implementation of `AppControl` (calls marshalled to the Qt thread, 2 s + 2 s) |
| `services/automation/api.py` | 720 | Live HTTP routes on the in-app listener: state, SSE events, actions, recording, filters, notify |
| `services/automation/meeting_events.py` | 114 | Meeting engine events -> public `meeting.*` events |
| `services/automation/service.py` | 94 | Start/stop/reload of control, webhooks, programs, MCP Events |
| `services/automation/channel_bridge.py` | 1135 | `openwhisper --channel`: hand-written stdio MCP server that is a Claude Code channel |

This area has no UI of its own. The Settings pages that drive it (Actions, Hooks, API & MCP) and their widgets are
area `a21-actions-automation-ui`. The listener that mounts `api.py`, the MCP tools and MCP Events, `installed_agents`
and `agent_process` are area `a20-agent-api-mcp-services`. The dictation pipeline that calls routing, filters and
`ActionRuntime.deliver` is `a04-runtime-services` (`services/runtime/transcription.py`) and `a05-recording-audio`
(`services/dictation_pipeline.py`). Design contract: `docs/design/automation/design.md` (956 lines, decisions 1-11,
"Contracts", "Changes during integration" rounds 1-4) is the best companion read for a porter.

## What the user gets

- **Actions** (up to 50): named things a hotkey, a spoken name or a program can send words to. Seven kinds: open a
  link with the words in it (`claude://claude.ai/new?q={text}`, ChatGPT, any URL scheme except
  file/javascript/data/vbscript/about/blob), hand off to or ask a coding agent (Claude Code, Codex, OpenCode), send
  into a Claude Code session listening on the OpenWhisper channel, POST to a webhook (signed), run a program with the
  words on stdin, copy only, or "Send to my scripts" (only publish the `action.ran` event). Each action can have a
  hotkey, up to 8 spoken names, a description, and "shaping" (raw, AI cleanup, a cleanup profile, or a transform).
- **Ten presets** in the gallery (Agent hotkey, Ask Claude, ChatGPT app, ChatGPT on the web, Quick answer, Tell my
  Claude Code session, Webhook, Run a program, Copy only, Send to my scripts).
- **Answers**: webhook replies (`{"text": ...}`), program stdout and agent "ask" answers go to Paste (only into the
  app the dictation started in, else copied), Scratchpad (default for agents), a notice near the pointer (first 140
  characters; full answer copied when longer), the clipboard, or nowhere.
- **Failures never lose words**: a failed run the user started leaves the words on the clipboard ("... - copied
  instead"). Runs a program started over the API/MCP never touch the clipboard or paste; their answers go to the
  Scratchpad.
- **Cancel**: the user's Cancel drops action runs still waiting and kills running programs and agent "asks" (with
  every child process); a status/notice says "Canceled X - too late to stop Y".
- **Route by voice**: a dedicated shortcut (`hotkeys.route_record`) records words and a fast judge (TypeSafe Jev,
  OpenRouter or the user's text model; Settings > Meeting Mode > Fast judgments) picks which action they are for and
  which part of the words is the request. Optionally plain dictations that *start* with an action's name are routed
  too. Only a confident answer (>= 0.5) routes; a timeout (1.5 s, 4 s for a text model) or failure pastes the words.
- **Agent handoff**: opens Claude Desktop (`claude://code/new?q=`), Claude Code (`claude-cli://open?q=&cwd=`), the
  ChatGPT app (`codex://new?prompt=`), or a new terminal running the CLI with the words as one argument, in a chosen
  folder or a fresh `~/OpenWhisper Agent Tasks/task-YYYY-MM-DD-HHMMSS-xxxx` folder.
- **Agent ask** ("Quick answer"): runs `claude -p` / `codex exec` / `opencode acp` headless with no tools, in an empty
  workspace, 120 s limit, and shows the answer.
- **Hooks** (Settings > Hooks): up to 20 webhooks and 20 programs that receive chosen events; up to 10 text filters
  (HTTP or program) that rewrite, keep or cancel dictated text before or after AI cleanup (3 s per stage in all).
  Each webhook/program has a "Test" button and a list of recent deliveries/runs (memory only).
- **Live local API** (when API & MCP is on, port 8767 by default, bearer token): read state, stream events as SSE
  with replay after reconnect, list/run actions, start/stop/cancel recordings, lease text filters, show a notice.
  Five permission switches gate it (live events on by default; the rest off).
- **Claude Code channel**: `openwhisper --channel` is an MCP server Claude Code starts; dictated "Tell my Claude Code
  session" words arrive in the session as `<channel source="openwhisper" origin="dictated" ...>` and Claude can call a
  `reply` tool to show a notice near the pointer.
- **Public events** for scripts (15 types + synthetic `events.gap`); payloads never carry selected text, window titles,
  file paths or audio. Dictated text, URLs, program paths/args, secrets and replies are never logged anywhere in the area.

## Feature inventory

Status: **port**, **drop** (engine-specific), **undecided** (depends on an undecided backend), **other-platform**.

| ID | Feature | Exact user-visible behavior | Source | Status |
|---|---|---|---|---|
| A19-01 | Action model and storage | Settings key `actions` (list). Max 50; name <= 80 chars, description <= 300, spoken names <= 8 x 40 chars (whitespace collapsed, case-insensitive dedupe), link template <= 2000. Unknown param keys dropped on load; type-coerced to the default's type; list params keep strings only, max 32. Ids `secrets.token_hex(6)` (12 hex). Malformed rows, duplicate ids and rows past 50 skipped. | `model.py:Action,_parse,load_actions,_coerce` | port |
| A19-02 | Action validation copy | "Give the action a name.", "Use a name with 80 characters or fewer.", "An action with that name already exists.", `"{spoken}" already names {owner}. Choose another name.`, "You can keep up to 50 actions. Delete one to add another.", "That cleanup profile no longer exists. Choose another.", "That transform no longer exists. Choose another.", "Choose how the words are prepared.", "Enter the link to open.", "Start the link with a scheme, such as https:// or claude://.", "Links that start with {scheme}: can't be used.", "Choose Claude Code, Codex or OpenCode.", "Choose the working folder with its full path.", "Choose the program with its full path.", "Use a time limit from 1 to 300 seconds.", "That shortcut is already used by {conflict}. Choose another." | `model.py:validate_action,_validate_*` | port |
| A19-03 | Presets gallery data | 10 presets with id, name, kind, params, description, icon (`wand-purple.svg`, `world-blue.svg`, `notes-blue.svg`, `server-blue.svg`, `cpu-blue.svg`, `copy-gray.svg`, `bolt-green.svg`) and note ("ChatGPT doesn't document this link, so it may stop working."). Strings name "OpenWhisper" (rebrand decision). | `model.py:PRESETS` | port |
| A19-04 | Link expansion | `{text}` replaced by the words percent-encoded with `quote(text, safe="")` (every reserved char encoded). Scheme = chars before first `:`, must start alpha, `[A-Za-z0-9+.-]`, lowercased. | `model.py:link_scheme,expand_link` | port |
| A19-05 | Public action view | API/MCP list only `id,name,kind,description,spoken_names,hotkey,shaping,enabled` (+`agent`,`mode` for agent kind); never URL, program, args, folder. Only enabled actions listed. | `model.py:Action.public_dict`, `app_control.py:list_actions` | port |
| A19-06 | Route by voice gate | Plain dictation (`route="auto"`) is judged only when it starts (after fillers and up to 4 lead-ins) with an action name/spoken name or a near-homophone; with the shortcut (`"hotkey"`) every enabled action is a candidate and a name said last also counts. Detailed word rules below (Modules). | `routing.py:gate,_leading_match,_trailing_match,_name_variants,_match_len,_alike,_sound` | port |
| A19-07 | Payload span proposal | Candidate payloads: words after the name, the same without link words ("to", "can you"...), each without a polite end ("please", "thanks", "thank you"), words before a trailing name, then the whole text. First letter raised when lower. | `routing.py:_spans,_without_polite_end,_sentence_start` | port |
| A19-08 | Judge request | One request, two `choice` questions (`route`, `payload`); exact instruction strings (benchmarked; "rewording needs a live re-run"). Per-candidate criteria `{name, spoken_names?, purpose?, does}`; shrinks description levels (300/8, 160/4, 80/2, 0/0) until JSON <= 20,000 chars. Dictation clipped to 1500 head + 400 tail, spans to 200 + 80 (`" … "` joiner). | `routing.py:plan,_INSTRUCTIONS,_NONE_CRITERIA,PAYLOAD_*,_KIND_PHRASES,_criterion,_clip` | port (judge backend: a12) |
| A19-09 | Judge answer reading | `route.choice` must be `none` or a candidate; confidence from `route.confidence`; payload by summed probabilities per span (`none` counts for the whole text) else `choice`. Routes only if action chosen and confidence >= 0.5. | `routing.py:RoutePlan.read,_payload,ROUTE_CONFIDENCE` | port |
| A19-10 | Judge timeout and cancel | 1.5 s budget (4.0 s when Fast judgments use the user's text model profile); judge asked on a helper thread; cancel polled every 20 ms; failure `timeout` / `unreachable` reported so the paste can say so. Publishes `route.decided` unless canceled. | `routing.py:decide,_ask,route_budget` | port |
| A19-11 | Route by voice status line | Messages: "Route by voice is off. Turn it on above.", "Route by voice needs Fast judgments. Turn them on in Settings → Meeting Mode → Fast judgments.", "Route by voice needs {key_phrase}. Add one in ...", "Route by voice is on, but there are no actions yet. Add one above.", "... but every action is turned off. Turn one on above.", "... but has nothing to listen to yet. Set a Route by voice shortcut below, or turn on Also listen to normal dictations.", ready: "Route by voice is on. {Everything you record with the Route by voice shortcut[, and dictations that start with an action's name,] are/is} {checked on this computer by X / sent to X} to choose the action." `fix` = `fast_judgments`/`actions`/`trigger`. Also the shortcut refuses to start with this message. | `routing.py:status,_status`; `application_controller.py:1044` | port |
| A19-12 | Action runtime dispatch | `deliver` on Qt thread: missing -> "That action no longer exists", disabled -> "That action is turned off", program words to agent/channel without permission -> "Words from a program need “Send words to my coding agents”". Clipboard/scripts/channel finish on Qt thread; link/webhook/program/agent on a 4-worker pool, max 8 pending ("Too many actions are still running"), after close "OpenWhisper is closing". Status "{name} · working…" while running. | `runtime.py:ActionRuntime.deliver,_start,_submit` | port |
| A19-13 | Output routing | Kinds with answers: webhook, program, agent ASK. Paste only when `paste_target(job)` is SAME (else copy: "Copied — the app changed" / "Copied — couldn't confirm the app" / "Copied — couldn't paste it"); Scratchpad "{name} answered in the Scratchpad"; Notice shows `_short(answer,140)` (+" — full answer copied" when longer and user's run); Clipboard "Copied {name}'s answer"; empty answer "Sent to {name} — nothing came back". Program/API runs redirect paste/clipboard to Scratchpad. | `runtime.py:_route,_paste,_output_of,_short` | port |
| A19-14 | Failure fallback copy | User's failed run (not clipboard kind): words copied, message "{name}: {reason} — copied instead". Shaping notice appended " — {notice}". `_say` sets status and shows notice (done=ok). | `runtime.py:_finish,_say` | port |
| A19-15 | Cancel of action runs | Waiting runs never start (`action.ran` "Canceled"); programs and agent asks are killed; links/webhooks/handoffs "can't be stopped" and finish with answer dropped. One summary when all stopped runs answered: "Canceled {name or N actions} — too late to stop {...}". `tests_only` for Settings' Try it (quiet). Returns count reached. "Nothing to cancel" text lives in the controller. | `runtime.py:cancel,_tell,_finish_canceled,_stoppable` | port |
| A19-16 | Try it (Settings) | `test(action, sample, done)`: runs for real with `source="ui"`, no history; channel kind only counts listeners ("No Claude Code session is listening" / "1 Claude Code session is listening" / "N Claude Code sessions are listening"); message capitalized, default "Sent to {name}". | `runtime.py:ActionRuntime.test,_listening` | port |
| A19-17 | Open link action | Refuses `claude:` links whose words exceed 14,000 chars ("That's too long for the Claude app (14,000 characters at most)"). | `runtime.py:_open_link` | port |
| A19-18 | Webhook action | Payload `{"v":1,"action":{"id","name"},"job_id","text","raw_text"(null when same),"language","app"}`; signed with per-action secret `OPENWHISPER_ACTION_SECRET_{id}` in credential store; 5 s timeout; reply JSON `"text"` is the answer. Errors: "couldn't keep its signing secret (...)", "the address answered {status}", "the address didn't answer", or the exchange's error copy. | `runtime.py:_webhook,_webhook_payload,ensure_action_secret,delete_action_secret,reply_text` | port |
| A19-19 | Program action | argv `[program, *args]`, words UTF-8 on stdin, env = user's minus app secrets + `OPENWHISPER_ACTION_ID`, `OPENWHISPER_ACTION_NAME`, `OPENWHISPER_JOB_ID`; timeout 1-300 s (default 30); non-zero exit "the program stopped with code N"; stdout decoded UTF-8 (replace), stripped. | `runtime.py:_start_program,_program` | port |
| A19-20 | Agent action with selection context | With `include_selection`, appends `\n\n<context source="OpenWhisper">\n<app>NAME</app>\n<selected_text>\n...\n</selected_text>\n</context>`; only when selection known, not blocked, non-empty; `<` of `selected_text/context/app` tags inside values escaped to `&lt;`. Waits up to `config.CONTEXT_CAPTURE_DEADLINE_S` for the capture. | `runtime.py:prompt_with_context,_inside_frame,_agent` | port |
| A19-21 | Program-words rule | Agent and channel kinds take words a program supplied/changed only with permission `api_agent_prompts`. Applied in deliver, API run/start, and filter leases. | `runtime.py:program_words_allowed` | port |
| A19-22 | "Send to my scripts" | Succeeds ("Sent to your scripts") only when an SSE stream is open, or an enabled webhook/program hook takes `action.ran`, or an MCP Events subscription exists; else "Nothing is listening for your scripts". Only this kind puts `text` in `action.ran`. | `runtime.py:scripts_listening,_publish_ran` | port |
| A19-23 | Channel action | Needs a `claude-channel` SSE subscriber ("No Claude Code session is listening"); publishes `channel.message`. | `runtime.py:_send_to_channel` | port |
| A19-24 | Agent handoff | Surface auto/app/terminal; app links tried best first, skipping schemes without handler (`auto` also skips unknown); `NoAnswer` from Windows stops (no double start). Messages "Opened {name}", "Opened {name} in a terminal", "{name} isn't installed.", "There's nothing to send to {name}.", "This action's working folder doesn't exist anymore. Choose another in Settings.", "Couldn't create a new working folder. Check that ~/OpenWhisper Agent Tasks is writable.", no-app copy per agent. | `agents.py:handoff,_handoff_session,app_links,_no_app_message,_new_handoff_folder` | port |
| A19-25 | Agent terminal argv | Claude: `[cmd, "--", prompt]` with a trailing space appended when the text has no whitespace (so a lone word is not a subcommand); Codex `[cmd, "--", text]`; OpenCode `[cmd, "--prompt=TEXT"]`. Windows: `.cmd` shim resolved to `node.exe script` or a packaged `.exe`; installed `.exe` preferred over a newer shim. | `agents.py:terminal_argv,terminal_command,npm_shim_command,_package_exe` | port (Windows parts other-platform-specific) |
| A19-26 | Agent ask (Claude Code, Codex) | System prompt "Answer briefly in plain text. You can't use tools or files."; Claude `-p --output-format json --system-prompt ... --tools "" --mcp-config <empty> --strict-mcp-config --settings <disableAllHooks>` + optional `--no-session-persistence --disable-slash-commands` (if in `--help`), `--model`; Codex `exec --json [--ephemeral] --skip-git-repo-check --ignore-user-config -s read-only -C <workspace> -c approval_policy=never -c web_search=disabled -c project_doc_max_bytes=0 -c model_reasoning_effort=low -c model_instructions_file=<path> --disable <each known tool feature> [-m model] -`. Prompt on stdin (binary). 120 s, stdout cap 4 MiB, stderr last 20 lines x 400 chars. Too-old refusal copy. | `agents.py:ask,_ask_headless,_claude_ask_command,_codex_ask_command,_codex_tool_features,_run_headless` | port |
| A19-27 | Agent ask (OpenCode over ACP) | `opencode acp` with env `OPENCODE_CONFIG_CONTENT` overlay (build/plan personas: ask prompt, deny-all permissions), `OPENCODE_DISABLE_PROJECT_CONFIG/AUTOUPDATE/FILEWATCHER=1`; initialize, `session/new` (cwd workspace, no MCP), optional `session/set_config_option` model, `session/prompt`; all permission requests rejected; session deleted after if supported. | `agents.py:_ask_opencode,_AcpAnswer,_refuse_acp_request,_opencode_model,_close_opencode` | port |
| A19-28 | Agent answer parsing and error copy | Claude: last `{"type":"result"}` in output (object or array); `is_error`/subtype -> failure. Codex: JSONL events, last `item.completed` agent_message; `turn.failed`/`error` messages, inner JSON error message extracted. Signed-out keywords -> "{name} is signed out. {sign_in_hint}". "{name} couldn't answer: {detail<=200}", "{name} stopped before answering: {last stderr line}", "{name} didn't answer in time.", "Stopped asking {name}." (stopped), "{name} didn't answer." | `agents.py:_claude_result,_claude_answer,_codex_answer,_codex_message,_failure,_stopped,_answered` | port |
| A19-29 | Open URI | Refuses empty / CR / LF / NUL ("That link can't be opened."), > 30,000 chars ("That's too long to send in a link. Try fewer words."), non-http(s) scheme with no handler ("No app on this computer opens {scheme}: links. Install the Claude app." etc.). Windows: ShellExecute inside Explorer via `ShellWindows.FindWindowSW` desktop dispatch (30 s wait, serialized; "Windows didn't answer. Try again."), fallback `os.startfile`; macOS `open`; Linux `xdg-open` (2 s: still running = opened). | `launch.py:open_uri,_windows_open,_ask_explorer,_explorer_open,_desktop_application,_allow_foreground,_open_with` | port |
| A19-30 | URL handler lookup | Cached 60 s; Windows `AssocQueryStringW` (DelegateExecute, then command, then executable missing = CO_E_APPNOTFOUND) under COM STA; macOS `URLForApplicationToOpenURL`; Linux `xdg-mime query default x-scheme-handler/S`. None = can't tell. | `launch.py:handler_available,_windows_handler,_macos_handler,_linux_handler,forget_handlers` | port |
| A19-31 | Open in terminal | Windows: only `.exe`, new console (`CREATE_NEW_CONSOLE`), refuses `.cmd/.bat/.ps1` with explanation, command line <= 32,000. macOS: self-deleting `.command` script (`#!/bin/zsh -l`, all args `shlex.quote`d) via `open -a Terminal`. Linux: self-deleting `/bin/sh` script, tries `$TERMINAL`, then x-terminal-emulator, xdg-terminal-exec, gnome-terminal, konsole, alacritty, ghostty, foot, xterm, kitty with per-terminal arg style; a terminal that exits non-zero within 0.5 s falls through. | `launch.py:open_in_terminal,_windows_terminal,_macos_terminal,_linux_terminal,_one_shot_script,_terminal_commands` | port |
| A19-32 | Event bus | Thread-safe publish from any thread, one dispatch thread, subscriber callbacks must only enqueue (slow > 1 s warned once). Buffer 500 events; `replay=False` for previews. Seq base = ms since epoch at app start (so old positions read as "restarted"). Gap reasons `evicted`/`restarted` computed per followed type. `clear()` on history/meeting delete. Rejects non-UTF-8 data. | `events.py:EventBus,bus,clear,publish` | port |
| A19-33 | Event catalog | 15 public types + `events.gap`; descriptions; payload builders; sources `hotkey, ui, api, mcp, dbus`; user sources `hotkey, ui, dbus`. | `catalog.py` | port |
| A19-34 | Permissions | `api_live_events` (default true), `api_run_actions`, `api_control_recording`, `api_text_filters`, `api_agent_prompts` (false). Public names `live_events, run_actions, control_recording, text_filters, agent_prompts`. Denial copy "Allow ... in OpenWhisper Settings > API & MCP." Re-read on every request. | `permissions.py` | port |
| A19-35 | Standard Webhooks signing | `whsec_` + base64(24 random bytes); `v1,` + base64(HMAC-SHA256(key, "id.ts.body")); verify: any `v1` sig, 300 s tolerance, constant-time compare. | `signing.py` | port |
| A19-36 | Saved hooks model | Webhooks (max 20), programs (max 20, timeout 1-300 s default 30, args <= 32 x 1000), filters (max 10, http/program, before/after cleanup, timeout 100-5000 ms default 1500). Validation copy ("Give the {noun} a name.", "A {noun} with that name already exists.", "Choose at least one event.", "Enter the address to send to.", "Use an address that starts with http:// or https://.", "Addresses can't contain spaces.", "You can keep up to {limit} {noun}s. Delete one to add another." ...). | `models.py` | port |
| A19-37 | Private URL detection | loopback/private/link-local IPs, `localhost`, `*.localhost/.local/.lan/.home.arpa` -> no proxy, no encryption warning. | `models.py:url_is_private` | port |
| A19-38 | Webhook delivery | Per webhook FIFO lane with own thread (ends after 30 s idle), queue 500 (oldest dropped, recorded "Dropped: too many deliveries were waiting."), 5 s timeout, retries after 1/5/25 s for network errors, timeouts, 408, 429, 5xx, `Retry-After` honored up to 60 s; 3xx = failure "HTTP {s}: redirects aren't followed"; private addresses bypass proxies; `User-Agent: OpenWhisper/{version}`; secret `OPENWHISPER_WEBHOOK_SECRET_{id}` created on first delivery (never re-created for a deleted webhook); last 50 records per webhook in memory. | `webhooks.py:_Engine,_Lane,_deliver,_exchange,_retry_after,ensure_secret,_delivery_secret,rotate_secret,delete_secret,deliveries` | port |
| A19-39 | Webhook test | `webhook.test` event `{"message":"This is a test delivery from OpenWhisper.","webhook_id"}`, once, no retry, even when disabled; "Save the webhook first." | `webhooks.py:send_test,_send_test` | port |
| A19-40 | One-off signed POST | `post_signed` / `post_signed_reply` (reply cap 1 MiB, "The reply was too large"), extra headers never override signature/content type; used by webhook actions and MCP Events. | `webhooks.py:post_signed,post_signed_reply,_post_signed` | port |
| A19-41 | Program hooks | Event wire JSON + `\n` on stdin, env `OPENWHISPER_EVENT_TYPE/ID/SEQ`; one run at a time per program, queue 20 (oldest dropped, "Dropped: too many events were waiting."); last 20 run records; "Exited with code N"; tests jump the queue; "Save the program first."; test event `program.test` `{"message":"This is a test run from OpenWhisper.","program_id"}`. | `programs.py:_Engine,_enqueue,_drain,_run_event,run_test,runs` | port |
| A19-42 | `run_once` | No shell, no window, own process group / Windows job; stdin written by a pipe thread, stdout <= 1 MiB kept, stderr discarded; cwd = home; timeout kills the whole tree ("Timed out after N s"); cancel polled 50 ms ("Canceled"); a program that exits on its own leaves children running (job limit cleared) and pipes are released after 1 s grace. Start errors: "The program wasn't found.", "The program isn't allowed to run (permission denied).", "Windows can't run that file directly. Choose a program (.exe)." (winerror 193). | `programs.py:run_once,_Pipe,_Input,_Output,_cancel_io,_wait,_disown,_start_error,child_env` | port |
| A19-43 | Text filters | Request `{"v":1,"stage","job_id","mode","action_id","text","language","app"}`; reply `{"text"}` replaces, `{}`/empty/204 keeps, `{"cancel":true}` or empty text = deliver nothing; anything else keeps text and publishes `filter.failed` (reason <= 200, never text). Saved filters first in order, then leases. Only `dictation`/`action` jobs, never empty text. Stage budget 3 s, filter not started with < 50 ms left ("Skipped: this stage's 3 s for filters ran out"). Cancel returns original text, no `filter.failed`. Reply cap 1 MiB. `program_changed` when a lease changed the words. | `filters.py:apply,StageRun,_interpret,_steps,_run_steps,_await` | port |
| A19-44 | Filter leases (API) | URL must be http(s) on 127.0.0.1/::1/localhost; stage valid; timeout 100-5000 ms (default 1500); modes subset of dictation/action; TTL 60 s (monotonic), renew, delete; max 10 ("10 filters are already registered. Remove one first."); id `flt_` + 16 hex; dropped when the listener stops. Leases skip action jobs whose action refuses program words. | `filters.py:register_lease,renew_lease,drop_lease,drop_all_leases,leases,_leases_apply` | port |
| A19-45 | Live API routes | `GET /v1/live/state`, `GET /v1/events` (SSE), `GET /v1/actions`, `POST /v1/actions/{id}/run`, `POST /v1/recording/start|stop|cancel`, `POST /v1/filters`, `POST /v1/filters/{id}/renew`, `DELETE /v1/filters/{id}`, `POST /v1/notify`. Details in Data and formats. | `api.py:create_live_router,_register_*` | port (listener: a20) |
| A19-46 | SSE event stream | `retry: 2000`, replay from `Last-Event-ID` / `?since=` / now, synthetic `events.gap` (no id line, seq 0), live events strictly ascending, `: ping` every 15 s, permission rechecked every 2 s, MCP-off checked every 0.5 s, queue 1000 then `events.gap` reason `slow` and end. ASCII JSON frames. `X-Accel-Buffering: no`. | `api.py:EventStream,sse_frame,gap_event,parse_types,parse_last_event_id` | port |
| A19-47 | AppControl on Qt thread | State read off-thread; start/stop/cancel/notify/run marshalled to Qt thread, 2 s to be taken (else "OpenWhisper didn't respond in time. Try again.", call never runs), 2 s more grace once running ("OpenWhisper is still working on that. Check its state in a moment."). Start refusals: "OpenWhisper is already recording.", "OpenWhisper is busy with another recording.", "Meeting Mode is active; end the meeting first.", "No enabled action has that id.", last status text of a refused start. Stop "Nothing is recording."; cancel "Nothing is recording or transcribing."; notify one line `"{title}: {text}"`, "There's nothing to show.". | `app_control.py`, `control.py` | port |
| A19-48 | Meeting events | `meeting.started` on first status `active`/`paused` per meeting id (last 32 remembered), `meeting.ended` with status (default "ended"), `meeting.segments` committed non-empty rows (skips payloads with `removed_ids`), `meeting.voice_command`. Only named fields leave. | `meeting_events.py:forward` | port (meeting area dependent) |
| A19-49 | Automation service lifecycle | After window shows: install AppControl, start webhooks, programs, MCP Events delivery; `reload(kind)` for `webhooks`/`programs`/`mcp_events`; stop at quit, clear AppControl. | `service.py` | port |
| A19-50 | Claude Code channel server | `--channel` stdio MCP server (JSON-RPC lines, ASCII JSON), protocol versions 2025-11-25/2025-06-18/2025-03-26/2024-11-05, `server/discover` -> method not found, capability `experimental.claude/channel`, `reply` tool, follows `/v1/events?types=channel.message` with backoff 1->30 s, 45 s read timeout, Last-Event-ID resume, gap notice only for `evicted`, `OPENWHISPER_URL`/`OPENWHISPER_TOKEN` overrides, exits for a Windows update. | `channel_bridge.py` | port (decide binary name) |
| A19-51 | Channel `reply` tool | text 1-500 chars (stripped), title <= 80 (whitespace collapsed); results "Shown to the user near their pointer." or refusal copy per HTTP status; 401 re-reads token once. | `channel_bridge.py:_call_tool,_reply_arguments,AppLink.notify,_reply_refusal` | port |
| A19-52 | Child environment hygiene | Every child (agents, terminals, link handlers on mac/Linux, programs, filters) gets the user's environment minus keys OpenWhisper loaded from `.env` and its internal tokens. | `launch.py:_child_env`, `programs.py:child_env` -> `installed_agents.agent_child_env` | port (a20/a01) |
| A19-53 | No secrets in logs | httpx request log lines dropped while the hooks engine requests (contextvar filter); exceptions logged by type and file:line only. | `webhooks.py:_QuietRequests`, `runtime.py:_described`, `api.py:_log_failure` | port (as a rule) |

### Added by check

Additional copy and rules a port must keep (verified against source). Checked items not listed here were already accounted for.

| ID | Feature | Exact behavior | Source | Status |
|---|---|---|---|---|
| A19-54 | More action validation copy | "Choose what the action does." (unknown kind), "Use a description of 300 characters or fewer.", "Use a link of 2000 characters or fewer.", "Choose whether to open a session or ask for an answer.", "Choose where the session opens.", "Choose where the answer goes." (agent), "Choose where the reply goes." (webhook), "Choose the program to run." (empty program), "Choose where the program's output goes." Webhook action URL goes through `validate_url` (A19-55). | `model.py:_validate_*,validate_action` | port |
| A19-55 | More hook validation copy | `validate_url`: empty or > 2000 chars "Enter the address to send to.", unparsable "That address isn't valid.". Program/filter: "Choose the program to run.", "Use at most 32 arguments of 1000 characters.", filter "Choose whether the filter is a web address or a program.", "Choose when the filter runs.", "Use a time limit from 100 to 5000 ms.", save with no id "This {noun} has no id.". Names compared case-folded. | `models.py:validate_url,_check_*,validate_*,_require_id` | port |
| A19-56 | Clipboard and fallback result copy | Clipboard kind: a user's run copies the words -> "Copied" / "Couldn't copy the words"; a program/API run puts them in the Scratchpad -> "Put in the Scratchpad". Unknown kind "OpenWhisper can't run this kind of action". Answer outcome messages (recorded in `action.ran`, which can differ from the notice shown): "Pasted {name}'s answer", "Showed {name}'s answer" (+ " — full answer copied"), "Copied {name}'s answer", "Couldn't copy {name}'s answer", "Couldn't paste or copy {name}'s answer". `action.ran.output` is where the answer actually went (`none` after a failed copy, `clipboard` after a paste fallback). | `runtime.py:_start,_start_clipboard,_route,_paste` | port |
| A19-57 | Terminal launch errors and styles | `open_in_terminal` copy: "There's no program to open.", "Those words can't be passed to a program.", "The working folder doesn't exist." (cwd default home). Windows: "{name} is a script, and Windows would read your words as commands on the way in. Install the program's own .exe build, or open it through its app instead.", "{name} isn't a program OpenWhisper can open in a terminal.", "That's too long to open in a terminal. Try a shorter request.", "{name} couldn't be started in a terminal.". macOS "Terminal couldn't be opened.". Linux "No terminal app could be started.", "No terminal app was found. Set $TERMINAL to the one you use.". Linux argument styles: `gnome-terminal`, `ptyxis`, `kgx`, `foot` use `--`; `wezterm` `start --`; `kitty`, `xdg-terminal-exec` none; `xfce4-terminal`, `mate-terminal`, `terminator` `-x`; anything else `-e`. `$TERMINAL` is shlex-split, its first word resolved on PATH, extra words kept, tried first. The Linux script runs as `/bin/sh <script>`. One-shot script body: `#!<shell>`, `rm -f -- "$0"`, `cd -- <cwd> || exit 1`, `exec <quoted args>`; temp name `openwhisper-*.sh` / `.command` (macOS shell `/bin/zsh -l`, `open -a Terminal`, 10 s limit). Detached terminals are reaped by a thread. | `launch.py:open_in_terminal,_windows_terminal,_one_shot_script,_macos_terminal,_terminal_commands,_linux_terminal,_TERMINAL_STYLES` | port |
| A19-58 | Scheme app hints and agent no-app copy | "No app on this computer opens {scheme}: links." plus " Install the Claude app." (`claude`), " Install Claude Code." (`claude-cli`), " Install the ChatGPT app." (`codex`). Agent no-app: Claude "That's too long for the Claude app. Choose Terminal for long requests." (> 14,000) else "Claude Code can't open in an app here. Install the Claude app, or choose Terminal."; Codex "Codex can't open in an app here. Install the ChatGPT app, or choose Terminal."; OpenCode "OpenCode has no app link. Choose Terminal instead.". Signed-out keywords: "not logged in", "login", "log in", "sign in", "signed out", "authenticat", "unauthorized", "401", "invalid api key". | `launch.py:_SCHEME_APPS,open_uri`, `agents.py:_no_app_message,_SIGNED_OUT_WORDS` | port |
| A19-59 | Run-once and hook error copy | `run_once`: "Choose the program to run.", "The time limit isn't a number.", "The working folder doesn't exist.", "The program couldn't start (its arguments aren't valid).", "The program couldn't start ({ExcType} {code}).", timeout floor 0.05 s; `cwd` parameter (empty = home). Engines: "The program couldn't run ({Type}).", "The delivery failed ({Type}).", "The test couldn't run ({Type}).", "The message isn't valid JSON." (signed POST of a non-JSON body), `SECRET_MISSING` "The signing secret is missing.", "This webhook has no id.". Filter failure reasons (`filter.failed.reason`): "The reply wasn't JSON", "The reply wasn't a JSON object", "The request failed", "The filter couldn't run", "The filter couldn't run ({Type})", "Exited with code N". | `programs.py:run_once,_start_error`, `webhooks.py`, `filters.py:_interpret,_run_*` | port |
| A19-60 | Filter lease validation copy | URL passes `validate_url` and host is 127.0.0.1, ::1 or localhost, else "A filter added over the API must listen on this computer (127.0.0.1, ::1 or localhost)."; "Use the stage before_cleanup or after_cleanup."; "Use a timeout_ms from 100 to 5000." (a float with an integer value is accepted, bools refused); "Give modes as a list: dictation, action or both.". Over HTTP only `url`, `stage`, `timeout_ms` are accepted, so `modes` cannot be set (always both); a non-integer `timeout_ms` -> 422 "timeout_ms must be a whole number of milliseconds."; lease JSON `expires_at` is Unix seconds (wall clock) while expiry itself runs on a monotonic clock. The client for loopback is warmed in the background on register. | `filters.py:register_lease,_lease_*`, `api.py:_filter_options,_lease_dict` | port |
| A19-61 | Listener labels and operation ids | Header `X-OpenWhisper-Client: claude-channel` (trimmed, exact) makes an event stream count as a `claude-channel` listener (`listeners.claude_channel`, what "Tell my Claude Code session" needs); any other or missing client counts as `sse`. OpenAPI operation ids are public contract (a20 may derive MCP tool names from them): `get_live_state`, `stream_events`, `list_actions`, `run_action`, `start_recording`, `stop_recording`, `cancel_recording`, `create_filter_lease`, `renew_filter_lease`, `delete_filter_lease`, `notify_user`. The `Last-Event-ID` and client headers are capped at 64 chars at the transport level. | `api.py:_stream_label,_register_*` | port |
| A19-62 | Event descriptions and permission denial copy | Event descriptions are user-visible (Hooks event picker, MCP events list): recording.started "A recording started (dictation, Command Mode or an action).", recording.stopped "A recording stopped and is being transcribed.", recording.canceled "A recording or transcription was canceled.", transcription.preview "Live preview text while recording, at most four a second.", transcription.completed "Finished text of a dictation, command, action or file.", transcription.failed "A transcription or rewrite failed.", history.saved "A finished transcription was saved to history.", action.ran "An action ran, with whether it worked.", channel.message "Words sent to Claude Code sessions listening on the channel.", route.decided "Route by voice chose an action, or fell back to pasting.", filter.failed "A text filter timed out or answered badly; the text was kept.", meeting.started "A meeting started.", meeting.ended "A meeting ended.", meeting.segments "New finished lines of a meeting transcript.", meeting.voice_command "Someone gave the meeting note-taker a spoken command.", events.gap "This stream missed events; fetch history to catch up.". Denials (exact): live events "Allow live events in OpenWhisper Settings > API & MCP.", run actions "Allow running actions ...", control recording "Allow starting recordings ...", text filters "Allow changing dictated text ...", agent prompts "Allow sending words to your coding agents ..." (each ends "in OpenWhisper Settings > API & MCP."). `transcription.failed` error kinds: `canceled`, `no_speech`, `refused`, `failed`. | `catalog.py:EVENT_TYPES,FAILURES`, `permissions.py:_DENIED` | port |
| A19-63 | Channel CLI behavior | `--channel` argv: `-h`/`--help` prints the usage text to stderr, exit 0; any other argument prints "Unexpected arguments. Run --channel --help for usage." and exits 2; missing stdin/stdout pipes logs and exits 2; an unhandled error exits 1. Log level from env `OPENWHISPER_LOG_LEVEL` (default INFO), logs only to stderr with prefix "openwhisper channel: ". Usage text names `OPENWHISPER_URL` and `OPENWHISPER_TOKEN`. Fd 1 is re-pointed at stderr after the protocol stream is duplicated, so stray prints never reach the protocol. On initialize it logs "{client} {version} connected (protocol {v})." (clipped 60/30); an unsupported requested protocol version is answered with the newest supported one. | `channel_bridge.py:main,_USAGE,_configure_logging,protocol_streams,_initialize` | port |
| A19-64 | Channel copy | `GAP_NOTICE`: "Some messages sent from OpenWhisper while this session was disconnected may have been missed. If the user seems to be waiting on something, ask them to send it again." (delivered as a channel notification with `meta.origin="openwhisper"`; origins `dictated`, `program`, `openwhisper`). Unreachable: "API & MCP is off in OpenWhisper Settings." (only when no `OPENWHISPER_URL` and saved `mcp_enabled` is false) else "OpenWhisper isn't running, or API & MCP is off in its Settings.". Reply refusals: 401 "OpenWhisper rejected the channel's access token, so the reply wasn't shown.", 403 "OpenWhisper didn't show the reply: allow running actions in OpenWhisper Settings > API & MCP.", 404 "This version of OpenWhisper can't show replies. Update OpenWhisper.", other "OpenWhisper couldn't show the reply ({server message or HTTP N}).", unexpected "OpenWhisper couldn't show the reply.". Stream refusals (state, message): 200 `unsupported` "OpenWhisper didn't answer with an event stream. Update OpenWhisper.", 401 `unauthorized` "OpenWhisper rejected the access token; retrying.", 403 `forbidden` "OpenWhisper refused the event stream. {server message, else Allow live events in OpenWhisper Settings > API & MCP.}", 404 `unsupported` "This version of OpenWhisper has no live event stream. Update OpenWhisper.", other `http_N` "OpenWhisper answered the event stream with HTTP N; retrying.". Tool errors (isError true): `Pass the reply as {"text": "..."}.`, "text is required: the words to show the user.", "Replies are limited to 500 characters; shorten it.", "title must be text.", "Titles are limited to 80 characters."; unknown tool name -> JSON-RPC -32602 "Unknown tool: {name}". `channel.message` meta: `origin`, `job_id`, `action` (the action id), `app` (the app's name, not the object). | `channel_bridge.py:GAP_NOTICE,unreachable_message,_reply_refusal,_stream_refusal,_reply_arguments,_call_tool,channel_params` | port |
| A19-65 | Meeting segment item shape | Each `meeting.segments` item: `segment_id` (row `id` or `segment_id`), `speaker` (row `speaker` if text, else `speaker_participant_id`), `text`, `start_s`, `end_s` (numeric or null); rows with blank text dropped; `meeting.started` title trimmed (empty when absent); events without a meeting id are ignored. | `meeting_events.py:_segment,_forward` | port (meeting area dependent) |

## Modules

### `services/actions/model.py`
- Responsibility: the saved-action data model; pure apart from imports of `cleanup_profiles.normalize_hotkey`,
  `cleanup_profiles.find_cleanup_profile`, `text_transforms.find_transform`, `hotkey_conflicts.hotkey_conflict`,
  `automation.models.validate_url`.
- Public surface: constants `MAX_ACTIONS=50`, `NAME_MAX_CHARS=80`, `DESCRIPTION_MAX_CHARS=300`, `MAX_SPOKEN_NAMES=8`,
  `SPOKEN_NAME_MAX_CHARS=40`, `TEMPLATE_MAX_CHARS=2000`, `_PROGRAM_TIMEOUT_MAX_S=300`, `AGENT_ACTION_ID="agent"`;
  enums `ActionKind`, `Output` (none/paste/scratchpad/notice/clipboard), `AgentMode` (handoff/ask), `AgentSurface`
  (auto/app/terminal); `AGENTS=("claude_code","codex","opencode_cli")`, `AGENT_NAMES`; `SHAPING_RAW="raw"`,
  `SHAPING_CLEANUP="cleanup"`, prefixes `profile:` and `transform:`; `REFUSED_SCHEMES`; `PARAMS` (per kind
  defaults, see Data); `Action` (frozen; `param`, `profile_id`, `transform_id`, `to_dict`, `public_dict`); `Preset`,
  `PRESETS`; `new_id`, `action_secret_name`, `load_actions`, `find_action`, `link_scheme`, `expand_link`,
  `action_hotkey_conflict`, `validate_action`, `save_action`, `delete_action` (the last two are mutate_settings mutators).
- Edge cases: `_coerce` treats bool before int (`model.py:205-216`); a hotkey that fails `normalize_hotkey` reads as
  none (`:225-233`); `shaping` empty -> cleanup; `enabled` non-bool -> True; folder checked absolute only when not
  `new_folder` (`:364`); spoken names clash with any other action's name or spoken names, case-folded (`:420-424`);
  the 50 limit applies only to a new id (`:425`); `save_action` re-normalizes then replaces in place, keeping order
  (`:440-457`). `timeout_s` validation uses `int(...)` (`:386`).

### `services/actions/routing.py`
- Responsibility: decide whether words are for a saved action and which part is the request.
- Public: `ROUTE_TIMEOUT_S=1.5`, `TEXT_MODEL_ROUTE_TIMEOUT_S=4.0`, `ROUTE_CONFIDENCE=0.5`, `ROUTE_HOTKEY="hotkey"`,
  `ROUTE_AUTO="auto"`, `NONE="none"`, `GateMatch`, `gate`, `RoutePlan` (`read`), `plan`, `Judgment`,
  `RouteDecision(action_id,payload,confidence,failure)`, `RoutingStatus(enabled,ready,message,fix)`, `FIX_*`,
  `FAILURE_TIMEOUT`, `FAILURE_UNREACHABLE`, `decide`, `route_budget`, `status`, `ROUTE_HOTKEY_NAME="route_record"`,
  `PAYLOAD_QUESTION`, `PAYLOAD_ADDRESSING`, `PAYLOAD_NONE`.
- Word model (`:72-114`): word regex `[^\W_]+(?:['’\-‐][^\W_]+)*` (Unicode letters/digits); `norm` = casefold with
  `' ’ - ‐` removed; `pause` = end of text or punctuation `[,.;:!?…—–-]` before next word; `name` = has an uppercase
  after the first char, or is capitalized where no sentence starts (sentence start = first word or after `[.!?…]`).
  C++ needs Unicode-aware casefold, `isupper`, `\w` classes: use ICU or Qt `QString::toCaseFolded`/`QChar` categories.
- Gate rules: `_FILLERS` and `("all","right")` skipped at start (`:118-185`); up to 4 `_LEAD_INS` (`:129-135`) may
  precede a name, each must be a lead-in; name variants (`:282-322`) = each name/spoken name as typed, then with
  leading lead-ins removed and/or generic tails (`_NAME_TAILS`) removed, a shortened form must be >= 3 chars and may
  follow lead-ins only when it holds a word typed as a name, or the dropped leading words were said before it, or a
  heard word is written as a name (`_Variant.follows`, `:271-279`); a name made only of tail words may not follow
  lead-ins. `_match_len` (`:325-347`): spacing ignored by concatenation ("chat gpt" = ChatGPT), but a name heard as
  more words than it has does not match when not followed by a pause and the next word is in `_PHRASE_GOES_ON`
  ("to do that"); else word-wise `_alike`. Note `_match_len` indexes `words[k+1]` only when `not words[k].pause`, which
  is safe because the last word always has `pause=True`.
- `_sound`/`_alike` (`:192-252`): crude phonetic key (consonant onset, first vowel class, consonant coda, syllable
  count) after spelling rewrites `ph->f, ck->k, x->ks, q->k, c->k, z->s`, silent final e, `-ed` -> `d` (not after
  t/d); requires both words >= 4 chars, length diff <= 2, onset+coda >= 3 consonants. Pinned pairs in tests (see Tests).
- Trailing match (hotkey only, `:391-409`): name at the end (after optional "please"/"thanks"/"thank you"), not at
  word 0, preceded by lead-ins or a pause.
- Ordering (`:433`): leading first, then longest span (`name_end - first`), then action order.
- `plan` (`:634-692`): returns None for unknown route, no words, no actions, or auto with no match. Candidates: auto
  = matched; hotkey = matched then every other enabled action. Payload question only when > 1 span option; payload
  `action_names` = names+spoken names of matched actions, max 12.
- Threading: `decide` runs on the transcription worker; `_ask` spawns a daemon thread "route-by-voice" so neither a
  slow judge nor reading its key blocks past the budget; cancel polled every 20 ms (`:796-805`). `decide` never raises.
- `status` reads `typesafe.route_from_settings`, `key_present`, `resolve_typesafe_enabled`; never raises (fallback
  "Route by voice isn't available right now.").

### `services/actions/runtime.py`
- Responsibility: run an action job's words; Qt-thread object owned by the controller (`controller.action_runtime`).
- Public: `ActionResult(ok,message,output,stopped)`, `ActionRuntime(controller)` with `deliver(job,text,*,raw_text,
  html,notice)`, `test(action,sample,done)`, `accepting()`, `cancel(*,tests_only)`, `close()`; module functions
  `ensure_action_secret`, `delete_action_secret`, `reply_text`, `prompt_with_context`, `program_words_allowed`,
  `scripts_listening`; constants `CHANNEL_LISTENER_LABEL="claude-channel"`, labels `sse/webhooks/programs/mcp-events`,
  `MAX_WORKERS=4`, `MAX_PENDING=8`, `WEBHOOK_TIMEOUT_S=5.0`, `NOTICE_MAX_CHARS=140`, `CONTEXT_OPEN/CLOSE`, message
  constants (`MISSING_MESSAGE` ... `FULL_ANSWER_COPIED`).
- Threading: `_ResultRelay` QObject singleton moved to the app thread; workers emit `finished(token, result)` only
  (they hold no reference to the runtime, `:719-732`); `_waiting` dict token->callback under lock; dropped tokens
  ignored. Pool created lazily (`ThreadPoolExecutor`, prefix `action-run`). `close()` sets every run's cancel event,
  drops tokens, `shutdown(wait=False, cancel_futures=True)`.
- State per run: `_Run` (job, action, text, raw_text, html, settings snapshot, done callback, cancel Event, notice);
  `_Pending` (run, future, canceled_by); `_Cancel` (quiet, stopped[], late[], waiting count).
- Cancel state machine (`:543-613`, `:739-770`): pending with cancellable future -> stopped now; under way and
  stoppable (program, agent ASK) -> `run.cancel.set()`, `waiting += 1`; under way and not stoppable -> `late`;
  already reached by an earlier cancel -> counted as `earlier`, untouched. When a stoppable run returns, `stopped`
  if `result.stopped` else `late`; `_tell` speaks once `waiting == 0`.
- Uses controller seams: `ui_controller.discard_clipboard_prefetch()`, `set_status`, `show_notice(msg, done=ok)`,
  `show_in_scratchpad`, `copy_to_clipboard(text, html=)`; `transcription_runtime.paste_text_now(answer, job=)`;
  `dictation_pipeline.paste_target`, `PasteTarget.SAME/CHANGED`, `history_fields(job,None,live=True,settings=)`,
  `DictationJob`, `JobMode.ACTION`; `job.snapshot(timeout=)`.
- Edge: settings and action re-read for each run (`:501-502`); a failed result message from a worker is prefixed
  "{name}: " unless a Try-it (`:745-746`); rstrip(".") on failure messages (`:782`).

### `services/actions/agents.py`
- Responsibility: blocking handoff/ask for three agents; called only from runtime workers and from
  `ui_qt/widgets/action_agent_card.py` (`available()`).
- Public: `available()`, `handoff`, `app_links`, `terminal_argv`, `terminal_command`, `npm_shim_command`, `ask`;
  `ASK_INSTRUCTION`, `CLAUDE_APP_TEXT_MAX=14000`, `CLAUDE_CLI_TEXT_MAX=5000`, `LINK_MAX_CHARS=30000`,
  `_MODEL_RE=[\w.:/@\[\]+-]{1,200}`, `_HELP_TIMEOUT_S=15`, `_STDOUT_MAX_BYTES=4 MiB`, `_CODEX_TOOL_FEATURES` (30 names).
- Threading: `_run_headless` starts three daemon pump threads (stdin feed, stdout reader, stderr tail) and polls
  `proc.wait(0.1)` for exit/cancel/deadline; always kills the process tree in `finally` (`:495-499`). `_help` probe
  output cached per (path, version, args) under a lock.
- Edge cases: Claude link includes `&folder=` only when set; claude-cli link always names `cwd` (home if none)
  (`:160-167`); links longer than 30,000 dropped (`:171`); a lone word gets a trailing space for Claude
  (`:197`); `.cmd` shim parse regexes `SET\s+"_prog=([^"]*)"` and `"%_prog%"\s+"%dp0%\\([^"%]+)"\s+%\*` (`:207-208`);
  `_package_exe` globs four `node_modules` patterns (`:266-268`); Codex without a readable feature list still disables
  `shell_tool` (`:432`); Codex model falls back to the user's configured default if valid (`:415-419`); a Codex
  answer with `turn.completed` wins over an earlier `error` (retried hiccup, `:597-598`); OpenCode narration before a
  tool call is cleared (`:652-653`); OpenCode model unknown and explicitly requested -> "OpenCode has no model {model}
  with your current providers. Pick another in Settings." Temp workdir `openwhisper-ask-*` always removed.

### `services/actions/launch.py`
- Responsibility: OS-level link opening and terminal launching; no app state.
- Public: `LaunchError`, `NoAnswer`, `open_uri`, `handler_available`, `forget_handlers`, `open_in_terminal`;
  `HANDLER_CACHE_S=60`, `WINDOWS_COMMAND_LINE_MAX=32000`, `LINK_MAX_CHARS=30000`, `CLAUDE_APP_TEXT_MAX=14000`,
  `OPEN_WAIT_S=2.0`, `TERMINAL_START_CHECK_S=0.5`, `EXPLORER_WAIT_S=30.0`.
- Threading: Windows Explorer call on its own daemon thread "explorer-open" with COM STA, serialized by
  `_explorer_turn` lock (no queue behind a hung Explorer, `:233`, `:273-302`); reaper threads for detached children.
- Windows constants: `ASSOCF_IS_PROTOCOL|INIT_IGNOREUNKNOWN|NOTRUNCATE`, `ASSOCSTR_COMMAND=1`, `EXECUTABLE=2`,
  `DELEGATEEXECUTE=18`, no-handler HRESULTs `0x80070483`, `0x80070490`, `CO_E_APPNOTFOUND=0x800401F5`;
  `CLSID_ShellWindows {9BA05972-F6A8-11CF-A442-00A0C90A8F39}`; `FindWindowSW(CSIDL_DESKTOP, SWC_DESKTOP,
  SWFO_NEEDDISPATCH)` -> `IServiceProvider::QueryService(SID_STopLevelBrowser, IID_IShellBrowser)` ->
  `QueryActiveShellView().GetItemObject(SVGIO_BACKGROUND, IDispatch)` -> `.Application` -> `ShellExecute(uri,"","","",1)`;
  `AllowSetForegroundWindow(explorer pid)` first. This is the documented "launch unelevated / as the shell" pattern;
  in C++ it is straightforward COM (`IShellWindows`, `IShellDispatch2`).

### `services/automation/events.py`
- Public: `Event(id,seq,type,time,data)` with `to_dict()` (adds `"v":1`) and `to_json()` (non-ASCII kept, compact);
  `Subscription(close, closed, wants)`; `EventBus(buffer_size=500, seq_base=0)` with `publish(type,data,*,replay)`,
  `subscribe(cb,*,types,label)`, `subscribers(label)`, `since(seq,*,types)->(events,gap)`, `recent(limit,*,types)`,
  `last_seq`, `clear`, `close`; module `bus()`, `clear()`, `set_bus()`, `publish()` (never raises).
- Model: `threading.Condition`; recipients fixed at publish time (a later subscriber never gets it, a later-closed one
  still does, `:163-171`); dispatch thread "automation-events" created on demand; `_dropped[type]` = newest seq no
  longer buffered (eviction or clear). `since` logic (`:213-248`) is the contract the SSE and MCP replay depend on.
- Event id `evt_` + 24 hex; time ISO UTC with milliseconds `YYYY-MM-DDTHH:MM:SS.mmmZ`.

### `services/automation/catalog.py`
- Pure data + builders (see Data and formats). `user_started(source)`; `known_types` keeps public types, order, dedup;
  `job_fields(None)` = file mode; `transcription_completed` publishes `raw_text` only for modes dictation/action/
  walkthrough/file and only when different; `transcription_failed` maps unknown error to `failed` and keeps
  `message` only for `refused` (<= 500 chars); `action_ran.message` <= 500; `filter_failed.reason` <= 200;
  `channel_message.origin` = dictated only when `job.program_words is False`.

### `services/automation/permissions.py`
- `granted(settings, perm)` (unknown perm -> False; malformed value -> default), `require` raises `PermissionDenied`
  (`.permission`, `.message`), `describe` -> public-name dict.

### `services/automation/signing.py`
- Pure; `PREFIX="whsec_"`, `new_secret`, `signature`, `headers(secret,msg_id,body,*,timestamp)`, `verify(secret,
  received,body,*,tolerance_s=300,now)`; header names case-insensitive; space-separated multiple signatures; secret
  must decode as strict base64 and be non-empty.

### `services/automation/models.py`
- Public: limits, `STAGES`, `FILTER_KINDS`, timeouts, `Webhook`, `Program`, `TextFilter` (frozen, `to_dict`),
  `load_*`, `validate_url`, `url_is_private`, `validate_*`, `save_*`, `delete_*`, `find_*`, `new_id`,
  `webhook_secret_name`. Loading clamps ints into range rather than dropping (`:115-118`); events filtered through
  `known_types` (max 64 read); invalid filter kind/stage or missing url/program skipped.

### `services/automation/webhooks.py`
- Public: `TIMEOUT_S=5`, `RETRY_DELAYS_S=(1,5,25)`, `RETRY_AFTER_MAX_S=60`, `MAX_QUEUED=500`, `LANE_IDLE_S=30`,
  `MAX_RECORDS=50`, `MAX_REPLY_BYTES=1 MiB`, `TEST_EVENT_TYPE="webhook.test"`, `SUBSCRIPTION_LABEL="webhooks"`,
  `CONTENT_TYPE`; `DeliveryRecord(event_id,event_type,at,ok,status,elapsed_ms,error,attempt)`, `SecretUnavailable`,
  `SECRET_MISSING`, `user_agent`, `ensure_secret`, `rotate_secret`, `delete_secret`, `no_answer_copy`
  ("No answer within {t:g} s"), `post_signed`, `post_signed_reply`, `deliveries`, `start`, `stop`, `reload`,
  `send_test` (Future), internal `_exchange`, `_new_client` (shared with filters).
- Engine (`:569-749`): one bus subscription for the union of enabled webhooks' types (re-subscribed new-before-old
  on change, dedupe by `seq > _last_seq`); lanes keyed by webhook id; `configure` drops lanes of removed/disabled
  hooks and filters queued items no longer wanted; retries go back to the front with `not_before`, keeping order.
  `stop` waits up to 1 s total for lane threads.
- `_exchange` error copy (`:330-396`): "Couldn't connect", "The proxy refused the request", "The server closed the
  connection", "The connection broke", "The request failed (Type)", "The address can't be used (Type)",
  "The reply was too large", timeout copy; "HTTP {status}". Damaged secret: "The signing secret is damaged. Make a
  new one in Settings." Store failure: "Couldn't keep the webhook's signing secret. {detail}".

### `services/automation/programs.py`
- Public: `MAX_QUEUED=20`, `MAX_RECORDS=20`, `MAX_STDOUT_BYTES=1 MiB`, `TEST_EVENT_TYPE="program.test"`,
  `SUBSCRIPTION_LABEL="programs"`, `OUTPUT_GRACE_S=1.0`, `PIPE_POLL_S=0.05`, `CANCEL_POLL_S=0.05`,
  `CANCELED="Canceled"`; `RunRecord(event_id,at,ok,exit_code,elapsed_ms,error)`, `child_env`, `run_once`, `runs`,
  `start`, `stop`, `reload`, `run_test`.
- `run_once` pipes (`:182-311`): each pipe served on its own thread; on POSIX non-blocking stdin plus `poll()` with
  50 ms timeout; on Windows a blocked synchronous read/write is cancelled with `CancelSynchronousIo` on the thread
  (retry loop 1 s). After a timeout/cancel the tree is killed and pipes get 2 s; after a clean exit `_disown` clears
  the Windows job's kill-on-close limit (`SetInformationJobObject(JobObjectExtendedLimitInformation, zeros)`) so apps
  the program opened stay open, and pipes get 1 s.
- Runners: module-level `_cond` and `_runners` dict; one daemon thread per program while it has work; tests deque
  served before events; event items re-resolve the program at start (settings changes apply immediately).

### `services/automation/filters.py`
- Public: `LEASE_TTL_S=60`, `MAX_LEASES=10`, `STAGE_BUDGET_S=3.0`, `MIN_SLICE_S=0.05`, `MODES`, `LOOPBACK_HOSTS`,
  `MAX_REPLY_BYTES`, `REQUEST_VERSION=1`; `FilterOutcome(text,canceled,changed,program_changed)`, `Lease`,
  `register_lease`, `renew_lease`, `drop_lease`, `drop_all_leases`, `leases`, `apply`, `StageRun(saved, leased)`.
- Threading: called on the transcription worker; each filter runs on a helper daemon thread "text-filter" and is
  waited with 20 ms cancel polls (`_await`, `:301-336`); HTTP clients per private/public kept for the app's life and
  warmed in the background on lease registration so TLS setup is not charged to a filter's time.
- `StageRun` shares one budget between its `saved` and `leased` parts (`:512-545`); routing happens between them in
  the pipeline (`transcription.py:1900-1918`).
- Edge: a cancel wins over an answer that arrived with it (`:323-327`); canceled stage returns the original text, not
  the partially filtered one (`:554-555`, `:574-575`); a program filter gets body + `\n`; JSON float `timeout_ms`
  with integer value accepted (`:99-100`).

### `services/automation/control.py` and `app_control.py`
- `control.py`: codes `busy`, `not_found`, `unavailable`, `refused`; `ControlError(code,message)`; `LiveState` with
  `to_dict()` (`action_id`/`job_id` null when empty); `AppControl` protocol; `set_app_control`, `app_control`,
  `require_app_control` ("OpenWhisper isn't ready for this yet.").
- `app_control.py`: `QtAppControl` holds the controller by weak reference; `call_on_qt(fn)` runs inline on the Qt
  thread, else emits `_Relay.call(fn, future)` (QueuedConnection) and waits `CALL_TIMEOUT_S=2.0`, cancels if not
  started, else waits `RUNNING_GRACE_S=2.0`. `with_statuses` captures `controller.status_update` strings emitted
  during a start to report a refusal's reason. Uses `controller.recorder.is_recording`,
  `transcription_runtime.has_active_job/current_job/start_recording(source=)/cancelable/cancel()/run_action_text`,
  `controller.start_action_recording(id, source=)`, `controller.is_meeting_active()`, `controller.stop_recording()`.

### `services/automation/api.py`
- FastAPI router mounted by `services/agent_mcp/app.py:675` on the in-app listener (bearer-token middleware there).
  Every handler re-reads settings and permissions; app calls run in `asyncio.to_thread`.
- Limits: `MAX_ACTION_TEXT=20000`, `MAX_NOTICE_TEXT=500`, `MAX_NOTICE_TITLE=100`, `MAX_ID_CHARS=64`,
  `MAX_URL_CHARS=2048`, `MAX_RUN_BODY=256 KiB`, `MAX_SMALL_BODY=16 KiB`, `PING_INTERVAL_S=15`, `ENABLED_CHECK_S=0.5`,
  `PERMISSION_CHECK_S=2`, `STREAM_QUEUE_SIZE=1000`.
- `read_json_body` (`:154-176`): declared Content-Length over limit -> 413 before reading; streamed read also capped;
  empty -> `{}`; not an object -> 422 "The request body must be a JSON object.".
- `_string` (`:209-221`): "{label} must be text of 1 to {max:,} characters." / "... at most ..."; lone surrogate ->
  "{label} must be valid Unicode text." (`is_unicode_text`).
- `peer_is_local`: loopback (incl. v4-mapped, scope id stripped) only; filter leases refused for others with 403
  `forbidden` "Only a program on this computer can lease a text filter.".
- `EventStream` (`:251-380`): subscribes before reading the replay buffer; position only rises; a position greater
  than `last_seq` is another session's -> gap `missed_from_seq=1`, position 0; slow gap `missed_from_seq` = first
  pending event above position, else the overflow event.

### `services/automation/meeting_events.py`
- `forward(kind, payload, *, meeting_id)` on the Qt thread from `services/runtime/meeting.py:2040`; never raises;
  `_announced` ordered set capped at 32.

### `services/automation/service.py`
- `start(controller)` idempotent (module flag), each sub-start isolated in try/except; `reload(kind)`; `stop()`.
  Called from `application_controller.py:890` (after the window shows), `:449` (settings changes), `:1361` (quit,
  after `action_runtime.close()`).

### `services/automation/channel_bridge.py`
- Separate process mode (`main.py:174-179`, before Qt/CUDA load). Classes: `LinkError(state,message)`, `Target`
  (`on_this_computer`), `AppLink` (URL/token resolution, `connect`, `token` cached and re-read after 401, `headers`,
  `notify`), `SseParser` (WHATWG rules: `last_id` updates only when a block ends; NUL in id ignored; one leading space
  stripped; default type `message`), `EventStream` (thread "openwhisper-channel-events"; state-change-only logging;
  socket shutdown to unblock reads on stop), `MessageWriter` (one ASCII JSON line, lock, closed flag),
  `ChannelServer` (`serve`, `_request`, `_notification`, `_initialize`, `_call_tool` on a thread per call),
  `PipeLineReader` (Windows: `PeekNamedPipe` polling 5-50 ms so no synchronous read blocks other threads' pipe calls),
  `protocol_streams` (dup fds 0/1 to binary, then point fd 1 and `sys.stdout` at stderr), `main`.
- Constants: `PROTOCOL_VERSIONS`, `CLIENT_HEADER="X-OpenWhisper-Client"`, `CLIENT_NAME="claude-channel"`,
  `EVENTS_PATH="/v1/events?types=channel.message"`, `NOTIFY_PATH="/v1/notify"`, `URL_ENV="OPENWHISPER_URL"`,
  `TOKEN_ENV="OPENWHISPER_TOKEN"`, backoff 1 s doubling to 30 s (reset after a stream opened), connect 5 s, read 45 s,
  notify 5 s, reply 500, title 80, meta values <= 200 chars, meta keys `[A-Za-z_][A-Za-z0-9_]*`.
- Strings: `INSTRUCTIONS` (full text at `:95-113`, must be kept verbatim apart from branding), `GAP_NOTICE` (`:89-93`),
  `REPLY_TOOL` schema and annotations (`:115-146`), `_USAGE` (`:148-162`), stream/notify refusal copy (`:405-429`),
  log lines ("Connected to OpenWhisper at {host:port}; waiting for dictated messages." etc.).
- Remote rule (`:274-296`): a non-loopback `OPENWHISPER_URL` requires https and `OPENWHISPER_TOKEN`; the saved token
  is only ever sent to this computer. Saved token read with a fresh `CredentialStore()` each time
  (`CREDENTIAL_NAME="OPENWHISPER_MCP_TOKEN"`).
- Update step-aside (`:530-541`, `:618-624`): on Windows frozen builds, when unreachable and an update/setup mutex
  exists, closes stdout and `os._exit(0)`.

## Data and formats

### Settings keys (all read through `services/settings.py` `SettingsKey`)
| Key | Type | Default | Read where |
|---|---|---|---|
| `actions` | list of action dicts | `[]` | `model.py:load_actions` (and backup, hotkeys, UI) |
| `voice_routing_enabled` | bool | false | `routing.py:_decide,_status` |
| `voice_routing_on_dictation` | bool | false | same |
| `hotkeys.route_record` | str | "" | `routing.py:_route_shortcut` |
| `api_live_events` | bool | **true** | `permissions.py` |
| `api_run_actions`, `api_control_recording`, `api_text_filters`, `api_agent_prompts` | bool | false | `permissions.py` |
| `automation_webhooks` | list | `[]` | `models.py:load_webhooks` |
| `automation_programs` | list | `[]` | `models.py:load_programs` |
| `automation_filters` | list | `[]` | `models.py:load_filters` |
| `mcp_port` | int 1-65535 | 8767 | `channel_bridge.py:configured_port` |
| `mcp_enabled` | bool | false | `channel_bridge.py:unreachable_message` |
| Fast judgments settings (TypeSafe enabled, route, key) | - | - | `typesafe.route_from_settings`, `key_present`, `resolve_typesafe_enabled` (a12) |

Action dict (`model.py:126-137`): `{"id","name","kind","params":{...},"hotkey","shaping","spoken_names":[],
"description","enabled"}`. `params` per kind (`PARAMS`): open_link `{"template":""}`; agent `{"agent":"claude_code",
"mode":"handoff","surface":"auto","folder":"","new_folder":false,"include_selection":false,"model":"",
"output":"scratchpad"}`; webhook `{"url":"","output":"none"}`; program `{"program":"","args":[],"output":"none",
"timeout_s":30}`; channel/clipboard/scripts `{}`. `shaping`: `raw` | `cleanup` | `profile:<id>` | `transform:<id>`.

Webhook dict `{"id","name","url","events":[],"enabled"}`; Program `{"id","name","program","args":[],"events":[],
"enabled","timeout_s"}`; Filter `{"id","name","kind":"http|program","stage":"before_cleanup|after_cleanup","url",
"program","args":[],"timeout_ms","enabled"}`. Backup (`services/backup.py:267-310`) scrubs these lists and turns
webhook/program/agent actions off on restore: field names must stay stable.

### Credential store names (service `OpenWhisper`, a10)
`OPENWHISPER_ACTION_SECRET_{action_id}`, `OPENWHISPER_WEBHOOK_SECRET_{hook_id}` (value `whsec_...`),
`OPENWHISPER_MCP_TOKEN` (read by the channel). An existing install's secrets must keep these names.

### Files on disk
- `~/OpenWhisper Agent Tasks/task-%Y-%m-%d-%H%M%S-<random>` (persistent, created per new-folder handoff).
- `%TEMP%/openwhisper-ask-*` (mcp.json `{"mcpServers":{}}`, settings.json `{"disableAllHooks":true,
  "alwaysThinkingEnabled":false}`, instructions.md), deleted after the ask.
- `%TEMP%/openwhisper-agent-workspace` (stable empty cwd for agents, from `installed_agents`).
- `openwhisper-*.command` / `.sh` one-shot terminal scripts (mode 0700, self-deleting).
- No database tables; delivery/run records are memory only.

### Event wire format (`events.py:Event.to_dict`)
`{"id":"evt_<24hex>","seq":<int>,"type":"<type>","time":"YYYY-MM-DDTHH:MM:SS.mmmZ","v":1,"data":{...}}`.
Payloads (`catalog.py`):
- `recording.started`: `{job_id, mode, action_id|null, source, route}`
- `recording.stopped`: `{job_id, duration_s|null}` (3 decimals); `recording.canceled`: `{job_id}`
- `transcription.preview`: `{job_id, text, final}` (published with `replay=False`, <= 4/s by the publisher)
- `transcription.completed`: `{job_id, mode, action_id, source, routed, text, raw_text|null, language, app|null,
  cleanup:{provider,model,level}|null, audio_s|null}`; job None -> mode `file`
- `transcription.failed`: `{job_id, error: canceled|no_speech|refused|failed, message|null}`
- `history.saved`: `{job_id, record_id(str), kind}`
- `action.ran`: `{job_id, source, action_id, name, kind, ok, message(<=500), output: none|paste|scratchpad|notice|
  clipboard, text?}` (text only for `scripts`)
- `channel.message`: `{job_id, source, origin: dictated|program, action_id, text, app|null}`
- `route.decided`: `{job_id, action_id|null, confidence|null (4 decimals), fallback}`
- `filter.failed`: `{filter_id, name ("API filter" for leases), stage, reason(<=200)}`
- `meeting.started` `{meeting_id,title}`, `meeting.ended` `{meeting_id,status}`, `meeting.segments`
  `{meeting_id, items:[{segment_id,speaker,text,start_s,end_s}]}`, `meeting.voice_command` `{meeting_id,command}`
- `events.gap`: `{missed_from_seq, reason: evicted|restarted|slow}`; test events `webhook.test`, `program.test`
  (never on the bus, seq 0).
- `app` object: `{id, name, category}` from history context fields `app_id/app_name/app_category`.

### Webhook HTTP (saved hooks)
POST event wire JSON (UTF-8, compact, non-ASCII kept), headers `Content-Type: application/json`, `webhook-id`
(=event id), `webhook-timestamp` (unix s), `webhook-signature: v1,<b64>`, `User-Agent: OpenWhisper/<ver>`; no redirects.

### Live API (on the in-app listener, `http://127.0.0.1:<mcp_port>`, `Authorization: Bearer <token>`)
Errors: `{"error":{"code","message"}}`; `ControlError` codes map busy 409, not_found 404, refused 422,
unavailable 503; permission 403 `permission_denied`; validation 422 `invalid_parameters`; 413 `request_too_large`;
unexpected 503 `unavailable` "OpenWhisper couldn't do that right now. Try again.".
| Route | Permission | Body / query | Response |
|---|---|---|---|
| `GET /v1/live/state` | token only | - | 200 `{recording,busy,meeting,mode,action_id,job_id,version,permissions:{live_events,...},listeners:{events,claude_channel}}` |
| `GET /v1/events` | live_events | `types` (comma list, <= 2000 chars, unknown -> 422), `since` (>= 0), headers `Last-Event-ID` (digits, <= 18), `X-OpenWhisper-Client` | `text/event-stream`; frames `id: <seq>\nevent: <type>\ndata: <ascii json>\n\n`; first `retry: 2000\n\n`; `: ping\n\n` |
| `GET /v1/actions` | token only | - | 200 `{"actions":[public_dict...]}` |
| `POST /v1/actions/{id}/run` | run_actions (+agent_prompts for agent/channel) | `{"text"}` 1-20,000 | 202 `{"job_id"}` |
| `POST /v1/recording/start` | control_recording | `{"action_id"?}` <= 64 | 202 `{"job_id"}` |
| `POST /v1/recording/stop` / `cancel` | control_recording | - | 202 `{}` |
| `POST /v1/filters` | text_filters, loopback peer | `{"url","stage","timeout_ms"?}` | 201 `{id,url,stage,timeout_ms,modes,expires_at,ttl_s:60}` |
| `POST /v1/filters/{id}/renew` | text_filters | - | 200 lease; 404 "That filter lease has expired. Create a new one." |
| `DELETE /v1/filters/{id}` | text_filters | - | 204; 404 "That filter lease has expired." |
| `POST /v1/notify` | run_actions | `{"text"(1-500),"title"?(<=100)}` | 202 `{}` |
Path ids 1-64 chars. The route list is pinned by `test_automation_api_routes.py:test_the_contract_lists_the_live_routes`.

### Text filter protocol
Request JSON (compact UTF-8; program filters get it + `\n` on stdin): `{"v":1,"stage","job_id","mode","action_id",
"text","language"|null,"app"|null}`. Reply semantics as A19-43.

### Webhook action protocol
POST `{"v":1,"action":{"id","name"},"job_id","text","raw_text","language","app"}` signed with the action secret;
reply `{"text": "..."}` (<= 1 MiB) is the answer.

### Program environment
Event hooks: `OPENWHISPER_EVENT_TYPE`, `OPENWHISPER_EVENT_ID`, `OPENWHISPER_EVENT_SEQ`. Program actions:
`OPENWHISPER_ACTION_ID`, `OPENWHISPER_ACTION_NAME`, `OPENWHISPER_JOB_ID`. Users' scripts read these names (rename
only as a deliberate break).

### Claude Code channel protocol
stdin/stdout newline-delimited JSON-RPC 2.0. `initialize` result `{protocolVersion, capabilities:{experimental:
{"claude/channel":{}}, tools:{}}, serverInfo:{name:"openwhisper", title:"OpenWhisper", version}, instructions}`;
`ping` -> `{}`; `tools/list` -> `[reply]`; `tools/call` -> `{content:[{type:"text",text}], isError}`; anything else
-32601 "Method not found: {m}"; parse error -32700; batches -32600 "Batches are not supported"; non-object params
-32602. Notification out: `notifications/claude/channel` `{content, meta:{origin, job_id, action, app}}`. Stream starts
on `notifications/initialized`. Registration snippet (`.mcp.json`, server name `openwhisper`, launch with
`claude --dangerously-load-development-channels server:openwhisper`) is generated by a20 `agent_mcp/setup.py`.

### Judge request (to a12's `judge.ask(state, questions, timeout_s=)`)
`state={"dictation": clipped}`; `questions={"route":{"type":"choice","instructions":str,"criteria":{action_id:
{name,spoken_names?,purpose?,does}, "none":{what,includes}}}, "payload"?:{"type":"choice","instructions":{question,
action_names,addressing},"criteria":{span:null,..., "none":PAYLOAD_NONE}}}`. Answers `{"route":{"choice","confidence",
"probabilities"}, "payload":{"choice","probabilities"}}`.

### Added by check

- Environment variables read by this area besides those above: `OPENWHISPER_LOG_LEVEL` (channel process log level) and `TERMINAL` (Linux terminal preference, A19-57). The only CLI surface is `--channel` and `--channel -h/--help`.
- Settings keys also read: `hotkeys` (for `route_record`, A19-11) and the resolvers `resolve_voice_routing_enabled`, `resolve_voice_routing_on_dictation`, `resolve_typesafe_enabled` (defaults live in a10).
- Event JSON keeps non-ASCII for webhook bodies and program stdin, but is ASCII-escaped on SSE frames and channel lines.
- `LiveState.to_dict` keys: `recording, busy, meeting, mode, action_id, job_id, version` (`action_id` and `job_id` null when empty; `mode` empty string when no job).
- Lease JSON: `{id, url, stage, timeout_ms, modes, expires_at (Unix s), ttl_s}`.

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| PyQt6 `QObject`, `pyqtSignal`, `QCoreApplication`, `QThread` | result relay, AppControl relay | `QObject` + `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` or signals; `std::promise/future` with timeout |
| `concurrent.futures.ThreadPoolExecutor`, `Future` | action pool, test futures | `QThreadPool` + cancellable tasks, or a small `std::jthread` pool; `QFuture`/`QPromise` |
| `threading` (Event, Condition, Lock, Thread) | everywhere | `std::mutex`, `std::condition_variable`, `std::stop_token`, `std::jthread` |
| `httpx` (sync client, streaming, no redirects, `trust_env`) | webhooks, filters, signed POSTs | `QNetworkAccessManager` on a worker thread with its own event loop (`setRedirectPolicy(ManualRedirectPolicy)`, `QNetworkProxy::NoProxy` for private hosts, `setTransferTimeout`, read cap via `readyRead`), or libcurl via vcpkg for simpler blocking code on lane threads |
| `fastapi`/`starlette` streaming | live routes, SSE | whatever a20 picks for the listener; `QHttpServer` (Qt 6.4+; chunked/streamed responses need 6.8+ `QHttpServerResponder::writeBeginChunked`) or a small custom server on `QTcpServer`/`QSslServer` |
| `http.client`, `socket` | channel bridge client | `QTcpSocket`/`QSslSocket` with hand SSE parsing, or `QNetworkAccessManager` streaming `readyRead`; the bridge must not load the GUI: build it as a separate small console target linking QtCore+QtNetwork only (or plain sockets) |
| `json` | all payloads | `QJsonDocument` (note: ASCII-escaped output needed for SSE and channel lines; QJson writes UTF-8, so escape non-ASCII manually) or nlohmann/json (`dump(-1,' ',true)` gives ensure_ascii) |
| `hmac`, `hashlib`, `base64`, `secrets` | signing, ids | `QMessageAuthenticationCode(QCryptographicHash::Sha256)`, `QByteArray::toBase64/fromBase64(AbortOnBase64DecodingErrors)`, `QRandomGenerator::system()`; constant-time compare hand-written |
| `subprocess`, `os`, `select`, `msvcrt`, `_winapi`, `ctypes` kernel32 | programs, agents, terminals | native `CreateProcessW` + Job objects + `CancelSynchronousIo`/overlapped pipes on Windows; `posix_spawn`/`fork+setsid`, `poll` on POSIX. `QProcess` lacks process-group/job-tree kill and "disown on clean exit": wrap a20's `agent_process` port |
| `ctypes` shlwapi/ole32/user32, `pywin32` (`pythoncom`, `win32comext.shell`) | handler lookup, Explorer ShellExecute | `AssocQueryStringW`, `CoInitializeEx`, `IShellWindows::FindWindowSW`, `IShellDispatch2::ShellExecute`, `AllowSetForegroundWindow` directly in C++ |
| PyObjC `AppKit.NSWorkspace` | macOS handler lookup | `LSCopyDefaultApplicationURLForURL` / `NSWorkspace URLForApplicationToOpenURL:` in Objective-C++ |
| `shlex`, `tempfile`, `shutil.which`, `glob` | terminal scripts, shim resolution | hand-written POSIX single-quote escaping; `QTemporaryFile`/`mkstemp`; `QStandardPaths::findExecutable`; `std::filesystem` |
| `urllib.parse.quote/urlsplit`, `ipaddress` | link encoding, URL checks | `QUrl::toPercentEncoding(text, {}, {})` (must encode every reserved char; verify it matches `quote(safe="")`, which leaves `-._~` and alnum), `QUrl`, `QHostAddress::isLoopback/isPrivateUse/isLinkLocal` (Qt 6.6+) |
| `re` (Unicode `\W`) | routing words | ICU regex or a hand tokenizer over `QChar::isLetterOrNumber` |
| `email.utils.parsedate_to_datetime` | Retry-After dates | `QDateTime::fromString(s, Qt::RFC2822Date)` |
| Meeting ACP client (`meeting/agent/installed/acp.py`) | OpenCode ask | a26 port of the ACP JSON-RPC client |

## Platform-specific code

- **Windows**: Explorer-hosted ShellExecute for links with fallback `os.startfile` (`launch.py:236-362`); handler lookup
  via `AssocQueryStringW` under COM (`:157-217`); terminals only for `.exe`, `CREATE_NEW_CONSOLE` (`:452-473`);
  npm `.cmd` shim resolution for agents (`agents.py:205-272`); `run_once` uses `CREATE_NEW_PROCESS_GROUP` plus
  `popen_agent`'s `CREATE_NO_WINDOW | CREATE_SUSPENDED` and kill-on-close job, `CancelSynchronousIo` to stop pipe
  threads, `_disown` to clear the job limit (`programs.py:154-156`, `:286-390`); winerror 193 copy; channel bridge
  `PipeLineReader` with `PeekNamedPipe`, binary-mode fds, update/setup mutex check (`channel_bridge.py:530-541`,
  `:991-1062`).
- **macOS**: `open uri`; handler via NSWorkspace (None without PyObjC); terminal through a `.command` script with
  `#!/bin/zsh -l` opened by `open -a Terminal` (login environment, not ours).
- **Linux (X11/Wayland/Hyprland alike)**: `xdg-open` (2 s wait), `xdg-mime query default x-scheme-handler/<s>`;
  terminal discovery via `$TERMINAL` and the list in `launch.py:536-539`, per-terminal argument styles
  (`:523-534`), `/bin/sh script` so a noexec `/tmp` still works; `start_new_session=True` for every child; POSIX
  `poll` on pipes, `killpg` SIGTERM then SIGKILL (a20 `agent_process`). No Wayland/Hyprland-specific code in this area;
  `dbus` is a recognized job source (Omarchy/shell integration elsewhere).

## Engine-specific code

None of this area depends on the speech engine. Nothing here is for faster-whisper, Moonshine, Qwen3-ASR, Parakeet MLX
or the worker-process plumbing. Points of contact with the engine are indirect:
- `app_control._start` refuses when the engine is not ready only through `runtime.start_recording` returning False and
  the status text it emitted; OpenWhisper_CPP's in-process Parakeet `EngineWorker` must emit an equivalent refusal status.
- `transcription.preview` events (published by a04) come from live preview; with Parakeet TDT that is windowed
  `Recognizer::transcribe`, so the <= 4/s throttle and `final` flag still apply.
- The "agent ask" and Route-by-voice judge use the user's text model / cloud judges (a12), not speech engines; they
  are not undecided speech backends but do require network or local LLM backends that a12 decides.

## Dependencies on other areas

Calls out:
- a10 settings/credentials: `SettingsKey.*`, `settings_manager.load_all_settings/mutate_settings`,
  `resolve_bool_setting`, `credentials.store()` get/set/delete, `CredentialStore`, `CredentialStoreError`.
- a20 agent API/MCP: `installed_agents` (`cached_scan`, `find_agent`, `candidate_paths`, `agent_child_env`,
  `agent_workspace_dir`, `popen_flags`, `kill_process_tree`, `configured_default_model`, `sign_in_hint`,
  `AGENT_SPECS`, `InstalledAgent`), `agent_process` (`popen_agent`, `kill_agent_process`, `run_agent_probe`, Windows job
  object `_agent_job` with `_lock/_handle/_api`), `agent_api.app._error`, `agent_mcp.events_ext` (`subscription_count`,
  `start_delivery`, `reload`, `stop_delivery`), `agent_mcp.setup.CHANNEL_SERVER_NAME`,
  `agent_mcp.runtime.CREDENTIAL_NAME`.
- a26 meeting agent: `meeting.agent.installed.acp` (`AcpConnection`, `AcpError`, `initialize`, `config_option`,
  `model_choices`).
- a12 text AI: `typesafe.judge_from_settings`, `route_from_settings` (`.profile`, `.name`, `.is_local`,
  `.key_phrase`), `key_present`; `cleanup_profiles.find_cleanup_profile/normalize_hotkey`; `text_transforms.find_transform`.
- a06 hotkeys: `hotkey_conflicts.hotkey_conflict(…, exclude=(ACTION, id))`.
- a05/a04 pipeline: `dictation_pipeline.DictationJob`, `JobMode`, `PasteTarget`, `paste_target`, `history_fields`,
  `begin_job`; `transcription_runtime.paste_text_now/run_action_text/start_recording/...`.
- a03 UI controller: `set_status`, `show_notice(done=)`, `show_in_scratchpad`, `copy_to_clipboard(html=)`,
  `discard_clipboard_prefetch`.
- a01: `_version.__version__`, `config.CONTEXT_CAPTURE_DEADLINE_S`, `process_env`; `services.app_update_apply.mutex_exists`,
  `update_contract.SETUP_MUTEX_NAME/UPDATE_MUTEX_NAMES` (updater area).

Called in by:
- `services/application_controller.py` (a03/a04): constructs `ActionRuntime`, wires Try it and cancel, calls
  `automation.service.start/reload/stop`, `routing.status` for the Route-by-voice shortcut.
- `services/runtime/transcription.py` (a04): `routing.decide`, `filters.apply/StageRun`, `model.find_action/SHAPING_RAW`,
  `program_words_allowed`, `catalog`/`events.publish` for pipeline events, `ControlError` codes, delivery to
  `action_runtime.deliver`.
- `services/dictation_pipeline.py` (a05): `catalog` builders. `services/runtime/hotkeys.py`: `load_actions` for
  action hotkeys. `services/runtime/meeting.py:2040`: `meeting_events.forward`. `history_manager.py:621` and
  `meeting/persist/data_lifecycle.py:113`: `events.clear()`.
- `services/agent_mcp/app.py`, `events_ext.py`, `controls.py` (a20): mount `create_live_router`, `filters.drop_all_leases`,
  bus, permissions, catalog, signing, `post_signed`, `validate_url`, `url_is_private`, `peer_is_local`, `is_unicode_text`.
- `services/backup.py` (a10): `ActionKind`, `load_actions`.
- a21 UI: `ui_qt/dialogs/settings_actions.py`, `settings_hooks.py`, `settings_mcp.py`, `widgets/action_*`,
  `hooks_panel.py` use model/presets/validation/save, `routing.status`, `ActionResult`, `agents.available`,
  webhook/program engines (`send_test`, `run_test`, `deliveries`, `runs`, `rotate_secret`, `delete_secret`),
  `filters.leases`. `ui_qt/ui_controller.py:1425` uses `ActionResult`.
- `main.py:174-179`: `--channel` entry. `benchmarks/voice_routing/run.py`, `cases.py`: routing benchmark.

## Tests

All under `D:\coding\whisper_local\tests`. Grouped by module:
- Model: `test_automation_foundation_models.py` (round trip, unknown params dropped, public dict, new_folder, link
  encoding/refused schemes, spoken-name uniqueness, hotkey conflicts, shaping existence, kind params, presets valid
  once filled, permission defaults), `test_automation_fix_pipeline_backup.py` (backup scrubbing of actions).
- Routing: `test_automation_routing_gate.py`, `test_automation_fix_routing_gate.py` (gate sentences that must not pass
  and must pass, mishearing pairs, lowercase names, pauses, generic names), `test_automation_routing_decide.py`,
  `test_automation_fix_routing_timeout.py` (cut off at budget, cancel), `test_automation_fix_routing_failure.py`,
  `test_automation_routing_status.py`, `test_automation_fix_routing_status.py` (exact status copy and `fix`),
  `test_text_judge.py` (judge contract). Benchmark corpus `benchmarks/voice_routing/cases.py` (480 lines, tags such as
  `gate-stop`, action sets).
- Runtime: `test_automation_actions_runtime.py` (787 lines: every kind, outputs, paste target, Try it, no logging,
  env hygiene), `test_automation_actions_relay.py` (relay thread, close, tokens), `test_automation_actions_pipeline.py`,
  `test_automation_actions_paste_seam.py`, `test_automation_fix_actions_runtime.py` (pending cap, cancel, program
  runs never copy, scripts listeners, notice shortening, frame escaping, Claude app limit), `test_automation_fix2_cancel.py`,
  `test_automation_r3_cancel_finished.py`, `test_automation_r4_cancel_outcome.py`, `test_automation_r3_program_answers.py`,
  `test_automation_r2_program_words*.py`, `test_automation_r2_selection.py`, `test_automation_r2_caret_text.py`.
- Agents/launch: `test_automation_agents_handoff.py`, `test_automation_agents_ask.py`, `test_automation_fix_actions_agents.py`
  (JSON shapes, npm shim), `test_automation_agents_launch.py`, `test_automation_fix_actions_launch.py`,
  `test_automation_r2c_launch.py`, `test_automation_r3c_launch.py` (Explorer path; some checks drive the real shell
  only when asked), `test_automation_r4b_launch.py`.
- Events/catalog: `test_automation_foundation_events.py`, `test_automation_fix_events_gap_reason.py`,
  `test_automation_r3_gap_per_type.py`, `test_automation_fix_pipeline_events.py`, `test_automation_fix_pipeline_lifecycle.py`.
- Signing: `test_automation_foundation_signing.py` (published Standard Webhooks vector: secret
  `whsec_MfKQ9r8GKYqrTwjUPD8ILPZIo2LaLaSw`, id `msg_p5jXN8AQM9LWM0D4loKWxJek`, ts 1614265330, body
  `{"test": 2432232314}` -> `v1,g0hM9SsE+OTPJTGt/tmIKtSyZlE3uFJELVlNIOLJ1OE=`).
- Hooks: `test_automation_hooks_webhooks.py` (716 lines: order, retries, Retry-After, redirects, proxies, test,
  secrets, logs), `test_automation_fix_hooks_webhook_lanes.py`, `test_automation_fix_hooks_webhook_secret.py`,
  `test_automation_hooks_programs.py`, `test_automation_fix_actions_programs.py`, `test_automation_r2c_programs.py`,
  `test_automation_r3c_programs.py`, `test_automation_hooks_filters.py`, `test_automation_fix_hooks_filter_cancel.py`,
  `test_automation_fix_hooks_filter_surrogate.py`, `test_automation_r3_lease_routing.py`, `test_automation_hooks_pipeline.py`.
- API/control: `test_automation_api_routes.py`, `test_automation_api_sse.py`, `test_automation_fix_api_sse_order.py`,
  `test_automation_fix_api_unicode.py`, `test_automation_fix_api_remote_filters.py`, `test_automation_fix_api_agent_prompts.py`,
  `test_automation_fix_api_not_found.py`, `test_automation_api_app_control.py`, `test_automation_api_end_to_end.py`,
  `test_automation_api_meeting_events.py`, `test_automation_r3_busy_and_state.py`, `test_automation_integration_fixes.py`.
- Channel: `test_automation_channel_protocol.py` (556 lines), `test_automation_channel_e2e.py` (690 lines, spawns the
  bridge process), `test_automation_fix_channel_gap.py`, `test_automation_fix_channel_link.py`,
  `test_automation_fix_channel_origin.py`.
- Pipeline integration (mostly a04 but pin this area's seams): `test_automation_foundation_pipeline.py`,
  `test_automation_fix_pipeline_control.py`, `test_automation_fix_pipeline_shaping.py`, `test_automation_fix2_pipeline.py`,
  `test_automation_r3_cancel_window.py`, `test_automation_r4_cancel_runtime.py`.
- MCP-side (a20, use this area): `test_automation_mcp_events.py`, `test_automation_mcp_tools.py`, `test_automation_fix_mcp_*`.

Golden-vector candidates (pure, deterministic): `routing.gate`/`plan` over the benchmark corpus and test sentences
(output: matched ids, spans, the questions JSON); `routing._alike`/`_sound`/`_words`; `RoutePlan.read` on canned
answers; `model.expand_link`/`link_scheme`/`_parse`/`validate_action` messages; `signing.signature/verify`;
`EventBus.since` gap logic as a sequence of publish/evict/clear ops; `catalog` builders; `filters._interpret`;
`webhooks._retry_after` (fixed now) and the `_exchange` status classification; `api.sse_frame`/`parse_types`/
`parse_last_event_id`; `channel_bridge.SseParser`, `clean_meta`, `channel_params`, `_reply_arguments`, JSON-RPC
dispatch transcripts; `agents.app_links`, `terminal_argv`, `_claude_result`, `_codex_answer`, `_failure`;
`launch._one_shot_script` text and `_terminal_commands`; `runtime.prompt_with_context`, `_short`.

## Porting notes

Suggested decomposition (namespace `fv`, Qt-free core where possible; the CLAUDE.md rule "Keep Qt out of src/engine"
suggests a new `src/automation` library that may use QtCore/QtNetwork but not Widgets):
1. `automation/catalog`, `signing`, `permissions`, `models` (hooks), `actions/model`: pure C++ + JSON. ~900 lines.
2. `automation/event_bus`: std threads, condition variable, replay buffer. ~350.
3. `actions/routing`: pure tokenizer/phonetics/plan + judge interface (`IJudge::ask(state, questions, timeout)`) with
   the timeout/cancel wrapper. ~900. Needs Unicode casefold/category: use ICU (vcpkg) or QtCore `QString`.
4. `platform/process` (shared with a20): spawn without shell/window, job object / process group, kill tree, disown,
   pipe threads with cancel. Then `automation/programs` (run_once + per-program runners). ~700.
5. `platform/launch`: Windows COM/Assoc, macOS ObjC++, Linux xdg; terminals and one-shot scripts. ~700.
6. `automation/http_client` (blocking POST with read cap, no redirects, proxy bypass for private hosts, error
   classification) used by `webhooks` (lanes, retries, records) and `filters`. ~1,100.
7. `actions/agents` (handoff, ask for Claude/Codex, OpenCode over a26's ACP client). ~900.
8. `actions/runtime` (Qt: QObject relay, pool, cancel state machine, output routing). ~800.
9. `automation/app_control` + `live_api` routes and SSE on a20's listener. ~900.
10. `channel_bridge` as its own console executable or a `--channel` mode that never initializes QApplication;
    stdin/stdout JSON-RPC, SSE client. ~900.
11. `meeting_events`, `service` glue. ~150.
Rough total: about 9,000 lines of C++ (plus tests).

Order: 1-2 first (everything publishes events), then 4 and 6 (shared infrastructure), then 3, 5, 7, 8, then the API
(9) once a20's listener exists, then 10. Routing can be ported and golden-tested early since it is pure.

Risks:
- Routing gate behaviour depends on Python's Unicode `\w`, `str.casefold`, `isupper`, `islower`; a C++ tokenizer that
  differs on apostrophes, hyphens or non-ASCII letters changes which dictations reach the judge. Replay the benchmark
  corpus and every pinned test sentence as golden vectors.
- The judge prompts and thresholds were tuned against Jev (TypeSafe) with a measured false-route margin; any reword or
  JSON shape change (key order included in the 20,000-char budget) needs a re-run of `benchmarks/voice_routing`.
- Process control: kill-the-tree on timeout/cancel but leave children alive after a clean exit, plus pipe threads that
  must not block on inherited handles, is subtle on Windows (job objects, `CancelSynchronousIo`); many regression
  tests pin it (`r2c_programs`, `r3c_programs`, `fix_actions_programs`).
- Explorer-hosted ShellExecute (environment and foreground rights) and the "no answer -> try nothing else" rule
  prevent double launches; easy to regress if replaced with `QDesktopServices::openUrl`.
- SSE over Qt: `QHttpServer` streaming is only in newer Qt; the listener choice belongs to a20 and blocks routes 9-10.
- Branding: event names, env vars (`OPENWHISPER_*`), credential names, header `X-OpenWhisper-Client`, MCP server name
  `openwhisper`, `~/OpenWhisper Agent Tasks`, and copy strings all say OpenWhisper. Users' scripts and existing
  secrets depend on these; renaming to OpenWhisper_CPP is an owner decision.

Native gains: no import cost for the channel bridge (the Python one hand-wrote MCP to avoid 0.5 s of SDK imports; a
C++ binary answers in milliseconds and can drop `PipeLineReader`'s polling by using overlapped I/O); routing gate in
microseconds; no GIL, so the dispatch thread, lanes and pool are truly parallel; HTTP clients can be created once
without TLS warm-up threads; smaller memory than per-lane httpx clients.
