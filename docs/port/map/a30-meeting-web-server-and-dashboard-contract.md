# Meeting web server (HTTP and WebSocket) and its contract with the React dashboard

Source root: `D:\coding\whisper_local`. Files: `meeting/web/{__init__,api,archive,auth,server,ws}.py`
(2,072 lines of Python) and `webui/src/{App.tsx,api.ts,main.tsx,playback.ts,report.ts,state.ts,theme.ts,types.ts,ws.ts}`
(2,591 lines of TypeScript, the dashboard's shell plus its wire contract).

**Gap in `areas.json`:** the rest of the dashboard is not assigned to any area. That is
`webui/src/components/**` (about 50 components plus 13 CSS files), `webui/src/styles.css` (4,041 lines),
`webui/src/themes/{palettes,designs}.css`, the helpers `chapters.ts corrections.ts dashboardActions.ts
evidence.tsx markdown.ts people.ts print.ts scroll.ts titles.ts useTranscriptWindow.ts`, and
`webui/tests/*.cjs` (22 node tests). Together that is roughly 13k lines of TSX and CSS. The
recommended plan below avoids porting it at all, but the owner has to confirm that (Open questions).

## What the user gets

- When a meeting starts, the app runs a small web server on its own thread: `127.0.0.1` by default,
  or `0.0.0.0` when the user opts into LAN sharing. The port is ephemeral by default. The server
  produces two capability links: a **host link** `http://<host>:<port>/m/<host_token>` and a
  **guest link** `/m/<guest_token>`. The desktop app opens the host link in the browser and shows or
  copies both.
- The browser loads a React single-page dashboard. It shows the live transcript, live notes,
  captured cards (decisions, actions, risks, and so on), participants, the topic and summary, the
  highlight ruler, a recording player, and agent activity (host only). After the meeting it shows
  report views (Ribbon, Brief, Signal, Editorial, Handoff), custom reports, and insight review.
  The host also gets a History pane that browses, searches, renames, deletes, exports and re-runs
  past meetings.
- A guest must enter a display name (JoinGate) and is added as a participant. Join and leave
  presence is broadcast to everyone. Guests can add notes and edit cards (store authorization
  decides what is allowed). Guests can never see custom reports, agent activity, history, search
  or exports, and never see the host token.
- Live updates arrive over a WebSocket at `/ws`. Every reconnect sends a full `hello` snapshot,
  followed by incremental `patch`, `segments`, `speech_preview`, `status`, `presence`,
  `voice_feedback`, `agent_activity` and `meeting_ended` messages. Every client edit is
  acknowledged with an `action_result`.
- The host can end, pause and resume the meeting, toggle AI insights (this needs the desktop
  consent first), rotate both links (old sockets are closed), request a note adjustment, and
  request, delete and download tailored reports.
- After an app restart, opening a past meeting starts an **archive dashboard**: the same server
  over a read and edit adapter, with no capture, ASR or agent running
  (`meeting/web/archive.py`).

## Feature inventory

Status legend: **port** = port as is. **undecided** = depends on a backend whose fate is not
decided yet (cloud LLM, OpenAI speaker ID, remote engine). **drop** = engine-specific.

| ID | Feature | Exact user-visible behavior | Source | Status |
|---|---|---|---|---|
| A30-01 | Threaded server lifecycle | Runs on a daemon thread named `MeetingWebServer` with its own event loop. `start()` blocks until the listener is bound, polling every 50 ms with a **15 s** startup timeout. If the thread dies first it raises `RuntimeError("Meeting web server failed to start")`; on timeout it raises `TimeoutError("Meeting web server startup timed out")`. A second `start()` while running returns the same base URL. `stop()` sets `should_exit`, joins for up to **10 s** (logs a warning if it overruns), and gives open responses **1 s** grace (`GRACEFUL_SHUTDOWN_S`), because the audio player keeps a streaming response open. Caller wording: `"Meeting dashboard could not start: {exc}"` (engine.py:1871). | server.py:MeetingWebServer.start/_run/stop, engine.py:_start_server | port |
| A30-02 | Bind and advertised host | `bind` is `"localhost"` (listen on 127.0.0.1) or `"lan"` (listen on 0.0.0.0). Any other value is treated as localhost. Port 0 means OS-chosen; the bound port is read back from the socket. The advertised host is always `127.0.0.1` for localhost. For LAN it is the result of `services.lan_address.discover_lan_ipv4()`, which prefers a LAN address over a VPN address and never picks a Docker or VM bridge; if that fails it warns `"Could not resolve a usable LAN address; using 127.0.0.1"`. | server.py:_display_host, discover_lan_ipv4 | port |
| A30-03 | Capability URLs | `base_url = http://{host}:{port}`. `host_url` and `guest_url` are `{base}/m/{token}`, read fresh from the meeting row on every access. They are `""` before start, when there is no meeting, when the token is missing, or when the repo raises. | server.py:host_url/guest_url/_tokenized_url | port |
| A30-04 | Token generation and role check | Each token is `secrets.token_urlsafe(32)`: 32 random bytes, base64url, no padding, 43 characters. Tokens come in host and guest pairs. `resolve_role` uses a constant-time compare and checks the host token first, so if both tokens are equal the result is host. Empty, `None` or non-string tokens give `None`. | auth.py | port |
| A30-05 | Dashboard page | `GET /m/{token}`: if the token does not resolve against the **current** meeting, returns 403 `{"detail":"invalid or expired link"}`. Otherwise serves `webui/dist/index.html` as `text/html`. If that file is missing, serves 200 with the HTML notice "The dashboard bundle is missing; run `npm run build --prefix webui`." (page title `OpenWhisper Meeting`). | api.py:_register_page_routes, _MISSING_BUNDLE_PAGE | port |
| A30-06 | Static assets | `/assets/*` is served from `webui/dist/assets`, and only mounted if that directory exists. `dist/licenses/*` is **not** served. | api.py:create_app | port |
| A30-07 | Session bootstrap | `GET /api/session?token=` returns `{role, meeting: public, state, voice_commands}`. `state` is the live `store.snapshot()`, or `{}` when there is no store. For a non-host, the `custom_reports` key is **removed** (not emptied). A bad token gives 401 `"invalid token"`. | api.py:api_session | port |
| A30-08 | Live transcript paging | `GET /api/transcript?token&cursor&limit=500` takes any role. The limit is clamped to 1..1000. Keyset order is (`start_s`, `id`). It fetches `limit+1` rows to compute `next_cursor`. With no meeting it returns `{"items":[],"next_cursor":null}`. A bad cursor gives 400 `"invalid transcript cursor"`. | api.py:transcript_page, _encode_cursor/_decode_cursor | port |
| A30-09 | History list | `GET /api/meetings` is host only. Parameters: `cursor`, `limit=50` (clamped 1..100), `q` (literal match, so `%` and `_` are not wildcards; tested), `since` (started after), and the flags `has_decisions`, `has_actions`, `needs_attention`. It includes running meetings. The digest filters apply **after** a bounded page, so a filtered page can be empty while still returning a `next_cursor`. The cursor is base64url of `[started_at, id]`. A bad cursor gives 400 `"invalid meeting cursor"`, as does a cursor longer than **1024** characters. | api.py:api_meetings, _filter_history_page, _history_cursor/_decode_history_cursor | port |
| A30-10 | Public meeting projection | Only these keys go out: `id,title,status,started_at,ended_at,paused_total_s,cloud_enabled,asr_model`. Added to them: `display_title` (falls back to the topic), `duration_s` (wall time minus pauses, `null` if unreadable; running when status is in {active, paused, ending}), `digest` (omitted on a corrupt snapshot), the compact finalization fields `finalization_status`, `finalization_deferred`, `insights_pill`, `insights_tone`, and, when a repo is passed, `content_summary` plus `has_audio`, `has_transcript`, `can_rerun_speakers`. **Never** sent: tokens, `state_json`, `steps`. | api.py:_public_meeting, _meeting_digest | port |
| A30-11 | History digest | Counts of non-removed `decisions`, `action_items` and `risks`, plus `open_questions` (questions may be a list or a dict). People are sorted with `kind=="me"` first, then `created_at`, then `id`. At most **6** people go in `participants`; the full count is in `participant_count`. | api.py:_meeting_digest | port |
| A30-12 | Meeting detail and state poll | `GET /api/meetings/{id}?include_transcript=1` is host only and returns `{meeting, state, segments, transcript_next_cursor, transcript_included}`. With `include_transcript=0` it does no transcript query. `GET /api/meetings/{id}/state` returns only `{state}` and must not call content-summary or segment queries. An unknown id gives 404 `"unknown meeting"`. Where the state comes from: the live store if this is the current meeting, else a cached review store, else the persisted state upgraded to the current wire shape (historical normalization turns interrupted `running` work into `failed`). | api.py:state_for, _stored_state | port |
| A30-13 | Per-meeting transcript and segment | `GET /api/meetings/{id}/transcript` and `GET /api/meetings/{id}/segments/{sid}`: hosts can read any meeting; **guests only the current meeting** (403 `"guest access is current meeting only"`). An unknown segment gives 404 `"unknown segment"`. | api.py:_register_meeting_file_routes, require_meeting | port |
| A30-14 | Recording playback | `GET /api/meetings/{id}/audio` builds a WAV through `meeting.audio_playback.build_playback`. A missing file gives 404 with the exception text; a ValueError gives 400. Response headers: `audio/wav`, `Content-Disposition: inline; filename="meeting-{id}.wav"`, `Cache-Control: no-store`. **HTTP Range is supported** (206 is tested). The client appends a `revision` query parameter as a cache buster. | api.py:api_meeting_audio | port |
| A30-15 | Rename meeting | `POST /api/meetings/{id}/rename {title}` is host only. `normalize_title` failure gives 400 with its message. The write goes to the live engine (`apply_client_action("host", None, set_title)`) if the meeting is current, else to the cached review store, else to `repository.rename_meeting`. Returns `{ok,title}`. | api.py:retitle_saved_meeting | port |
| A30-16 | Insight review | `POST /api/meetings/{id}/review {op}` is host only. `op` must be one of `start, review_answer, review_skip, review_reopen`, else 400 `"invalid review action"`. If the meeting is busy it returns 409 `"Wait for final insights to finish"` or `"A meeting update is already running"`. `start` calls `insight_review.start_review`; the others call `store.apply("host",None,[body])`. Returns `{ok,error?,state}`. | api.py:api_insight_review | undecided (cloud LLM) |
| A30-17 | Re-run insights | `POST /api/meetings/{id}/reinsights` is host only. On the active meeting it returns 409 `"cannot re-run insights on the active meeting"`. A double click gives 409 `"insights are already running for this meeting"`. It also refuses while other work is busy (A30-38). The job `rerun_finalization(from_step="failed", model_lease=engine.model_lease, **rerun_options(meeting))` runs on a **single-thread executor** so it cannot starve the shared pool. `FinalizationBusyError` maps to 409 and `ValueError` to 400 with the message. The resulting `state` is adopted by the live or cached store. | api.py:_register_insights_rerun_route, run_post_meeting_pass | undecided (cloud LLM; redecode uses the ASR model lease, so adapt to Parakeet) |
| A30-18 | Re-run speakers | `POST /api/meetings/{id}/respeakers` is host only. On the active meeting: 409 `"cannot re-run speakers on the active meeting"`. With no loopback recording: 400 `"no system-audio recording is available for speaker identification"`. If the gate `resolve_speaker_pass()` fails it returns 400 with the gate reason; the tested strings are `"Speaker identification is not set to OpenAI."`, `"Audio-upload consent has not been given."` and `"No OpenAI API key is configured."`. If already running: 409 `"a post-meeting pass is already running for this meeting"`, or `"post-meeting steps are already running for this meeting"` from refinalize. | api.py:_register_speakers_rerun_route | undecided (OpenAI) |
| A30-19 | Delete meeting | `DELETE /api/meetings/{id}` is host only. On the active meeting: 409 `"cannot delete the active meeting"`. While a review or insights run is in flight: 409 `"Wait for the meeting update to finish before deleting"`. It also refuses while other work is busy. It removes the cached review store and calls `delete_meeting_data(repo, id, spool_root or config.MEETINGS_FOLDER)`. | api.py:api_delete_meeting | port |
| A30-20 | Search | `GET /api/search?q&mode=keyword|semantic` is host only; any other mode gives 400 `"invalid search mode"`. `keyword` returns `{results: repo.search_transcripts(q), mode:"keyword", message:""}`. `semantic` returns `meeting.semantic_search.search_history(repo,q,semantic=True)`. | api.py:api_search | keyword: port; semantic: undecided |
| A30-21 | Audit events | `GET /api/events?before_seq&limit=100` is host only and covers the current meeting only (`{"events":[]}` when there is none). Rows carry `undoable`. | api.py:api_events | port |
| A30-22 | Export | `GET /api/export/{md|json|txt}?meeting_id=` is host only. An unknown format gives 404 `"unknown export format"`. With no meeting id and no current meeting: 404 `"no meeting"`. Returns `Content-Disposition: attachment; filename="meeting-{id}.{ext}"`. The exporter input is `{**meeting, **public}`. The body never contains tokens (tested for all three formats). | api.py:_register_export_routes | port |
| A30-23 | Custom reports | `POST /api/meetings/{id}/reports {request}` is host only. A request that is not a string, is blank, or is longer than `MAX_REQUEST_CHARS` gives 400 "Describe the report you want, in 1 to {N} characters." While an insights run is busy: 409 `"Wait for the meeting's insights to finish"`. Rejections from `start_custom_report` map to 409 through the `_REPORT_REJECTIONS` table (default text `"The report could not be started."`). The provider and model come from `_report_endpoint`: the meeting's recorded endpoint wins, an installed agent core uses its own, and the fallback is `openrouter`. `DELETE .../reports/{rid}` gives 404 for `unknown_report` and 409 for anything else. `GET .../reports/{rid}/download` returns `text/markdown`, attachment `report-{rid}.md`, 409 `"report is not ready"`, and 404 `"unknown report"`. | api.py:_register_report_routes | undecided (cloud LLM) |
| A30-24 | Note adjustment request | `POST /api/meeting/notes/request {text}` accepts **any role**. Text must be 1..4000 characters (`_NOTE_REQUEST_MAX_CHARS`), else 400 "Enter a note request of 1 to 4000 characters.". It calls `engine.request_note_adjustment` (ValueError maps to 400, RuntimeError to 409) and awaits the returned Future without blocking a worker thread. Returns `{ok,applied,rejected,error}`. The archive dashboard raises "Turn on AI insights in an active meeting first.". | api.py:_register_note_routes | undecided (cloud LLM) |
| A30-25 | End, pause, resume | `POST /api/meeting/end`, `/pause`, `/resume` are host only and call `engine.end()`, `pause()` and `resume()` on a worker thread; each returns `{ok:true}`. An End that takes longer than **1.0 s** logs `"Meeting end request took %.2fs (%.2fs waiting for a worker thread, %.2fs in engine.end)"`. | api.py:api_meeting_end, _log_slow_end | port |
| A30-26 | AI insights toggle | `POST /api/meeting/cloud {enabled}` is host only. Enabling without the desktop consent gives 403 "Turn on AI insights once from the desktop app first; it asks for consent before meeting text is sent to the AI provider.". Disabling is always allowed. Calls `engine.set_cloud_enabled` and persists setting `meeting_cloud_last_enabled`. | api.py:api_meeting_cloud | undecided (cloud LLM) |
| A30-27 | Rotate links | `POST /api/meeting/tokens/regenerate` is host only and returns `{ok, host_url, guest_url}`. The engine or archive writes new tokens and then calls `server.invalidate_connections()`. The hub sleeps **0.25 s** (so the REST reply reaches the client) and then closes every socket with **4401**. | api.py:api_regenerate_tokens, ws.py:_invalidate_connections, archive.py:regenerate_tokens, engine.py:~2304-2319 | port |
| A30-28 | WS admission | `GET /ws?token&name&guest_id`. The socket is accepted first, then checked. No current meeting or no store: close 4401. Bad token: close 4401. A guest without a name: close **4400**. More than **32** sockets: close **4429**. A handshake exception: close 1011. A normal end: close 1000. | ws.py:handle_connection, _admission_refusal | port |
| A30-29 | WS hello | Keys: `type:"hello"`, `voice_commands`, `role`, `participant_id` (the host gets the `kind=="me"` participant), `seq`=state.seq, `state` (guests lose the `custom_reports` key), `segments` (the last **200** segments, chronological; `[]` on error), `urls:{guest}` (absolute URL, falling back to `/m/{guest_token}`), and `meeting:{id,title,started_at,status}` where title and status prefer the state values. Hosts also get `agent_activity`, the last **50** ticks oldest first; the key is **omitted** for guests. The host token never appears; the guest token appears only inside `urls.guest`. | ws.py:_send_hello | port |
| A30-30 | No lost patch around hello | The socket is registered **before** the snapshot is read. Broadcasts that arrive while hello is being built are buffered, up to **512**, and flushed right after hello. If the buffer overflows it is cleared and the socket is closed with **4409** (the client then resyncs). There is no await between the end of the flush and `ready=True`. Host-only messages are *not* buffered for sockets that are not ready yet. | ws.py:broadcast_json, _flush_pending | port |
| A30-31 | Broadcast fan-out | JSON is encoded once (`ensure_ascii=False`, `default=str`) and sent concurrently to every ready socket, each with a **5 s** timeout. After **3** consecutive timeouts the socket is dropped with 1011; any other send error drops it immediately. Thread-safe entry is `schedule_broadcast`, which does nothing when the loop is not running. | ws.py:schedule_broadcast, _send_guarded, _drop | port |
| A30-32 | Store patch stream | The hub subscribes to `engine.store` (re-checked on every connect, because the store can be created after the server starts). Each applied batch becomes `{"type":"patch","seq",results:[{op,target_id,effect,seq}]}`. Results whose `effect.entity` is in `HOST_ONLY_ENTITIES={"custom_report"}` are removed from the shared patch, and a second **host-only** patch is sent carrying *all* rows. A batch made only of reports is sent to hosts only. | ws.py:_on_store_batch, _ensure_store_subscription | port |
| A30-33 | Guest identity and presence | `guest_id` must match `^[A-Za-z0-9_-]{1,64}$`. A reconnect with the same `guest_id` **and the same name** reuses the participant; a changed name or unknown key calls `engine.add_guest(name)` again. After hello, `{"type":"presence","event":"joined",participant}` goes to everyone, including the joiner. On disconnect, `left` is sent with the freshest participant data. | ws.py:_join_guest, _serve, _announce_left | port |
| A30-34 | WS receive loop hardening | A binary frame gives error `binary_unsupported` ("Binary frames are not supported."). A frame over **65,536** characters gives `message_too_large` ("Message exceeds the size limit."), checked before parsing. Invalid JSON or JSON nested too deep gives `bad_json` ("Message was not valid JSON."). A non-object gives `bad_message` ("Message must be a JSON object."). An unknown type gives `unknown_type` ("Unknown message type: 'x'", Python repr quoting). A handler crash gives `internal_error` ("The server could not process that message."). None of these closes the socket. | ws.py:_receive_loop, _dispatch | port |
| A30-35 | WS ping, action and undo | `ping` is answered with `pong`. `action {client_action_id, op}`: a non-dict op gives reason `malformed_op`; more than **4** in flight gives `too_many_in_flight`; otherwise `engine.apply_client_action(actor, participant_id, op)` with actor `"host"` or `"user"`. `undo {seq}`: guests get `host_only`; a seq that is not an int in 0..2^63-1 (including `1e400` and 10^40) gives `invalid_seq`. The reply is always `{"type":"action_result",client_action_id,results:[{ok,reason,target_id,seq,effect}]}` and is **not** re-broadcast, because the store subscription produces the patch. | ws.py:_handle_action, _handle_undo, _coerce_seq | port |
| A30-36 | Engine push messages | The engine calls `server.broadcast(msg, host_only=?)` with `status` (status, intelligence_online, diarization_available/reason, capture, finalization, live_highlights_status, speech), `segments` (`items`, optional `removed_ids`), `speech_preview` (channel, text, start_s, end_s, final), `meeting_ended` (status; the engine never sends a `meeting_end` type), `voice_feedback` (seq, phase, message, command), and `agent_activity` (host only: kind, label, tool, pass_kind, ts). | engine.py:_broadcast and its call sites (lines 366, 527, 954, 1166, 1213, 1626, 1654, 1665, 2057, 2667) | port (shape); the `speech.source:"remote"` part is undecided |
| A30-37 | Cross-thread control | `retitle_saved_meeting(id,title)` runs the async retitle on the server loop and waits up to **15 s**; if the loop is down it raises "Meeting dashboard is unavailable.". `refresh_saved_meeting_title(id)` refreshes the live and cached stores; if the live title changed it closes every socket with **4409** (a resync, which keeps the token valid). `invalidate_connections()` closes with 4401. | server.py, api.py:refresh_saved_meeting_title, services/runtime/meeting.py:529-551 | port |
| A30-38 | Single-writer guards | `insights_running` is a set of meeting ids; check and claim happen with no await in between. `review_stores` caches one historical store per past meeting, so that reports, reviews and renames share one writer. `_refuse_rerun_while_busy` returns 409 with "Wait for the post-meeting steps to finish", "Wait for insight review to finish" or "Wait for the report being written to finish". Re-runs and delete drop the cached store. | api.py:_AppContext, _refuse_rerun_while_busy | port |
| A30-39 | Archive dashboard | An engine-shaped adapter over a persisted meeting. It opens a store with `historical=True` (in-flight work is shown as failed or interrupted). `is_active()` is False. `end`, `pause` and `resume` do nothing. A note request raises RuntimeError "Turn on AI insights in an active meeting first." (the route maps it to 409); the cloud toggle raises RuntimeError "AI insights are unavailable for archived playback", which `/api/meeting/cloud` does **not** catch, so the client gets a bare HTTP 500 (a C++ port may return a clean 409 instead; say so if it deviates). `add_guest` raises ValueError when the store rejects the participant, which fails a guest handshake with close 1011. `add_guest` trims the name to 80 characters, or uses "Guest". Link rotation writes the repo and invalidates sockets. `shutdown()` stops the server. LLM options: an installed agent core uses its own model; otherwise the meeting's `agent_provider`, `agent_model` and stored endpoint win. | archive.py:ArchivedMeetingDashboard | port (LLM options undecided) |
| A30-40 | Dashboard boot (client) | The token comes from `/m/{token}` (URL-decoded) or from `?token=`. Screens and their strings: "Missing meeting token." / "Open the link shared by the host."; "Loading meeting…"; "Unable to join meeting" plus the error; the guest JoinGate; "Connecting…"; "Meeting link expired" or "Unable to join meeting" plus `socketStatusMessage`. Supported query parameters: `?history=<id>` (host opens History on that meeting), `?view=room` (Room display), and `?view=report` (added by the runtime; read by `components/HistoryPane.tsx`, not by App.tsx). `App.tsx` itself reads only `token`, `history` and `view=room`. | App.tsx:App, extractToken | port (reuse bundle) |
| A30-41 | REST client (client) | Every call puts the token in `?token=`. Errors become `ApiError(status, detail)`, where detail is `body.detail`, else `body.message`, else the raw body, else `HTTP {status}`. List responses may be bare or wrapped (`meetings`, `items` or `events`). Flags are sent as `1`; `include_transcript` as `0` or `1`. | api.ts | port (reuse bundle) |
| A30-42 | Socket client (client) | URL `ws[s]://{host}/ws?token[&name&guest_id]`. Ping every **20 s**. Reconnect backoff is `min(15000, 1000*2^min(attempt,6)) + rand*400` ms. Close codes 4400 and 4401 are terminal (no reconnect); everything else, including **4409** and 4429, reconnects. An unacknowledged action times out after **15 s**. `guest_id` lives in `sessionStorage['ow_meeting_guest_id']`. Strings: "This meeting link has expired. Ask the host for a new link.", "A display name is required. Reload the page and join again.", "Reconnecting to the meeting…", "You are offline. Your change was not sent.", "The server did not acknowledge the change. Check whether it applied before retrying.", "Connection interrupted before the server acknowledged the change.". | ws.ts | port (reuse bundle) |
| A30-43 | Client state reducer | Applies `hello`, `patch` (skips results with seq <= doc.seq), `action_result` (optimistic, never advances seq; reasons are shown with `_` replaced by a space), `segments`, `speech_preview` (keyed per channel, monotonic `end_s`), `voice_feedback` (monotonic seq; ignored when paused or ended), `presence`, `status`, `agent_activity` (newest first, cap **50**), `error`, and `meeting_ended`. Terminal statuses are `ended`, `failed` and `needs_recovery`. | state.ts:meetingReducer, applyEffect | port (reuse bundle) |
| A30-44 | Report views (client) | Agent views `ribbon`, `brief` and `signal` come from `state.report_views` (unknown names are dropped, and an empty or missing list becomes just `ribbon`); `editorial` and `handoff` are always appended. The stored choice is used only if still enabled, else Ribbon, else the first view. The choice is remembered in `localStorage['ow_report_view']`. Labels, hints and notes are in `REPORT_VIEW_META`. | report.ts | port (reuse bundle) |
| A30-45 | Themes (client) | `localStorage['openwhisper.meeting.theme']` is `light` or `dark`; the third choice `system` removes the key and removes `<html data-theme>` so CSS `prefers-color-scheme` decides. Otherwise it sets `<html data-theme>`. The design theme is stored in `openwhisper.meeting.design`, one of `studio` (default), `sunlit`, `sage`, `focus` or `original`, and sets `<html data-design>`. Both are applied before the first paint. | theme.ts, main.tsx | port (reuse bundle) |
| A30-46 | Playback helpers (client) | `playMoment`: the last click wins, it waits for `loadedmetadata`, and it clamps to the duration. `useAudioClock` and `usePlayingSpanId` listen to the audio element's events. `spanAt` uses a minimum span of **0.4 s** and picks the latest-starting overlapping span. | playback.ts | port (reuse bundle) |
| A30-47 | Error envelope | Every handled HTTP error is `{"detail": "<string>"}`. FastAPI also produces 422 `{"detail":[...]}` when query parameters fail validation (for example `limit=abc`), 404 `{"detail":"Not Found"}` for unknown paths, and 405 for a wrong method. The client relies only on `detail` being a string, or falls back to the raw body. | FastAPI convention | port |
| A30-48 | Docs disabled | FastAPI `title="OpenWhisper Meeting"`, with `docs_url`, `redoc_url` and `openapi_url` all `None`. No `/docs` routes are exposed. | api.py:create_app | port (nothing to build) |

### Added by check

- **Missing token.** `token` defaults to `""` on every route, so a missing token is 401 `"invalid token"`, not 422. Only `/m/{token}` answers 403 (`"invalid or expired link"`). There is no CORS middleware and no cookies; the token is the only credential.
- **Report rejection wording (A30-23).** `_REPORT_REJECTIONS`: `report_running` "A report is already being written for this meeting."; `meeting_not_ready` "Reports are written once the meeting has ended and its insights have finished."; `report_limit_reached` "This meeting is holding the maximum number of reports. Delete one to make room."; `invalid_request` "Describe the report you want."; `request_too_long` "That request is too long."; `host_only` "Only the meeting host can request a report."; `persistence_error` "The report could not be saved. Retry."; anything else "The report could not be started.". The success body is `start_custom_report`'s dict plus `state`. Check order: host, JSON body (400), request text (400), meeting exists (404), insights busy (409), then the 409 rejection table. `DELETE .../reports/{rid}` applies op `remove_custom_report` through the owning store and uses the op's `reason` string as `detail`. The download is `{markdown or ""}` for a report whose `status` is `ready`.
- **AI insights toggle coercion (A30-26).** `enabled = bool(body.get("enabled"))`, so any truthy JSON value, including the string `"false"`, enables. Consent is checked only when enabling. The reply is `{ok:true, enabled}`.
- **Events paging (A30-21).** `before_seq` is an optional int (422 if not an int) and `limit` (default 100) is passed to the repository **unclamped**. The dashboard client sends only `before_seq`.
- **Export (A30-22).** The target meeting must exist, else 404 `"unknown meeting"`, after the format and no-meeting checks. State comes from the same `state_for` order as A30-12, segments from `repository.get_segments`. Media types: `text/markdown`, `application/json`, `text/plain`.
- **Session and hello carry `voice_commands`** from `meeting.voice_help.voice_command_guide()` (a29): `{names[], primary, examples[{label, phrase}]}`. It is built on a worker thread per request.
- **History digest row shape (A30-11).** Each of the up-to-6 `participants` is `{id, display_name, kind, created_at}`, all strings (`""` when missing); participants without an `id` are skipped.
- **History row projection (A30-09/10).** List rows use `_public_meeting(row)` with **no** repository, so `content_summary`, `has_audio`, `has_transcript` and `can_rerun_speakers` appear in list rows only if the row already carries `content_summary`. Detail (`/api/meetings/{id}`) passes the repository and always includes them. The needs-attention filter is `insights_tone == "warning"`; has-decisions and has-actions are `digest.decisions > 0` and `digest.action_items > 0`.
- **Guest participant memory (A30-33).** The hub keeps `guest_id -> (participant_id, name)` in memory for its lifetime. It is **not** cleared when links are rotated or sockets invalidated. The `left` presence carries `current or participant`, so it still goes out if the participant vanished from the state.
- **Host patch duplication (A30-32).** A batch containing report effects goes to guests as the filtered patch and to hosts as **two** patches (the shared one, then the full host-only one). Clients rely on per-result `seq` to ignore the repeat.
- **Rejection reasons in `action_result` (A30-35).** Local reasons are `malformed_op`, `too_many_in_flight`, `host_only`, `invalid_seq` and `internal_error` (an exception in `apply_client_action` or `undo`). `client_action_id` is echoed as sent, even when absent or not a string. Actor id is the connection's `participant_id`.
- **Undo.** `engine.undo(seq, participant_id)` on a worker thread. It shares the 4-in-flight cap with actions. The archive adapter forwards it to `store.undo`.
- **Token redaction in logs.** The runtime logs only the origin of dashboard URLs (`services/runtime/meeting.py:redact_meeting_url`; tokens grant authority and log files get attached to bug reports). The C++ port must never log a full capability URL or a token. A malformed URL logs `<redacted>`.
- **Static file conditional behavior (A30-05/06).** `FileResponse` and `StaticFiles` also send `ETag`, `Last-Modified` and answer conditional requests with 304 and byte ranges with 206. The MIME type comes from the file extension (`.woff2`, `.js`, `.css`). A missing asset is a 404 from Starlette's `StaticFiles` (record its exact body from the Python server).
- **Report views setting.** `state.report_views` is set from `resolve_meeting_report_views`, which reads the bool settings `meeting_report_ribbon`, `meeting_report_brief` and `meeting_report_signal` (a10). If all are off it falls back to `("ribbon",)`.
- **Client strings and defaults not listed above (A30-41/42/43).** Rejecting pending actions on a deliberate close: "The meeting connection was closed before the change was acknowledged.". A rejected action with no reason shows "Action was rejected". An `error` message shows `message` or `code`. Unparseable inbound frames are ignored. The WebSocket URL uses `wss` on https pages and `location.host`. `guest_id` is a random action id stripped to `[A-Za-z0-9_-]`, cut to 64 characters. Client page sizes: transcript 500, History 50. `socket_status` other than `open` clears `voiceFeedback` and `speechPreviews`. A `status` message replaces `finalization` only if the key is present.- **Design theme labels (A30-45).** `studio` "Studio" (Forest ink & soft peach), `sunlit` "Sunlit" (Warm paper & garden green), `sage` "Sage" (Quiet greens & a dark bar), `focus` "Focus Stage" (Airy sage & centered type), `original` "Original" (Classic charcoal & blue).
- **Report derived-view rules (A30-44).** `liveItems` drops `removed` items and prefixes text with "Needs verification: " (review state `unsupported`) or "Provisional: " (`provisional`), appends " — Due: {deadline|due_date}" for action items, and " (Advisory: {citation})". Signal headline: first live decision clipped to 14 words, else `topic.current`, else the title, else "Meeting recap". Standfirst: the first two sentences of `rolling_summary`. Listen picks: first cited segment of the top decision, the highest-severity risk (high, medium, low) and the first action, de-duplicated. `speakerName` falls back to "Me" (mic), "Others" (loopback) or "Unknown". `clock` is `M:SS`. These matter only if the bundle is rewritten.

## Modules

### meeting/web/__init__.py
Re-exports `MeetingWebServer`. The docstring states the security model: plain HTTP, localhost by
default, LAN only on explicit opt-in, and capability tokens.

### meeting/web/auth.py
- Public surface: `ROLE_HOST`, `ROLE_GUEST`, `TOKEN_BYTES=32`, `generate_token()`,
  `generate_token_pair()`, `resolve_role(token, host, guest)`.
- `_matches` uses `hmac.compare_digest` on UTF-8 bytes and returns False when the expected token is
  empty (auth.py:41-47). The host token is checked first (auth.py:64).
- Called by `api._AppContext.resolve`, `ws.handle_connection`, `archive.regenerate_tokens`,
  `engine.regenerate_tokens` (engine.py:2304), and `services/remote_records/kinds.py:683`.

### meeting/web/server.py
- `MeetingWebServer(engine, repository, bind="localhost", port=0)`. Properties: `app`,
  `base_url`, `host_url`, `guest_url`. Methods: `start()`, `stop()`, `is_running()`,
  `broadcast(msg, host_only=False)`, `retitle_saved_meeting`, `refresh_saved_meeting_title`,
  `invalidate_connections`.
- Threading: a uvicorn server runs on a daemon thread with a fresh asyncio loop. Signal handlers are
  never installed. `quiet_connection_lost(loop)` (from services/asyncio_utils, area a01) silences
  Windows Proactor `ConnectionResetError` noise. `log_config=None` and `access_log=False` are set
  because a windowed frozen build has no stdout (tested in test_meeting_web_server_logging.py).
- It wires `hub.get_guest_url = lambda: self.guest_url` (server.py:73).
- Constants: `STARTUP_TIMEOUT_S=15.0`, `STOP_JOIN_TIMEOUT_S=10.0`, `GRACEFUL_SHUTDOWN_S=1.0`; the
  cross-thread call timeout is 15 s (server.py:253, 262).
- Edge cases: if the bound port cannot be read, it falls back to the configured port
  (server.py:192-198); `stop()` is idempotent; `refresh_saved_meeting_title` does nothing when
  the loop is down (server.py:257).
- Import error text when uvicorn is missing: "Meeting Mode needs the dashboard dependencies..."
  (server.py:21-25). This has no meaning in C++; drop it.

### meeting/web/api.py
- `create_app(engine, repository, hub)` registers routes in a fixed order (api.py:607-619; the order
  matters for FastAPI path matching, for example `/api/meetings/{id}/state` versus
  `/api/meetings/{id}`). The full route table is in Data and formats.
- `_AppContext` holds the shared state: `insights_executor` (a ThreadPoolExecutor with max_workers=1,
  thread prefix `meeting-reinsights`), `insights_running: set`, and `review_stores: dict`. Helpers:
  `is_current`, `is_active_meeting`, `live_store`, `meeting_store`, `owning_store`,
  `current_meeting`, `resolve`, `require(token, host_only)` (401 `"invalid token"`, 403 `"host only"`),
  `require_meeting`, `saved_meeting`, `transcript_page`, `state_for`, `retitle_saved_meeting`,
  `refresh_saved_meeting_title`, `run_post_meeting_pass`.
- Async model: every blocking repo, store or engine call goes through `asyncio.to_thread`, so it
  runs on the loop's default pool; only re-runs use the dedicated executor. The note request awaits
  a `concurrent.futures.Future` with `asyncio.shield(wrap_future)` (api.py:1082), so a client
  disconnect does not cancel the scheduler job.
- Lifespan: on startup `hub.on_startup()`; on shutdown `hub.on_shutdown()` and
  `insights_executor.shutdown(wait=False)`.
- State machine: per meeting id, at most one post-meeting pass is claimed in `insights_running`.
  The review route claims the same set (api.py:793-804). Re-run and delete check
  `_refuse_rerun_while_busy` against `finalization.status=="running"`,
  `insight_review.status=="running"`, or any `custom_reports[].status=="running"`.
- Edge cases a port must keep:
  - Cursor decoders reject every kind of malformed input with 400: bad base64, invalid UTF-8,
    wrong JSON type or shape, recursion depth, and float overflow (api.py:109-138; table in
    test_meeting_web_history.py:102-142).
  - The history cursor may be at most 1024 characters (api.py:93, 113).
  - `_meeting_digest` never raises (api.py:221-224). `_public_meeting({})` returns `{}`.
  - The filtered history page still returns a continuation cursor (api.py:687-695).
  - Reinsights computes `rerun_options` *before* the check-and-claim; there is no await between the
    check and the claim (api.py:830-840).
  - Export merges `{**meeting, **public}` (api.py:968); the exporters strip tokens themselves.
  - Rename on a meeting that is neither live nor cached needs `saved_meeting` to give 404 first
    (api.py:411).

### Added by check (modules and wire types)

- **server.py:** uvicorn runs with `log_level="warning"`, `access_log=False`, `lifespan="on"` and `timeout_graceful_shutdown=1.0`. `port` is `int(port or 0)`. `stop()` clears `base_url`, so `host_url` and `guest_url` become `""` afterwards. `broadcast` copies the dict before queuing. `retitle_saved_meeting` returns `bool(result["ok"])`. `_ThreadedUvicornServer.install_signal_handlers` is a no-op.
- **ws.py hub internals:** `on_startup` captures the loop and subscribes to `engine.store`; `handle_connection` also captures the loop if startup has not. `_ensure_store_subscription` unsubscribes from a stale store when `engine.store` changes. Handshake failure after registration pops the socket and closes 1011. A failed pending-buffer write closes 1011. `_invalidate_connections` sleeps 0.25 s only for the 4401 (rotation) case; the 4409 resync case closes at once. The `name` query value is stripped; a whitespace-only name counts as missing.
- **types.ts effect entities** (the `effect.entity` values the server emits in `patch` and `action_result`): `live_highlights{pulses}`, `voice_feedback{feedback{message,at}}`, `review{review,items}`, `custom_report{report,removed}`, `item{item}`, `topic{topic}`, `rolling_summary{text,evidence}`, `title{text}`, `intent{intent}`, `cloud_enabled{enabled}`, `participant{participant}`, `question{question}`, `segment_speaker{segment_id,participant_id,source,pinned}`, `segment_text{segment_id,text,segment}`. Patch rows are `{op, target_id, effect, seq}`.
- **Client op builders** (`types.ts:ops`): `add_item{card,text,data?,evidence?}`, `update_item{id,set}`, `remove_item`, `pin_item`, `unpin_item`, `confirm_item`, `set_topic{text}`, `set_meeting_intent{text}`, `set_title{text}`, `set_cloud_enabled{enabled}`, `rename_participant{participant_id,display_name}`, `answer_question{question_id,answer_text}`, `dismiss_question`, `reopen_question`, `reassign_segment_speaker{segment_id,participant_id|null}`. Other ops the components send (review ops, `remove_custom_report`) come from the state store (a28).
- **Other wire types:** `voice_feedback.phase` is one of `heard, recognized, working, saved, uncertain, error, unavailable`. Agent tick `kind` is `thinking, writing, tool, turn, retry, compaction, start, settled, update` (or any string); `pass_kind` is `cards, notes, polish, consolidation` or `""`; `tool` and `pass_kind` are `""`, never null. Audit event rows are `{seq, ts, actor_type, actor_id, action, target_id, payload?, undoable}`. Rerun responses are `{ok, state, applied, error}` (respeakers adds `created?`, `windows?`).

### meeting/web/ws.py
- `WsHub(engine, repository)` with attribute `get_guest_url`. Methods: `on_startup()`,
  `on_shutdown()`, `schedule_broadcast(msg, host_only)`,
  `schedule_invalidate_connections(resync=False)`, `broadcast_json` (async),
  `handle_connection(ws)` (async).
- `_Connection` slots: `role, participant_id, name, ready, pending(deque), overflowed,
  send_timeouts, inflight`.
- Connection state machine: accept, validate, register (buffering), identify (the host's `me`
  participant or the guest join), send hello, flush pending (on overflow close 4409), set ready,
  broadcast `presence joined` (guests only), receive loop, then unregister, broadcast `left`
  (guests only) and close 1000.
- Constants: close codes 4400, 4401, 4409, 4429, 1000, 1011; `HELLO_SEGMENT_COUNT=200`,
  `HELLO_ACTIVITY_COUNT=50`, `MAX_MESSAGE_CHARS=65536`, `MAX_CONNECTIONS=32`,
  `MAX_INFLIGHT_ACTIONS=4`, `MAX_PENDING_MESSAGES=512`, `BROADCAST_SEND_TIMEOUT_S=5.0`,
  `MAX_SEND_TIMEOUTS=3`, `MAX_EVENT_SEQ=2**63-1`.
- Threading: all connection bookkeeping happens on the loop. Producers on other threads (the store
  subscriber callback, which runs on whichever thread applied the ops, and `server.broadcast`)
  enter through `run_coroutine_threadsafe`, and failures are logged through
  `_log_broadcast_failure`.
- Edge cases:
  - The size check counts characters, not bytes (ws.py:670).
  - `_recent_agent_activity` runs on the loop because it is an in-memory copy (ws.py:519-537).
  - `invalidate` clears the registry **before** closing (ws.py:293-298).
  - Host-only ticks skip sockets that are not ready yet; this is tested (test_meeting_ws.py:304).
  - The `receive` message is raw ASGI: `websocket.disconnect` is converted into the exception.
  - Undo `seq` coercion is `int(value)`, so a JSON float `3.7` becomes `3` and the string `"5"`
    becomes `5` (ws.py:127-133). Keep that leniency or decide to tighten it.

### meeting/web/archive.py
- `ArchivedMeetingDashboard(repository, meeting, *, spool_root, llm_provider="openrouter",
  llm_model="", llm_endpoint=None, agent_core_kind="pi", sidecar_payload_dir=None,
  model_lease=None)`. It provides the engine surface the web layer needs: `meeting_id`, `store`,
  `options` (a SimpleNamespace with `spool_root, llm_provider, llm_model, llm_endpoint,
  agent_core_kind, sidecar_payload_dir`), `model_lease`, `is_active`, `apply_client_action`,
  `undo`, `add_guest`, `get_transcript`, `regenerate_tokens`, `end`, `pause`, `resume`,
  `request_note_adjustment`, `set_cloud_enabled`, `attach_server`, `host_url`, `is_running`,
  `shutdown`.
- It has no `recent_agent_activity`, so hosts get `agent_activity: []`.
- Created by `services/runtime/meeting.py:1622-1652`, which also builds the server with
  `port = 0 if background engines else meeting_server_port`.

### webui/src (files in this area)
- **main.tsx**: imports the fonts (`@fontsource-variable/dm-sans` opsz plus italic, and
  `@fontsource/instrument-serif` 400 plus italic), `styles.css`, `themes/palettes.css` and
  `themes/designs.css`. It applies the theme and design theme before the first render, then renders
  `<App/>` in StrictMode into `#root`.
- **App.tsx**: `App` (boot and gating) and `MeetingDashboard`. Layout tree (class names come from
  `styles.css`):
  ```
  div.app-shell
    SelectionInsight (hidden while History is shown)
    HeaderBar
    [History] div.app-main.full-bleed > HistoryPane        (host only)
    [else] EvidenceProvider >
      div.app-main.workspace[.pocket][.ended] [data-workspace-scroll] [data-pocket-tab]
        div.workspace-ruler > HighlightPulseStrip
        aside.workspace-conversation > TranscriptPane(newestFirst, headerExtra=div.recording-inline > RecordingPlayer)
        div.workspace-center >
          ended or finalized: InsightReview (host) + ReportTabs(showDownload=false, showSwitcher=false)
          live: PreFlight (warming up) | MeetingOverview + MeetingBrief + div.center-notes > NotesPane
                + VoiceCommandHelp (while running)
          ActivityPane (host and showActivity and not reportsSettled)
        aside.workspace-rail > section.panel.capture > h3.capture-heading "Captured" + div.capture-body > CardsPane(highlightTop=3)
                             + ParticipantsPane
      JumpToLive + PocketTabs (pocket layout only)
    RoomDisplay (when ?view=room or opened from the header)
    VoiceAssistantBubble (live and active only)
  ```
  The CSS variable `--workspace-scrollport` is set from a ResizeObserver on the workspace. The audio
  element key is `${meeting_id}:${status}`. The player uses `preload="none"` while the meeting is
  active, otherwise `metadata`. Its src is `api.audioUrl(token,id,status)`, and a cache-busting
  `Date.now()` revision is used on click.
- **api.ts**, **ws.ts**, **state.ts**, **report.ts**, **theme.ts** and **playback.ts** are covered in
  the inventory above. **types.ts** is the wire schema: `MeetingStateDoc`, `Segment`, `CardItem`,
  `Question`, `Participant`, `Effect` union, `ServerMessage` union, the REST response types, and
  the `ops` builders (the client op vocabulary).

## Data and formats

### Settings keys (read by the callers, area a10)
| Key | Type | Default | Where read |
|---|---|---|---|
| `meeting_server_bind` | choice `localhost`/`lan` | `localhost` (config.py:746) | engine options `server_bind`; services/runtime/meeting.py:1643 |
| `meeting_server_port` | int, clamped 0..65535 | `0` (config.py:747) | engine options `server_port`; runtime (forced to 0 when background engines run) |
| `meeting_cloud_last_enabled` | bool | False | **written** by api.py:_remember_cloud_choice |
| meeting cloud consent | bool | False | `services.settings.resolve_meeting_cloud_consent()` (api.py:147) |
| `meeting_speaker_id_backend`, `meeting_audio_upload_consent_given` | choice, bool | | the respeakers gate (`services.meeting_rerun.resolve_speaker_pass`) |
| `meeting_report_ribbon`, `meeting_report_brief`, `meeting_report_signal` | bool | see `config.MEETING_REPORT_*` | `resolve_meeting_report_views`, which fills `state.report_views` (all off gives `ribbon`) |

### Database (owned by a28; this area only reads and writes through the repository)
The `meetings` row fields used here are `id, title, status, started_at, ended_at, paused_total_s,
cloud_enabled, asr_model, host_token, guest_token, state_json, state_seq, spool_dir, agent_provider,
agent_model` plus the stored endpoint. Repository calls: `get_meeting`, `get_segments_page`
(keyset on (start_s, id), limit clamped to 1001), `get_last_segments`, `get_segment`,
`get_segments`, `list_past_meeting_summaries(limit, query, cursor_started_at, cursor_id,
include_running, started_after)`, `search_transcripts`, `list_events`, `rename_meeting`,
`replace_tokens`, `get_meeting_content_summary`. test_meeting_web_history.py:53 pins **5 SELECTs**
for one history page: auth, bounded sessions, audio, counts, first preview.

### Files on disk
- `{bundle_root}/webui/dist/index.html` and `/assets/*`. This is a **prebuilt bundle checked into
  git**: about 361 KB of JS, 158 KB of CSS and the woff/woff2 fonts. Vite builds it with
  `base:'./'`, and `index.html` has `<base href="/">` so relative asset URLs resolve under `/m/{token}`.
- The meeting spool folder (`config.MEETINGS_FOLDER`, or `options.spool_root`) is deleted by the
  delete-meeting route.
- The playback WAV path comes from `build_playback` (area a28/a25).

### HTTP routes (all take the token as `?token=`; every JSON error is `{"detail": str}`)
| Method | Path | Role | Response |
|---|---|---|---|
| GET | `/m/{token}` | any valid | index.html / missing-bundle HTML / 403 |
| GET | `/assets/{path}` | none | static |
| WS | `/ws?token&name&guest_id` | any | see below |
| GET | `/api/session` | any | `{role, meeting, state, voice_commands}` |
| GET | `/api/transcript?cursor&limit` | any | `{items, next_cursor}` |
| GET | `/api/meetings?cursor&limit&q&since&has_decisions&has_actions&needs_attention` | host | `{meetings, next_cursor}` |
| GET | `/api/meetings/{id}?include_transcript` | host | `{meeting, state, segments, transcript_next_cursor, transcript_included}` |
| GET | `/api/meetings/{id}/state` | host | `{state}` |
| GET | `/api/meetings/{id}/transcript?cursor&limit` | host; guest if current | `{items, next_cursor}` |
| GET | `/api/meetings/{id}/segments/{sid}` | host; guest if current | `{segment}` |
| GET | `/api/meetings/{id}/audio` | host; guest if current | WAV, Range, no-store |
| POST | `/api/meetings/{id}/rename` `{title}` | host | `{ok,title}` |
| POST | `/api/meetings/{id}/review` `{op,...}` | host | `{ok,error,state}` (+ start_review fields) |
| POST | `/api/meetings/{id}/reinsights` | host | `{ok,state,applied,error}` |
| POST | `/api/meetings/{id}/respeakers` | host | `{ok,state,applied,created?,windows?,error}` |
| DELETE | `/api/meetings/{id}` | host | `{ok:true}` |
| GET | `/api/search?q&mode` | host | `{results, mode, message}` |
| GET | `/api/events?before_seq&limit` | host | `{events}` |
| GET | `/api/export/{md|json|txt}?meeting_id` | host | attachment |
| POST | `/api/meetings/{id}/reports` `{request}` | host | `{ok,report_id,error,state}` |
| DELETE | `/api/meetings/{id}/reports/{rid}` | host | `{ok,state}` |
| GET | `/api/meetings/{id}/reports/{rid}/download` | host | text/markdown attachment |
| POST | `/api/meeting/notes/request` `{text}` | any | `{ok,applied,rejected,error}` |
| POST | `/api/meeting/end` / `pause` / `resume` | host | `{ok:true}` |
| POST | `/api/meeting/cloud` `{enabled}` | host | `{ok,enabled}` |
| POST | `/api/meeting/tokens/regenerate` | host | `{ok,host_url,guest_url}` |

Authorization rule: the token is always resolved against the **current** meeting's
(`engine.meeting_id`) token pair. Past meetings are reachable only with the current dashboard's
host token. The query booleans follow FastAPI parsing: `1/0/true/false/yes/no/on/off`, with
anything else giving 422. The JSON body must be an object, else 400 `"body must be an object"`;
unparseable JSON gives 400 `"invalid JSON body"`.

### Cursor encodings (opaque to the client, but a golden-vector target)
- Transcript: `base64url(json.dumps([float(start_s), str(id)], separators=(",",":")))` with the
  `=` padding stripped. Python formats floats with `repr`, for example `1.0`, and `ensure_ascii`
  defaults to True, so non-ASCII ids are escaped as `\uXXXX`.
- History: the same scheme over `[started_at, id]`; both must be strings when decoded.

### WebSocket protocol
- Client to server: `{"type":"ping"}`, `{"type":"action","client_action_id",op:{op,...}}`,
  `{"type":"undo","client_action_id","seq"}`. The op vocabulary comes from `types.ts:ops` plus the
  ops used by components (`reassign_segment_speaker`, `rename_participant`, `set_meeting_intent`,
  review ops, ...). The authority is `meeting/state/patches.py` (area a28).
- Server to client: `hello`, `patch`, `action_result`, `presence`, `status`, `segments`,
  `speech_preview`, `voice_feedback`, `agent_activity` (host only), `error{code,message}`,
  `meeting_ended{status}`, `pong`. The shapes are in `types.ts:375-560` and `ws.py` (see A30-29..36).
- Close codes: 4400 name required (terminal), 4401 unauthorized or rotated (terminal),
  4409 resync (reconnect), 4429 too many (reconnect), 1011 server error, 1000 normal.

### Browser-side persisted keys (they belong to the origin `http://host:port`; since the port is ephemeral by default they effectively reset per server)
- `sessionStorage['ow_meeting_guest_id']`
- `localStorage['ow_report_view']`
- `localStorage['openwhisper.meeting.theme']`
- `localStorage['openwhisper.meeting.design']`

### External dependents
- The desktop runtime opens `host_url` with `?history=<id>[&view=report]` appended
  (services/runtime/meeting.py `_with_history_target`, tested in test_meeting_archive_dashboard.py:199-246).
- Any open browser tab keeps working across app versions as long as the routes and messages above
  stay unchanged.

## External dependencies

| Python / JS | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| fastapi, starlette (routing, `HTTPException`, `FileResponse` with Range, `StaticFiles`, `WebSocket`) | HTTP and WS server | **Qt HttpServer** (`QHttpServer`, Qt 6.4+, with WebSocket upgrade through `QAbstractHttpServer::newWebSocketConnection` and `QWebSocket`) together with **Qt WebSockets**. Alternatives: Boost.Beast plus Asio, or Drogon. Note that Qt HttpServer is **GPLv3 or commercial**, not LGPL. Range and 206 handling has to be written by hand with QHttpServer. |
| uvicorn | ASGI server, graceful shutdown | the same server object on a dedicated `QThread` with its own event loop; or the main thread, since Qt networking is asynchronous |
| asyncio (`to_thread`, `run_coroutine_threadsafe`, `wait_for`, `gather`) | offloading blocking work, cross-thread entry | `QThreadPool` / `QtConcurrent::run` plus `QFutureWatcher`; `QMetaObject::invokeMethod(Qt::QueuedConnection)`; per-socket `QTimer` for the 5 s send timeout (in practice watch `bytesToWrite()` on the socket) |
| concurrent.futures.ThreadPoolExecutor(1) | the re-run executor | a dedicated `QThreadPool` with `maxThreadCount=1` |
| json | encoding and decoding | `QJsonDocument` (the depth limit is built in), or nlohmann::json in the Qt-free core (it has a parser depth callback) |
| base64 | cursors | `QByteArray::toBase64(Base64UrlEncoding|OmitTrailingEquals)` / `fromBase64Encoding(..., AbortOnBase64DecodingErrors)` |
| secrets, hmac | tokens | `QRandomGenerator::system()` (or the OS CSPRNG) with base64url, and a constant-time compare loop |
| services.lan_address | LAN IPv4 | `QNetworkInterface` plus a UDP connect trick (area a17) |
| React 18, Vite, TypeScript, @fontsource | the dashboard | **keep as a prebuilt bundle** (recommended); otherwise a full rewrite, which would be out of this area's scope |

## Platform-specific code

- None in this area beyond the LAN address choice (a17) and `quiet_connection_lost`, which silences
  Windows Proactor reset errors (a01). Under Qt this is not needed.
- `log_config=None` works around a windowed frozen build on Windows having no stdout (a
  PyInstaller quirk). This has no C++ equivalent.
- The browser is opened by the runtime with `webbrowser.open`; in Qt that is
  `QDesktopServices::openUrl`. It behaves the same on X11, Wayland and Hyprland.
- `crypto.randomUUID` is missing on plain-HTTP LAN origins, so `ws.ts:newActionId` has a fallback.
  Keep the bundle as is.

## Engine-specific code

- Nothing here is specific to Whisper, Moonshine or Qwen. **No drops.**
- Reinsights passes `model_lease=engine.model_lease` to `rerun_finalization` (redecode reloads
  the ASR model). In FioriVoce the lease has to cover the in-process Parakeet `Recognizer`, which
  runs on `EngineWorker`'s thread. The re-run job must not call the recognizer from the server
  thread.
- `state.speech` (`source:"remote"`, host, model, connected, message) exists only for the
  remote-computer engine: **undecided**. Pass it through untouched if that engine is kept.
- `asr_model` in the public meeting fields will hold a Parakeet model name.
- Undecided cloud backends: review (A30-16), reinsights (17), respeakers (18, OpenAI), semantic
  search (20), reports (23), note requests (24), cloud toggle (26), and the archive LLM options (39).
  The routes should exist anyway and return the same 4xx wording when the backend is absent, so the
  bundle keeps working unchanged.

## Dependencies on other areas

Calls out:
- **a24 meeting-engine**: the engine surface `meeting_id, store, options.{spool_root, llm_*,
  agent_core_kind}, model_lease, is_active(), apply_client_action(actor, pid, op), undo(seq, pid),
  add_guest(name), end(), pause(), resume(), set_cloud_enabled(b), regenerate_tokens(),
  request_note_adjustment(text) -> Future, recent_agent_activity()`.
- **a28 state, persist and export**: `MeetingStateStore` (`snapshot, apply, undo, subscribe,
  unsubscribe, replace_document, refresh_title, with_state`), `MeetingState`, `parse_state_json`,
  `meeting.stored` (`open_store, load_state, stored_endpoint, compact_finalization_list_fields`),
  `meeting.content` (`meeting_display_title, summarize_meeting_content`), the exporters
  (markdown, json, txt), `delete_meeting_data`, `meeting_duration_s`, `OpResult`, and the
  repository.
- **a29 intelligence**: `custom_report.start_custom_report`, `MAX_REQUEST_CHARS`,
  `insight_review.start_review`, `semantic_search.search_history`, `refinalize.rerun_finalization`,
  `rerun_speakers`, `FinalizationBusyError`, `voice_help.voice_command_guide`.
- **a25**: `audio_playback.build_playback`.
- **a10 settings**: consent and the last-enabled setting, bind and port, `MeetingAgentCore.INSTALLED`.
- **a04 runtime services**: `services.meeting_rerun` (`rerun_options`, `resolve_speaker_pass`).
- **a17**: `services.lan_address`. **a01**: `services.asyncio_utils`, `config.bundle_root`.
- **Shared**: `services.titles.normalize_title`.

Called in by:
- a24 `engine._start_server` and `engine._broadcast`; `regenerate_tokens` calls `invalidate_connections`.
- a04 `services/runtime/meeting.py` (archive dashboard, `retitle_saved_meeting`,
  `refresh_saved_meeting_title`, host URL).
- a31 meeting Qt UI (opens the links); a20 MCP and agent API (meeting rename goes through the runtime).

## Tests

| Test file | What it pins | Golden-vector candidate |
|---|---|---|
| tests/test_meeting_web_auth.py | role resolution and host precedence; token length >= 32; cloud consent 403 and setting persistence; dashboard page and missing bundle; host-only 403 `"host only"`, bad token 401; public meeting fields (`display_title` from topic, finalization pill `"Saved for later"`/`warning`, `duration_s = 50*60-180`, digest ordering with `me` first, corrupt snapshot means no digest and `duration_s: null`); slow-End log text; transcript keyset with no gaps on equal `start_s`; guest segment and audio scoping; audio `no-store` and Range 206; historical state wire upgrade (`capture` defaults including `audio_incomplete` and `integrity_error`); events and token rotation; re-run guards (403, 409, 404 table) and the respeakers gate strings; reinsights double-click 409 then release; export token stripping; unknown export format 404 | `resolve_role`, `_public_meeting`/`_meeting_digest` (JSON in, JSON out) |
| tests/test_meeting_web_history.py | history keyset paging over 10 meetings with exactly 5 SELECTs; no token or state_json leak; a filtered empty page keeps a cursor; literal `q`; `since`; detail without transcript does no transcript queries; the state poll does no content-summary query; 401 and 403 on history and state; malformed cursor tables for both decoders | `_decode_cursor`, `_decode_history_cursor`, `_encode_cursor`, `_history_cursor` (strong candidates: pure byte in, byte out) |
| tests/test_meeting_ws.py | hello shape; no host token anywhere and guest token only inside `urls.guest`; a patch during the hello window is delivered after hello; agent_activity is host only and omitted in guest hello; host-only ticks are not buffered for sockets that are not ready; action echo of `client_action_id`; ping and pong; guest without a name refused; guest_id reuse and changed-name behavior; guest authorization against a real store (`set_title`, `set_cloud_enabled` and undo give `host_only`; a note is allowed); hostile input (binary, 20000 `[`, oversize, `1e400`, `10**40`, array, string op, unknown type) | `_on_store_batch` split (pure), `_coerce_seq`, `_clean_guest_id` |
| tests/test_meeting_custom_report.py:621-828 | report REST: host only, bad-request table, 404, "already being written" 409, interrupted run shown as failed, delete 200 then 404, download headers; guest scoping in the fan-out, `/api/session` and detail | `_on_store_batch` with report effects |
| tests/test_meeting_note_requests.py:88-107 | note request: 401 without a token, 400 for `""`, whitespace, 4001 characters or a number; both roles allowed; result summary | `_note_request_summary` |
| tests/test_meeting_archive_dashboard.py | archive normalizes status and title from the row; interrupted finalization becomes `failed` with "interrupted"; link rotation and shutdown; archive serves an authenticated session; runtime history URL building (`?history=`, `&view=report`) | `_with_history_target` (a04) |
| tests/test_meeting_title_consistency.py | every title write path follows the same contract; a refresh of the current dashboard closes sockets with **4409** without expiring the host token; a cached report delete after an MCP rename does not restore the old title | |
| tests/test_meeting_web_server_stop.py | `stop()` finishes in under 5 s while a streaming response is open | |
| tests/test_meeting_web_server_address.py | LAN display host falls back to 127.0.0.1 | |
| tests/test_meeting_web_server_logging.py | no stdout dependency (Python-only, drop) | |
| tests/test_meeting_insight_review.py:350, test_mcp_controls.py:192, test_installed_agent_core.py:263, test_meeting_stored.py:90 | archive dashboard used inside wider flows | |
| webui/tests/dashboard-actions.cjs, custom-reports.cjs (`applyEffect`), end-meeting-feedback.cjs and history-performance.cjs (`api`), plus the rest | the client reducer and components. These stay valid as long as the bundle is reused. | `meetingReducer` and `applyEffect` are already tested in JS; they matter only if the client is rewritten |

**Contract test approach:** record request and response pairs, and WS transcripts, from the Python
server against fixture repos (the fakes in `tests/fakes/meeting_web.py`). Then replay them against
the C++ server and compare status, `detail` text and JSON bodies with volatile fields masked. This
gives the strongest parity check for this area.

## Porting notes

**Recommended shape: keep the React dashboard and port only the server.** The dashboard is a
browser app by design: it serves guests on other machines and has no Qt UI to match. Reusing
`webui/dist` gives an identical look for free, keeps the 22 JS tests meaningful, and limits this
area to the server contract. The cost is a Node toolchain at development time only, needed to
rebuild after rebranding (index title `OpenWhisper · Meeting`, the missing-bundle notice text, the
storage keys `openwhisper.meeting.*`). Vendor `webui/` source and `dist` into FioriVoce, and serve
`dist` from the install folder or from Qt resources (`:/webui`).

Suggested decomposition:
1. **Qt-free core** (`src/meeting/web/`, plain C++20, testable with a compiler alone):
   `auth` (token generation, constant-time role check), `cursor` (encode and decode, matching the
   Python bytes exactly), `public_meeting` (projection and digest), `ws_protocol` (frame
   validation, message builders, `coerce_seq`, `clean_guest_id`, patch split), and a
   transport-agnostic `DashboardApi` that maps (method, path, query, body, token) to
   (status, headers, body), holding the `_AppContext` guards. It depends on interfaces for
   Engine, Store and Repository from a24 and a28.
2. **Transport layer (Qt):** `MeetingWebServer` (QHttpServer plus QWebSocket, or Beast) on its own
   `QThread`. It owns `WsHub`: the connection registry, pre-hello buffer, fan-out with
   back-pressure timeouts, and invalidation with a 250 ms delay. It also serves static files with
   Range support. Cross-thread `broadcast()` goes through queued invocation; store subscriber
   callbacks arrive from any thread.
3. `ArchivedMeetingDashboard` as a second implementation of the engine interface.

Order of work: auth and cursors with golden vectors; the static and page routes; session and
transcript; the WS hub (hello, buffer, patch, presence, action, undo); the engine push messages;
history, detail and state; audio with Range; export, rename and delete; meeting controls and token
rotation; the archive adapter; then the cloud-dependent routes (review, reinsights, respeakers,
reports, notes, cloud, semantic search) once their backends are decided. Until then those routes
should return a 4xx with a clear message.

Risks:
- Matching FastAPI's incidental behavior: query bool and int parsing with 422 bodies, 404 and 405
  shapes, the `detail` envelope, `Content-Disposition` formatting, and Range or 416 handling.
  Record these from the Python server.
- Qt HttpServer licensing (GPLv3) and its maturity: WS upgrade on the same port and streaming
  file responses. Boost.Beast avoids the license issue but costs more code.
- Single-writer guarantees (`insights_running`, `review_stores`) depend on the asyncio loop being
  single-threaded. In C++, keep all `_AppContext` mutation on the server thread, or guard it with a
  mutex.
- The hello buffering race (A30-30) needs registration and readiness flipped on the same thread as
  the broadcasts.
- Byte-exact JSON is not required (the client parses it), but `ensure_ascii=False` versus `True`
  differs between WS and cursors.
- Stopping must not wait on a streaming audio response: abort sockets after 1 s.

Rough size: core about 1,200 lines and transport plus hub about 1,600 lines, so with the archive
adapter and glue about **3,000 lines of C++** (excluding the cloud-backed handlers' business
logic, which belongs to a29). Reusing the bundle adds 0 lines of client code; rewriting the
dashboard natively would be over 15k lines and is not recommended.

What native code could do better: no 15 s startup poll (bind synchronously and know the port
immediately); no separate interpreter thread and event loop; zero-copy `sendfile`-style audio
serving; serving the bundle from compressed Qt resources; and lower per-socket memory. The
fan-out can serialize once and write the same `QByteArray` to every socket, as the Python does.
