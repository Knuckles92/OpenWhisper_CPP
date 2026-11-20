# Remote engine: host, client, pairing, TLS, discovery, records and history sync

Area `a17-remote-engine-services`. Source (relative to `D:\coding\whisper_local`), 9 628 lines:

| File | Lines | One line |
|---|---|---|
| `services/remote_asr/protocol.py` | 296 | Wire format: frames, PCM16, limits, close codes, fingerprints, SAS, address parsing |
| `services/remote_asr/host.py` | 1346 | `SpeechHost` TLS WebSocket server, `DeviceRegistry`, pairing (code / Tailscale / approval), admission, dispatch |
| `services/remote_asr/client.py` | 703 | Probe, pair, request pairing, `RemoteConnection` (pinning, fallbacks, heartbeat, cancel) |
| `services/remote_asr/service.py` | 968 | `RemoteEngineService`: both roles in one object the controller owns; scans, model switch, records, MCP, update hold |
| `services/remote_asr/discovery.py` | 371 | UDP LAN discovery (responder + search), /24 TCP sweep |
| `services/remote_asr/settings.py` | 269 | Persisted client pairing, token credential name, host opt-ins |
| `services/remote_asr/tailscale.py` | 217 | `tailscale status/whois --json` via CLI; owner trust |
| `services/remote_asr/engines.py` | 199 | `HostEngine` adapter over the selected backend; `host_models()` |
| `services/remote_asr/mcp_link.py` | 226 | `HostMcpLink` + facades so the MCP settings page edits the host's MCP |
| `services/remote_asr/dependencies.py` | 224 | Per-device runtime readiness, remote runtime installation job |
| `services/remote_asr/activity.py` | 144 | In-memory per-device counters and recent events for the host dashboard |
| `services/remote_asr/model_management.py` | 125 | Model catalog and one remote-initiated model download job |
| `services/remote_asr/reachability.py` | 123 | Windows network profile (Public) / VPN / discovery-port notes |
| `services/remote_asr/runtime.py` | 113 | `ready.runtime` description and `configure_runtime` validation |
| `services/remote_asr/tls.py` | 103 | Self-signed host cert (EC P-256, 20 y), server/client SSL contexts |
| `services/remote_asr/__init__.py` | 6 | docstring |
| `services/lan_address.py` | 329 | Interface enumeration (ctypes getifaddrs / GetAdaptersAddresses), LAN/VPN/virtual/Tailscale classification |
| `services/remote_records/host_store.py` | 421 | Host side of record storage: staging, resumable verified upload, export/fetch |
| `services/remote_records/kinds.py` | 786 | Dictation and meeting records as files (bundle/import/export), untrusted-JSON coercion |
| `services/remote_records/sync.py` | 930 | Client outbox worker (`record_sync` table), copy/move/delete, checkout, bring back, audio cache |
| `services/remote_records/gate.py` | 46 | `RecordGate`: drain record requests before a backup / recovery |
| `services/remote_records/__init__.py` | 7 | docstring |
| `services/remote_history/channel.py` | 164 | Host side of "direct history": `HistoryBroker` sends `history_query` to clients |
| `services/remote_history/client.py` | 220 | Client side: `HistoryClient` reconnect loop, allow-listed read-only `HistoryReader` |
| `services/remote_history/__init__.py` | 1 | empty |

No widgets live here. The Settings "Remote engine" page, host dashboard and pair-request dialog are
`a18-remote-engine-ui`; the client speech backend (`transcriber/remote_backend.py`) is `a14`; the
controller glue (`services/runtime/remote.py`: `switch_engine_for_client`, `configure_engine_for_client`,
`on_host_event`) is `a04`. Nothing in this area imports Qt; listeners fire on arbitrary threads.

Status of the whole area for OpenWhisper_CPP: **undecided backend** (the remote-computer engine is not yet kept or
dropped). Within it, Whisper / Moonshine / Qwen / MLX specifics are **drop**. `lan_address.py` is also used by the
meeting web server (`meeting/web/server.py:discover_lan_ipv4`), so it is **port** regardless.

## What the user gets

- **Share this computer's speech engine** (host). One toggle in Settings -> Remote engine starts a TLS WebSocket
  server on TCP 47821 (configurable 1..65535) and a UDP discovery responder on 47821. Paired computers then dictate,
  get live previews and run meetings on this computer's local engine. Turning sharing off disconnects everyone.
- **Pair a computer** three ways:
  1. *Code*: host clicks "Show a pairing code"; a 6-digit code valid for 5 min, one pairing, 5 wrong tries closes it.
  2. *Tailscale owner*: a computer signed into the same untagged Tailscale login pairs with no code (on by default,
     host setting "trust my Tailscale computers").
  3. *Approval request*: a computer found on the network asks; both screens show the same 6-digit SAS ("123 456");
     the host owner allows or denies (120 s to answer). The prompt lists what allowing also grants.
- Client verifies the host by **certificate pinning** (SHA-256 of the self-signed cert, 5 groups of 4 shown to
  people, e.g. `ABCD-EF01-...`). The token is never sent until the pinned cert matches.
- **Finds hosts**: "Search" = UDP broadcast on each LAN card (1.2 s) + Tailscale peers probed in parallel, merged by
  certificate; "Check every address" = TCP knock on the whole /24 (for routers dropping broadcasts).
- **Follows a moving host**: the host lists its LAN and Tailscale addresses in every `ready`; the client saves them,
  tries the last good one first, falls back (2.5 s per non-final address), and rediscovers by certificate on the LAN
  (at most every 30 s). Laptop away from home falls to the Tailscale address.
- **Host dashboard data**: connected clients (busy flag, recording), per-device counts (transcriptions, audio s,
  host s, previews, errors) and the last 60 events since sharing started; reachability notes ("Windows treats ... as
  a public network", "A VPN is on", "Other computers can't find this one by searching (UDP port 47821 is in use...)").
- **Model switching from the client**: client lists models ready on the host and switches it; reply after the model
  has loaded (up to 240 s). Opt-in **model management**: download models and install runtimes on the host.
- **Runtime controls** from the client: device (auto/cpu/cuda), precision (Whisper), language (optional engines).
- **Recording reservations**: client reserves a recording on the host so the host's auto-updater waits.
- **Records on the host** (opt-in on host, "Keep records for paired computers"): client chooses where its
  dictations and meetings live while paired: `local` (default), `host` (moved), `both` (copied). Resumable,
  SHA-256-verified uploads in 1 MiB chunks; History / Past Meetings list host-kept records; "Bring back" moves all
  home. Host shows them badged with the device name in its own History / Past Meetings.
- **Direct history** (client opt-in, off by default): the host's MCP/API can query the client's history read-only
  while the client is online.
- **MCP control** (host opt-in): a paired computer's MCP settings page can turn the host's MCP server on/off,
  change port/Tailscale/permissions and read its access token.
- **Rename** a paired device (host side) or the paired host (client side); empty name reverts.

## Feature inventory

| ID | Feature | Exact behaviour | Source | Status |
|---|---|---|---|---|
| A17-01 | Frame format | `>I` header length + UTF-8 JSON header (compact separators) + payload. Header <= 64 KiB. Audio = 16 kHz mono int16 LE, float->int16 `clip(rint(x*32768),-32768,32767)`, back `/32768`. Odd byte count -> `ProtocolError("Audio payload has an odd byte count")`. Audio <= 120 s (3 840 000 bytes). Reply <= 8 MiB. Request <= 4+64Ki+3 840 000. | `protocol.py:pack_request/unpack_frame/encode_audio/decode_audio/payload_audio` | undecided backend |
| A17-02 | Endpoint and version | `wss://host:port/openwhisper/v1/engine`, `PROTOCOL_VERSION=1`, other paths -> HTTP 404 "Not an OpenWhisper remote engine endpoint\n". Wrong `protocol` in hello -> error `protocol` "This host speaks remote engine protocol 1; update OpenWhisper on both computers." + close 4400. | `protocol.py`, `host.py:_check_path,_handle_admitted` | undecided |
| A17-03 | Close codes | 4401 unauthorized, 4400 bad request, 4408 handshake timeout, 4409 engine changed, 4429 busy; 1001 "host stopped sharing". | `protocol.py:CLOSE_*` | undecided |
| A17-04 | Host TLS identity | EC P-256 self-signed, CN `OpenWhisper host <hostname>` (64 chars max), valid now-1d .. +20y, SHA-256 signed; key PKCS8 PEM unencrypted, file mode 0600; cert `remote_engine_host_cert.pem`, key `remote_engine_host_key.pem` in the user data dir; regenerated if unreadable. Server TLS >=1.2, **no TLS 1.3 session tickets** (Windows stall). Client: TLS>=1.2, no CA, no hostname check. | `tls.py` | undecided |
| A17-05 | Fingerprints | `certificate_fingerprint` = uppercase hex SHA-256 of DER; `short_fingerprint` = first 5 groups of 4 alnum chars joined by `-`. | `protocol.py` | undecided |
| A17-06 | Address parsing | Strips `https:// http:// wss:// ws://` and any path; `[v6]:port`, `host:port`, unbracketed v6 = host; errors: "Enter the host's address, such as 192.168.1.20 or devbox.local", "The IPv6 address is missing its closing bracket", "Put the port after the bracket, as in [fe80::1]:47821", "The host's address is not valid", "The port must be a number", "The port must be between 1 and 65535". `format_address` brackets hosts containing `:`. | `protocol.py:parse_address,format_address` | undecided |
| A17-07 | Name cleaning | printable chars only, trimmed, max 60; device fallback "Unnamed computer". | `protocol.py:clean_name`, `host.py:clean_device_name` | undecided |
| A17-08 | Device registry | Settings list `remote_host_devices`: `{id: uuid4 hex, name, token_sha256, paired_at, last_seen (UTC ISO, no us), via, [paired_name], [record_owner_ids]}`. Token = `token_urlsafe(32)`, only SHA-256 hex stored. Auth compares against **every** entry with constant-time compare. `last_seen` persisted at most every 60 s per device. Rename: empty -> back to paired name; `paired_name` kept while renamed. `record_owner_ids` sanitized to `[A-Za-z0-9_-]{1,64}`, dedup. | `host.py:DeviceRegistry` | undecided |
| A17-09 | Code pairing | 6 digits `randbelow(10**6)` zero-padded, TTL 300 s, one use; wrong code -> `bad_code` "That code doesn't match the one on the host."; 5th wrong -> closes pairing, "Too many wrong codes. Start pairing again on the host."; not open -> `pairing_closed` "Pairing isn't open on the host. Click \"Show a pairing code\" there and enter the code it shows." Reply `paired {token, device_id, host{name,fingerprint,addresses}}` then close. Client strips non-digits; empty -> "Enter the pairing code shown on the host." | `host.py:open_pairing,_pair,_finish_pairing`; `client.py:pair_with_host` | undecided |
| A17-10 | Tailscale pairing | `pair` with `tailscale:true`; allowed only if host trust on, Tailscale running, **both** remote and local socket addresses are tailnet IPs, and `tailscale whois` names the same untagged login. Refusal `tailscale_refused`: "This host only lets its owner's own Tailscale computers pair without a code. Enter the pairing code shown there instead." or "This host doesn't pair over Tailscale without a code. Enter the pairing code shown there instead." Name defaults to whois node name. | `host.py:_pair_tailscale`, `tailscale.py:own_device` | undecided |
| A17-11 | Approval pairing (SAS) | `pair_request{device_name}` -> host `pair_commit{commit, timeout_s:120}`; client `pair_nonce{nonce hex 32B}`; host `pair_reveal{nonce}`; both compute SAS = `sha256("openwhisper-pair-sas-v1"+host_nonce+client_nonce+FP_upper_ascii)[:8]` big-endian mod 10^6, 6 digits; commitment = `sha256("openwhisper-pair-commit-v1"+nonce)` hex. Client checks reveal vs commit: "The host's pairing answer didn't check out. Pairing was stopped; try again on a network you trust." Host shows request only once SAS exists. Outcomes: allowed -> `paired`; denied `pair_denied` "<host> declined the request."; stopped `pair_closed` "<host> stopped sharing its engine."; timeout `pair_timeout` "Nobody answered on <host>. Try again while someone is at it, or pair with a code."; client gone = canceled (no message). Client may send `pair_cancel`; client waits max 150 s ("The host didn't answer in time. Try again."). | `protocol.py:pairing_*`, `host.py:_admit_request,_pair_request,_await_answer`, `client.py:request_pairing,_await_decision` | undecided |
| A17-12 | Approval admission | Only private/link-local/loopback/Tailscale senders (`approval_refused` "This host only takes pairing requests from its own network. Enter the pairing code shown there instead."); max 4 requests per address per 600 s (`approval_limited` "Too many pairing requests from this computer. Wait a few minutes, or enter the pairing code shown on the host."); one at a time (`approval_busy` "<host> is already asking about another computer. Try again in a minute."). Request expiry: 10 s + 120 s before SAS, 120 s after. Answer API `answer_request(id, allow)` false if gone. On stop: request told "stopped", host waits up to 2 s for the handler to finish. | `host.py` | undecided |
| A17-13 | MITM check on pairing | `paired.host.fingerprint`, if present, must equal the cert this socket saw: "The host's certificate didn't match what it reported. Pairing was stopped; try again on a network you trust." | `client.py:_pairing_result` | undecided |
| A17-14 | Probe | Unauthenticated `probe` answered **before** version check: `{type:probe, protocol, host{name}, engine{label,device,available,family}, tailscale_pairing, approval:true}`; no secrets. Client probe timeout 2 s, records fingerprint. | `host.py:_probe`, `client.py:probe_host` | undecided |
| A17-15 | Hello / ready | `hello{protocol, token, [capabilities{recording_activity:true}] or [purpose:"history", history_enabled]}`. Unknown token -> `unauthorized` "This computer isn't paired with the host anymore. Pair it again." + 4401. `ready{host{name,fingerprint,addresses}, device{id,name}, engine (describe), models, runtime, capabilities{model_management, runtime_installation, engine_controls, recognition_hints, recording_activity:true, [mcp_control], [records]}, [records summary]}`. While loading: engine `available:false, status:"The host is loading its speech engine."`; during update hold: "The host is preparing an OpenWhisper update. Try again shortly." | `host.py:_serve,_serve_device,_ready_message` | undecided |
| A17-16 | Admission control | Max 12 pending handshakes, 32 paired connections (separate semaphores; hello trades handshake slot for connection slot). Over -> `{type:error, code:busy, message:"The host is busy. Try again shortly.", retry_after_ms:1000}` + 4429. Costly ops (`transcribe stream select_model configure_runtime model_catalog download_model install_runtime`) go through a FIFO pool: 2 active, 4 queued, 30 s max wait, abandoned if client disconnects; overflow -> per-request `busy` reply with `retry_after_ms:1000`. Record ops skip the pool. Per-connection inbound queue 2 frames. Handshake timeout 10 s. Server pings every 20 s, 20 s timeout. No compression. | `host.py:_Reservation,_WorkAdmission,_handle,_admit_and_dispatch,start` | undecided |
| A17-17 | Request dispatch | Request `{id, op, ...}`; reply `{id, result}` + `host_ms` (time from receipt, queue wait included, 0.1 ms) or `{id, error, [code], [retry_after_ms]}`. Revocation rechecked before every frame (device removed -> 4401 "device removed"). Ops: `transcribe` (language, phrases), `stream` (session, finish), `cancel_stream`, `describe`, `select_model`, `configure_runtime`, `model_catalog`, `download_model`, `install_runtime`, `recording_begin/end`, `records_*`, `mcp_state/mcp_configure`. Unknown -> "Unknown operation: 'x'". Engine identity changed -> `{code:engine_changed, error:"The host switched speech engines. Reconnecting."}` then close 4409. Not settled -> `engine_reloading` "The host is loading its speech engine." | `host.py:_dispatch*,_decode_request` | undecided |
| A17-18 | Streams per connection | Session id scoped `remote-<conn8>-<session>`; all open streams cancelled on disconnect; missing session -> "A stream needs a session id". | `host.py:_stream,_drop_client` | undecided (Parakeet has no stream) |
| A17-19 | Vocabulary hints | `transcribe.phrases`: <=50 strings, each whitespace-collapsed, 1..64 chars, others dropped; only passed when engine `accepts_phrases`; never part of identity. | `protocol.py:header_phrases`, `host.py:_transcribe` | undecided |
| A17-20 | Engine change push | When this computer's engine finishes loading, idle clients whose `engine_identity` differs are closed 4409 "engine changed" (background thread); in-request clients are left for their reply. Also rechecked right after registration, before `ready`. | `host.py:engine_changed`, `service.py:engine_changed` | undecided |
| A17-21 | Client connection | `RemoteConnection(host, port, token, fp, alternates, history_enabled, reports_recording)`: candidates = last-good-for-fp first, then saved order; timeout 6 s final / 2.5 s others; wrong cert = try next ("<where> presented a different certificate than when this computer paired with it. If OpenWhisper was reinstalled there, pair again; otherwise something is intercepting the connection."); all fail -> LAN rediscovery by fp (<= every 30 s per fp) -> else raise the error for the **paired** (first) address. Busy reply -> `RemoteHostBusy(retry_after_s)`. After connect: saves addresses (`remember_host_addresses`). | `client.py:RemoteConnection.connect,_connect_to,_search_network,_arrived` | undecided |
| A17-22 | Client errors | Translation of open failures: HTTP status -> "<where> answered, but it isn't an OpenWhisper remote engine (HTTP n)."; bad handshake -> "<where> isn't an OpenWhisper remote engine."; DNS -> "Couldn't find a computer named <host>."; refused -> "Couldn't reach <where>: nothing there is accepting connections on that port. Check that the host is sharing its engine, on that port." (`refused=True`); other -> "Couldn't reach <where>. Check that the host is on, sharing its engine, and allowed through its firewall." Read errors: "The host didn't answer in time.", "The host closed the connection.", "The host sent a reply this version can't read." Exception classes: `RemoteEngineError`, `RemoteHostBusy`, `RemoteRequestError(code, retry_after_s, retryable=code=="busy")`, `RemoteConnectionLost(sent)`, `RemoteDecodeNotStarted`, `RemoteUnreachable(refused)`, `PairingCanceled`. | `client.py` | undecided |
| A17-23 | Client heartbeat | Own ping thread: ping immediately then every 20 s, pong timeout 20 s -> mark closed. `latency` = last RTT (perf counter), `beats` = pongs. `alive` = socket OPEN. `via_tailscale` by peer IP. `closed_for_engine_change` = close code 4409. | `client.py:_heartbeat` and properties | undecided |
| A17-24 | One request at a time, cancel | Serial ids; recv polled every 50 ms; `abandon()` marks all current serials abandoned (keeps connection; late replies skipped by id); `close()` from another thread cancels ("Transcription canceled"); request timeout default 180 s closes connection ("The remote engine on <where> timed out; the connection was closed."); send failure -> `RemoteConnectionLost(sent=False)`, receive failure -> `sent=True`. | `client.py:request_timed,_await_reply,_receive,abandon` | undecided |
| A17-25 | Recording reservation | `recording_begin/end{recording_id 32 hex}`, no payload; client waits <= 0.75 s for the lock ("The remote speech connection is busy. Try again shortly."). Host: only when hello advertised `recording_activity`; begin refused during update hold (`engine_reloading`); disconnect with an open reservation keeps a 15 s "reconnecting" grace; same id on reconnect reclaims it. | `client.py:recording_activity`, `host.py:_recording_request,_drop_client` | undecided |
| A17-26 | Update hold | `prepare_update()` -> `{token, expires_in:60}` or `{reason}`: "The speech host is starting or reloading", "Another update already reserved the host", "A paired client is recording or using the speech host", "A connected client cannot report its recording state"; `cancel_update(token)`; `update_status()` = `{recordings, requests, untracked_clients, reconnecting_recording, maintenance, running, engine_ready}`. During hold costly ops answer `engine_reloading` with HOST_UPDATE_MESSAGE. Service without host -> "Speech sharing is off". | `host.py:prepare_update,update_status`, `service.py` | undecided (Linux updater only) |
| A17-27 | Model switch by client | `select_model{family, model, [device]}`; host offers only models cached with runtime installed (`host_models`, Whisper last); refuses concurrent change ("The host is changing its engine. Try again in a moment."); not ready -> "<model> isn't ready on <hostname>. Download it there first."; already running -> returns current; waits up to 240 s polling 0.1 s: "<name> is still loading <label>. Try again in a moment.", "<name> couldn't load <label>.", "<name> switched to another model while loading <label>." Reply `{engine, models}`; activity event `switched`. With `device`: needs model management permission ("Model management is disabled on the host."). No switch wired -> "<host> doesn't let paired computers change its model." | `host.py:_switch_model`, `service.py:select_model,_select_model`, `engines.py:host_models` | undecided |
| A17-28 | Runtime controls | `configure_runtime{family, model, settings{device|compute_type|language}}`; stale engine -> "The host changed engines. Reconnect before changing its runtime."; other validations (see runtime.py strings); waits for load <= 240 s; reply `{engine, runtime}`. `ready.runtime` = `{family, model, selected{device, compute_type|language}, devices, compute_types, languages, gpu{name,total_mib}, dependencies}`. | `runtime.py`, `service.py:configure_runtime`, `host.py:_configure_runtime_request` | undecided; Whisper/Moonshine/MLX branches drop |
| A17-29 | Model management | Opt-in `remote_host_model_management` (off), rechecked per request: "Model management is disabled on the host. Enable it in Settings → Remote engine there." Exact header field allow-lists. `model_catalog` -> `{models[{family,model,label,cached,runtime_ready,download_size,dependencies,selected_device}], download, engine, can_select, can_install_runtime:true, installation, runtime}`. `download_model`: only bundled catalog, one job at a time, honours HF policy ("Model downloads are disabled by HF_HUB_OFFLINE on the host.", "The host requires download consent. Choose ‘Always allow downloads’ in Settings → Downloads on the host, or download this model there first."), sanitized failure "Download failed on the host. Check its connection, free disk space and logs, then retry." Survives disconnect. | `model_management.py`, `service.py:manage_host_models` | undecided |
| A17-30 | Remote runtime install | `install_runtime{family, model, device auto|cpu|cuda}`; options from `dependency_options` (ready/installed/installable/reason/download_bytes); states installing/complete/failed/restart_required; strings in dependencies.py. | `dependencies.py` | drop (component installer for other engines) unless NeMo GPU runtime install is kept |
| A17-31 | Host activity | Per device: transcriptions, audio_s, host_s, previews, errors, last_at; events max 60 (`transcribed` audio_s 2dp host_s 3dp, `failed` reason <=160 chars with "…", `connected`/`disconnected` only on first/last connection of a device, `paired` via, `switched` label); snapshot newest-first, totals, monotonically increasing `serial`; reset when sharing starts; never on disk. | `activity.py` | undecided |
| A17-32 | Host state for the page | `host_state()` dict: enabled, running, port, error ("Couldn't listen on port N (<strerror>). Another program may be using it; choose a different port." / "Couldn't start sharing: ..."), host_name, address, address_kind (lan/vpn), fingerprint, pairing (code, seconds_left), clients, devices, engine, tailscale, tailscale_trust, model_management, keep_records, manage_mcp, pair_request, notes. Listeners get kinds: state, pairing, pair_request, devices, clients, activity, paired, engine, models, components, records, mcp, tailscale, client, host_renamed. | `service.py:host_state,_notify` | undecided |
| A17-33 | Reachability notes | `public_network` (Windows category Public on the LAN card's interface) with action "Open network settings" -> `ms-settings:network-wifi` / `network-ethernet` / `network-status`; `vpn` when a VPN address exists beside a LAN one; `discovery` when UDP bind failed. Profiles read via PowerShell `Get-NetConnectionProfile`, cached 60 s, background refresh. | `reachability.py`, `service.py:network_profiles` | undecided (Windows only) |
| A17-34 | LAN discovery responder | UDP 47821; query = `OWHISPER?1` + 16-byte nonce, zero-padded to 512 B; reply `OWHISPER!1` + JSON `{n, name<=60, port, fp, protocol, approval, engine{label,device,available,family}}`, never > 512 B (trims engine to `{available}`, then name to 16). Answers only private/link-local/loopback/100.64/10 senders; 20 answers/sender and 200 total per 10 s; sender map pruned >1024. Bind failure: error "UDP port N is in use (...)", sharing keeps running. | `discovery.py:DiscoveryResponder,encode_reply` | undecided |
| A17-35 | LAN search | Sends to each private LAN card's broadcast (prefix 8..30) + 255.255.255.255, at 0, 0.3, 0.7 s, listens 1.2 s; no bind; dedup by fp; reply validation: nonce match, 64-hex fp, port 1..65535, printable name. `find_host(fp)` stops early. | `discovery.py:search,find_host,parse_reply,broadcast_targets` | undecided |
| A17-36 | Sweep | Other addresses of the first two private LAN /24s (prefix clamped to >=24), TCP connect 0.4 s, 64 workers; then probe each answerer (12 workers). | `discovery.py:sweep*`, `service.py:scan_lan` | undecided |
| A17-37 | Tailnet scan + merge | Online desktop peers (windows/linux/macos) probed on 47821, 12 workers; `NearbyHost` merged by fp (tailnet key fallback `tailnet:<ip>`); own fp excluded; sorted by name (Tailscale machine name preferred); address = LAN unless code-free tailnet pairing possible. | `service.py:scan_tailnet,scan_nearby` | undecided |
| A17-38 | Tailscale CLI | `tailscale status --json` / `whois --json <ip>`; CLI lookup PATH, then `%ProgramFiles%\Tailscale\tailscale.exe`, macOS app/homebrew/usr/local, Linux /usr/bin, /usr/local/bin, /snap/bin; macOS env `TAILSCALE_BE_CLI=1`; 4 s timeout; states running/stopped/needs_login/starting/not_installed/unavailable; peers `mine` = same login, untagged, not ShareeNode; sorted mine first. Status cached 30 s, refreshed on a background thread, never blocks. Tailnet = 100.64.0.0/10 and fd7a:115c:a1e0::/48 (v4-mapped handled). | `tailscale.py`, `service.py:tailscale_status` | undecided (also used by a20 MCP) |
| A17-39 | Client pairing persistence | Settings `remote_engine_client` dict; token in OS credential store under `OPENWHISPER_REMOTE_ENGINE_TOKEN` (per-user dir) or `..._<SHA256(normcase(realpath(data_root)))[:12] upper>`; legacy read fallback to unscoped name. Save token first, then settings. Keychain failure keeps pairing in memory, `retry_pairing_save()`. Forget removes settings + credential. | `settings.py`, `service.py:_paired,retry_pairing_save,forget_host` | undecided |
| A17-40 | Following a moved host | `remember_host_addresses(fp, reached, port, listed)` rules (see Modules). | `settings.py` | undecided |
| A17-41 | Client-side host rename | `paired_name` holds original; `name_for(reported)` uses local name when renamed. | `settings.py:rename_client_host,ClientPairing` | undecided |
| A17-42 | Remote model / MCP requests | One-shot connection per call; capability gates with messages "Update OpenWhisper on the host to manage its models remotely.", "Update OpenWhisper on the host to install runtimes and choose a model's device remotely.", "The paired host changed. Close this window and open Manage host models again."; timeout 250 s for select_model else 30 s; no retry. MCP: `unsupported`/`forbidden` codes. | `service.py:remote_model_request,remote_mcp_request` | undecided |
| A17-43 | Host MCP control | Opt-in `remote_host_manage_mcp`; ops `mcp_state`, `mcp_configure{settings}`; `ValueError("code: text")` mapped to code; forbidden text "<host> isn't letting paired computers manage its MCP server. Turn on \"Allow paired computers to manage MCP\" in Settings → Remote engine there." | `host.py:_mcp_request`, `service.py:mcp_request` | undecided |
| A17-44 | MCP link (client) | View refreshed every 2 s when READY, 5 s otherwise; optimistic changes, rolled back on failure with `take_problem()` once; states loading/ready/forbidden/unsupported/offline; enabling shows "Starting MCP…", disabling "MCP is off." and clears token; facades `HostMcpSettings`, `HostMcpServer`. | `mcp_link.py` | undecided |
| A17-45 | Pairing grants text | "download models and runtimes here", "keep its dictations and meetings here", "manage MCP here, including the access token that reads every saved dictation and meeting". | `service.py:pairing_grants` | undecided |
| A17-46 | Host record store | `records_*` ops scoped to device (+ host-assigned `record_owner_ids`); writes need opt-in (forbidden text "<host> isn't keeping records for other computers. Turn on \"Keep records for paired computers\" in Settings → Remote engine there."), reads/deletes always allowed while paired. Resumable verified upload, 1 MiB chunks, free space margin 1 GiB ("Not enough free space on the host to keep this record (N MB needed)."), staging TTL 7 d, export TTL 15 min, list default 100 max 500, newest first. Error mapping: RecordsBusy -> busy 1000 ms, LookupError -> not_found, ValueError -> bad_request. | `host_store.py`, `host.py:_records_request` | undecided |
| A17-47 | Record kinds | Dictation: entry fields + `entry_ext` + one WAV under `audio/`; host stores audio in `recordings/devices/<id>/`, applies the device's retention limits. Meeting: session + chunks/segments/participants/state_items/questions/events rows + audio folder; only `ended`/`failed`; update sends only changed files. | `kinds.py` | undecided |
| A17-48 | Client outbox sync | `record_sync` table; copy/move/delete; backoff 5,15,60,300 s; 5 attempts then failed; digest-check every 10 min; viewing-copy checkout TTL 1 h; progress text "Sending a <kind> · N%" (<=4/s); waiting texts. | `sync.py` | undecided |
| A17-49 | Host records in History | `list_remote` (8 s timeout, 30 s quiet after unreachable "<host> isn't reachable right now."), `remote_item`, `delete_remote`, `audio_for` (cached until restart), `check_out`, `bring_back` ("Bringing back N…"). | `sync.py` | undecided |
| A17-50 | Record recovery | Host owner can assign an unpaired device's orphaned records to a paired device (`recoverable_record_owners`, `recover_device_records`); "These records already belong to a paired computer.", "Pair the receiving computer again before recovering its records.", "These records are no longer available for recovery." | `service.py`, `host.py:attach_record_owner` | undecided |
| A17-51 | Backup gate | `RecordGate.paused(30 s)` drains record requests; new ones get busy "The host is backing up or recovering records. Try again shortly."; "Another backup or record recovery is already running."; "Host record transfers are still finishing. Try again after sync finishes." Client side `paused_for_backup` (30 s): "Records are still moving to or from the paired host. Try backup again after sync finishes." | `gate.py`, `sync.py:paused_for_backup` | undecided |
| A17-52 | Direct history | Separate connection `hello purpose:"history"`; host `ready{capabilities{client_history:true}}` then `history_query{id, operation, params}`; client replies `history_result{id, result|error}`; max 8 pending per peer, 5 s timeout; ops allow-list and param schema (see Data); reconnect every 10 s, 60 s if host lacks capability. Error codes & texts in `ClientUnavailable`. | `remote_history/*` | undecided |
| A17-53 | LAN address classification | Interface list + route probe (UDP connect 192.0.2.1:9), classification lan/vpn/virtual/tailscale, ranking, `best_lan_address`, `discover_lan_ipv4`. Never raises. | `lan_address.py` | **port** (meeting web server needs it) |
| A17-54 | Whisper host engine | faster-whisper decode, beam/VAD config, hotwords; `whisper_language`. | `engines.py:WhisperEngine` | drop |

### Added by check

| ID | Feature | Exact behaviour | Source | Status |
|---|---|---|---|---|
| A17-55 | Discovery port is fixed | `apply_host_settings` always starts discovery on `DISCOVERY_PORT` (47821, = `DEFAULT_PORT`) whatever `remote_host_port` is; a host on another TCP port is still found by UDP 47821 and the reply carries the real `port`. Receive buffer `_DATAGRAM_MAX_BYTES=2048`; sweep takes at most `_SWEEP_NETWORKS=2` /24s. | `service.py:apply_host_settings`, `discovery.py:46-66` | undecided |
| A17-56 | Port change restarts host | `set_host_port` saves, and only if sharing is enabled re-applies: stops when `host.port != port` then restarts. `apply_host_settings` is serialised by `_apply_lock`; failures end up in `host_state().error`; always emits `state`. | `service.py:679-690` | undecided |
| A17-57 | `set_location` re-queues outbox | Changing `remote_records_location` rewrites pending/failed non-delete rows: to `local` drops rows that never reached the host (no `content_digest`), otherwise re-puts them as `move` (host) or `copy` (both) with state pending; then notifies and wakes the worker. Unknown value raises `ValueError("Unknown storage location: ...")`. Records already moved stay on the host. | `sync.py:set_location` | undecided |
| A17-58 | Host copy lookup / summary refresh | `copies_on_host(kind, ids)` = ids with a `done` row of action copy/move for the paired host's fingerprint (History uses it to badge "kept on both"). `refresh_summary()` opens and closes a connection purely to refresh `ready.records` counts. `listing_wanted(kind)`: false when unpaired; true when location != local; when local, true while host support is unknown or the host still holds a count for that kind. | `sync.py:208,789,800` | undecided |
| A17-59 | Host record fetch details | `_fetch` pulls `records_open` then `records_fetch` chunks into a temp dir; per-file size check ("<name> came back incomplete."); manifest must be an object whose `kind` matches ("The host sent a record this version can't read."). `audio_for` downloads into `.download-*` beside the cache, moves in whole with `os.replace`, per-record lock so concurrent callers share one download; no audio -> `RecordsUnavailable("That entry has no recording on the host.")`; failures to list -> `RecordsUnavailable(str(exc) or "The host didn't answer.")`. `list_remote` caches items as `stored_on=<host name>` for `remote_item`; skips items without a string `id`. | `sync.py:_fetch,_audio_for,list_remote` | undecided |
| A17-60 | Check-out bookkeeping | After importing a fetched record it writes a `done` outbox row (action `copy` if location is both, else `move`) with the record's digest (`"fetched"` if the digest fails), so an edit is sent back and an unedited viewing copy is dropped on next start. `delete_remote` also drops the row, forgets the cached item and emits `records`. | `sync.py:check_out,delete_remote` | undecided |
| A17-61 | Bring back semantics | Per kind, loops `records_list(limit 100)` (host has no paging, newest first), imports each, then `records_delete` on the host and drops the row; a record that fails import is skipped (logged) and, once only skipped ones remain, older ones stay on the host. Progress callback "Bringing back N…" (N counts from 1). Runs under `_transfer_lock`; stops on worker stop. | `sync.py:_bring_back` | undecided |
| A17-62 | Direct history error texts | `ClientUnavailable` messages: `sharing_disabled` "History sharing is disabled on this client."; `not_found` "Record not found on this client."; `invalid_query` "Invalid client history query."; `snapshot_unavailable` "This client's saved insights are unavailable."; `unsupported` "Direct client queries are not available on this server."; anything else (incl. unknown code) "Client history is unavailable. The client may be offline." Query model is strict with `extra=forbid`; per-operation field sets and required fields (`transcription` record_id, `meeting`/`segments`/`insights` meeting_id, `segment` meeting_id+segment_id, `search` q); `check` takes none. Host-side peer: `query` raises `sharing_disabled` if the client's opt-in is off, `unavailable` at 8 pending, timeout 5 s; a replaced peer is closed 1001 "history connection replaced"; broker not accepting -> 1001 "host stopped sharing"; the reply frame cap host-side is 8 MiB, the query frame cap client-side 64 KiB. The reader reports only the exception type in logs. | `remote_history/channel.py`, `client.py` | undecided |
| A17-63 | Tailscale CLI errors | `TailscaleError` wraps CLI failures; `status()` maps them to state `unavailable`/`not_installed` and never raises; `own_device(remote, local, owner)` is the pairing check (both sockets tailnet, same untagged owner). `DESKTOP_OSES = {windows, linux, macos}`. | `tailscale.py` | undecided |
| A17-64 | Pairing change signalling | Settings `ClientPairing.__eq__` ignores moved addresses so a connection is not rebuilt when only addresses change; `remote_model_request` / `remote_mcp_request` take `expected_pairing` and refuse if the paired host changed ("The paired host changed. ..."). | `settings.py`, `service.py` | undecided |

## Modules

### `services/remote_asr/protocol.py`
- Responsibility: constants and pure helpers shared by both roles. Docstring (lines 1-131) is the protocol spec; keep
  it as the C++ header comment.
- Constants: `PROTOCOL_VERSION=1`, `DEFAULT_PORT=47821`, `PATH="/openwhisper/v1/engine"`, `SAMPLE_RATE=16000`,
  `MAX_AUDIO_SECONDS=120`, `MAX_HEADER_BYTES=65536`, `MAX_REQUEST_BYTES`, `MAX_REPLY_BYTES=8 MiB`,
  `RECORD_CHUNK_BYTES=1 MiB`, close codes, `ENGINE_RELOADING`, `MAX_NAME=60`, `MAX_HINT_PHRASES=50`,
  `MAX_HINT_CHARS=64`, `PAIRING_NONCE_BYTES=32`, `SAS_DIGITS=6`.
- Edge cases: `unpack_frame` rejects non-binary, <4 bytes, length > 64 KiB or past end, non-JSON, non-object
  (`:236-252`). `header_phrases` silently drops invalid entries and stops at 50 (`:207-221`). `pairing_sas` encodes
  fingerprint as ASCII with `replace` (`:299-303`). `parse_address` count(":")==1 rule means unbracketed IPv6 is a
  bare host (`:336-341`).
- Pure: every function here is a golden-vector candidate.

### `services/remote_asr/tls.py`
- `ensure_host_identity(dir) -> HostIdentity{cert_path, key_path, fingerprint}`; `own_fingerprint(dir)` ("" when never
  shared); `server_context`, `client_context`, `peer_fingerprint(sock)` (raises if no cert).
- Key file created with `O_CREAT|O_TRUNC, 0600` (`:89`). Unreadable pair -> warn and regenerate (`:38-43`), which
  changes the fingerprint and orphans all pairings (clients then see the "different certificate" error).
- `num_tickets = 0` (`:100-105`) must be replicated (no TLS 1.3 session tickets) if the C++ client is threaded the
  same way; harmless otherwise.

### `services/remote_asr/host.py`
- Public: `DeviceRegistry(load, save)` (`list/add/rename/remove/attach_record_owner/authenticate`), `PairRequest`,
  `SpeechHost(engine_provider, registry, identity, host_name, on_event, tailscale_owner, addresses, models,
  select_model, model_management, manage_models, runtime, engine_settled, configure_runtime, records_enabled, records,
  records_summary, mcp_enabled, mcp, history, limits...)`. Methods: `start(port, bind, discovery_port) -> port`,
  `stop()`, `open_pairing(ttl) -> code`, `close_pairing`, `pairing_status -> (code, seconds_left)|None`,
  `pending_request -> {id,name,address,sas,seconds_left}|None`, `answer_request`, `connected_clients ->
  [{device_id,name,address,busy,since,recording}]`, `update_active/update_status/prepare_update/cancel_update`,
  `engine_changed -> int`, `remove_device`, `rename_device`, `activity`, `discovery`, `history`, `running`, `port`.
- Events (`on_event(kind, detail)`): `state`, `pairing{open}`, `pair_request{name,address}|{}`, `paired{name}`,
  `devices`, `clients`, `activity{name,busy}`, `engine{family,model,by}|{by}`, `mcp{by}`, `records{device,op}`.
- Threading: websockets sync server on daemon thread `RemoteEngineHost`, **one OS thread per connection**, handlers
  block on the engine. `_lock` guards clients/pairing/request/update fields; registry has its own lock.
- State machine per connection: handshake slot -> first JSON (10 s) -> probe | version check -> pair (code /
  tailscale) | pair_request (commit -> nonce -> reveal -> wait answer) | hello -> auth -> connection slot -> register
  client -> recheck identity -> `ready` -> request loop -> drop client (cancel streams, maybe reconnect grace).
- Edge cases to keep: hello while sharing stopped -> close 1001 (`:1089-1092`); register before `ready` and recheck
  identity (`:1104-1109`); revocation rechecked per frame including buffered ones (`:1114-1119`); `engine_changed`
  reply closes after sending (`:1125-1127`); busy refusal sent before close so the client reads `retry_after_ms`
  (`:725-732`, client `:608-613`); activity counted before busy flag clears (`:1313-1320`); stream failures logged at
  debug (`:1322-1324`); `stop()` closes all client sockets after `shutdown()` (`:508-519`); `_touch_locked` throttles
  `last_seen` writes (`:193-202`).
- `remote_address` "unknown" when not a tuple (`:747-748`). Request ids are client serials (ints) echoed back.

### `services/remote_asr/client.py`
- Public: `probe_host`, `pair_with_host`, `request_pairing(on_code, canceled)`, `RemoteConnection` (see A17-21..25),
  result types `PairingResult{token, device_id, host_name, fingerprint, alternates, via}`, `ProbeResult{host_name,
  protocol, engine, tailscale_pairing, fingerprint, approval, compatible}`.
- Constants: `CONNECT_TIMEOUT_S=6`, `FALLBACK_CONNECT_TIMEOUT_S=2.5`, `HANDSHAKE_TIMEOUT_S=10`, `PROBE_TIMEOUT_S=2`,
  `KEEPALIVE_INTERVAL_S=20`, `KEEPALIVE_TIMEOUT_S=20`, `_REQUEST_POLL_S=0.05`, `REDISCOVER_INTERVAL_S=30`,
  `APPROVAL_WAIT_S=150`.
- Process-wide statics: `_last_good{fp: host}`, `_searched_at{fp: t}` (class attributes, shared by all
  connections: dictation, meeting, history, sync).
- Threading: caller thread blocks; websockets reader thread; keepalive daemon thread per connection. `_lock` = one
  request in flight; `_state_lock` = socket/closed/serial.
- Signature parity: `request(op, timeout=180, audio=None, payload=None, **fields) -> dict` mirrors the local worker
  process's `request`, so `RemoteSpeechBackend` drives it the same way.
- `connect()` on a connection closed meanwhile raises "Transcription canceled" (`:627-630`).
- The client does **not** use websockets' own keepalive because it rounds to 15.6 ms on Windows (`:639-646`);
  a C++ port with a steady clock can use library keepalive if it exposes RTT.

### `services/remote_asr/service.py`
- `RemoteEngineService(backend_provider, identity_dir, on_client_changed, bind="0.0.0.0", switch_engine,
  engine_settled, configure_engine, on_host_catalog, records_root)`. Owned by the application controller (a03).
- Host role: `apply_host_settings()` (blocking; start/stop to match `remote_host_enabled`/`remote_host_port`; after
  start refreshes network profiles), `set_host_enabled`, `set_host_port`, `open_pairing`, `cancel_pairing`,
  `pending_pair_request`, `answer_pair_request`, `pairing_grants`, `remove_device(delete_records)`, `rename_device`
  (also re-badges stored records), `engine_changed`, `connected_clients`, `host_activity(events)`, `engine_state`,
  `host_state`, `set_tailscale_trust`, `set_keep_records`, `set_model_management`, `set_manage_mcp`,
  `update_active/prepare_update/cancel_update/update_status`, records host plumbing (`record_store`,
  `records_request` under `RecordGate`, `records_summary`, `recoverable_record_owners`, `recover_device_records`,
  `paused_records_for_backup`), `mcp_request`, `manage_host_models`, `host_models`, `runtime_state`,
  `configure_runtime`, `select_model`.
- Client role: `client_pairing`, `pair(address, code, tailscale)`, `request_pairing(address, on_code, canceled)`,
  `pending_pairing_name`, `retry_pairing_save`, `remote_model_request`, `remote_mcp_request`, `rename_host`,
  `forget_host`, `start_client_history`, `set_share_history`, scans (`scan_tailnet`, `scan_lan(sweep)`,
  `scan_nearby(sweep)`), `tailscale_status(refresh)`.
- Engine cache: one `HostEngine` per backend object identity (`:407-414`).
- Locks: `_lock` (RLock, state), `_apply_lock` (one start/stop at a time, `_lock` not held during it),
  `_pairing_save_lock`, `_runtime_lock` (non-blocking try: concurrent switch/configure refused).
- Contract with controller (a04 `services/runtime/remote.py`): `switch_engine(family, model, device_name[, device])`
  selects on the UI thread and raises with a user reason; `engine_settled() -> bool`; `configure_engine(family,
  model, changes, device_name)`. Service polls `engine_settled` every 0.1 s up to 240 s.
- Bind default `0.0.0.0`; tests use 127.0.0.1 to avoid the Windows firewall prompt (`:158-160`).

### `services/remote_asr/discovery.py`
- Responder thread `RemoteEngineDiscovery`, socket timeout 0.5 s; ignores Windows ICMP-port-unreachable `OSError`
  (`:246-253`). `answer(data, sender)` is pure apart from rate limiting (injectable clock would help testing).
- `search(timeout, targets, stop_when)`: unbound socket with `SO_BROADCAST` (`:324-327`) so searching never makes the
  app a listener (no firewall prompt).

### `services/remote_asr/settings.py`
- `ClientPairing` frozen dataclass; equality ignores host, port, host_name, alternates, paired_name (identity =
  fingerprint + device_id + paired_at + via). `address`, `over_tailscale`, `tailscale_fallback`, `renamed`,
  `name_for`.
- `load_client_pairing` validation (`:98-128`): host str non-empty, fingerprint non-empty, port int 1..65535, via in
  routes else "code", alternates filtered (str, non-empty, != host).
- `remember_host_addresses` (`:212-249`): under `_PAIRING_LOCK`; ignore if fp differs; if reached == saved host,
  update port; elif saved host is an IP not in the listed addresses -> replace with first listed non-Tailscale
  address, else reached; known tailnet addresses kept unless the host now lists one; alternates = listed + reached +
  kept tailnet, deduped, minus host; write only if changed.
- `load_host_devices` reads only that key (perf note `:340-345`).

### `services/remote_asr/tailscale.py`
- `TailscalePeer{name, dns_name, address(v4), os, online, owner, tagged, mine, is_desktop}`,
  `TailscaleStatus{state, detail, name, dns_name, address, owner, tailnet, peers, running}`,
  `PeerIdentity{node, owner, tagged}`. `status()` never raises. `whois` treats owner `tagged-devices` as tagged.
  Self tagged -> no owner. Subprocess with `CREATE_NO_WINDOW`, stdin null.

### `services/remote_asr/engines.py`
- `HostEngine` interface: `identity: tuple`, `accepts_phrases`, `describe() -> {family, model, label, device,
  streaming, available, status[, compute_type]}`, `transcribe(audio, language, phrases=()) -> {text, segments[{text,
  start, end}]}` (+ whatever the worker returns), `stream(session, audio, language, finish) -> {events}`,
  `cancel_stream(session)`.
- `UnavailableEngine(reason)` identity `("unavailable", reason)`. `host_engine_for(backend)`: None -> "No
  transcription engine is selected on the host"; remote backend -> "The host is itself using a remote engine. Select
  a local engine there."; cloud -> "The host's selected engine runs in the cloud and can't be shared. Select a local
  engine there." Provider exception -> "The host's engine is unavailable"; provider None -> "No engine is selected on
  the host" (`host.py:713-719`).
- `SpeechWorkerEngine` identity = ("worker", backend_id, model, device, saved device, saved language); serialized by
  the backend's decode lock (shared with host's own dictation).

### `services/remote_asr/runtime.py`, `dependencies.py`, `model_management.py`
- See A17-28..30. Validation strings in `runtime.py:89-125` ("Choose a device or precision supported by the host.",
  "Select a local speech engine on the host first.", "That device isn't ready on the host. Check its hardware and
  installed runtimes.", "This engine doesn't expose a host language setting.", "That precision isn't supported by
  the selected device on the host.", "This engine manages its own precision on the host.", "Choose a language
  supported by this engine on the host."). Persist to `LOCAL_ASR_DEVICES[family]` + `LOCAL_ASR_LANGUAGE`.
- `HostModelManager` / `HostRuntimeInstaller`: one job each, daemon worker, `on_changed` -> service notifies
  "models"/"components"; claims through `hf_access_coordinator` / `component_coordinator` (a14).

### `services/remote_asr/activity.py`
- `HostActivity(clock, max_events)`; thread-safe; `empty_snapshot(since)` shape is consumed by a18's dashboard.

### `services/remote_asr/reachability.py`
- `parse_profiles` accepts dict or list, NetworkCategory as int (0/1/2) or name; `host_notes(lan, addresses,
  profiles, discovery_error) -> [Note{kind, message, action, url}]` (pure).

### `services/remote_asr/mcp_link.py`
- See A17-44. `_SETTING_FIELDS` maps MCP `SettingsKey`s to configure field names; `HostMcpSettings.save_setting`
  ignores `MCP_ENABLED` (start/stop carry it), raises `KeyError` for unknown keys; `mutate_settings` diffs
  `MCP_WRITABLE_SETTINGS` and sends only changed grants. One worker thread `remote-mcp-link` at a time; pending
  changes win over a fetch.

### `services/lan_address.py`
- `LocalAddress{address, interface, kind, prefix, physical, private}`; `local_ipv4_addresses()` ranking
  (LAN<VPN<TAILSCALE<VIRTUAL, private first, default-route address, physical); `best_lan_address()` (LAN or VPN);
  `discover_lan_ipv4()`.
- Windows: `GetAdaptersAddresses(AF_INET, skip anycast|multicast|dns)` retry on ERROR_BUFFER_OVERFLOW (111) x4; skip
  OperStatus != Up and loopback; VPN if IfType PPP(23)/PROP_VIRTUAL(53)/TUNNEL(131) or description match; physical if
  IfType 6/71. POSIX: getifaddrs, BSD sockaddr layout on macOS; Linux sysfs `/sys/class/net/<if>/type` in
  {512,768,776,778,65534} or `tun_flags` -> VPN; `device` exists -> physical; else name prefixes / p2p / /32.
- Hostname resolution only when no interface listing (can block; `:355-358`).

### `services/remote_records/gate.py`
- `RecordGate.request()` context (raises `RecordsBusy` while paused), `paused(timeout=30)` context.

### `services/remote_records/host_store.py`
- `HostRecordStore(root, kinds)`; `handle(op, header, payload, device)`; `begin/put/commit/abort/open/fetch`,
  `delete_device`, `rename_device`, `summary`. Per-upload locks in a `WeakValueDictionary` (`:189-194`).
- Validation regexes: record id `^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$`, device `^[A-Za-z0-9_-]{1,64}$`, path segment
  `^[A-Za-z0-9][A-Za-z0-9._-]{0,119}$`, depth <= 4, name <= 400 chars, sha `^[0-9a-f]{64}$`. Limits:
  `MAX_RECORD_FILES=50000`, `MAX_RECORD_BYTES=64 GiB`, `MAX_MANIFEST_BYTES=256 MiB`.
- Upload state machine: `begin` (re-stage if files/bytes changed; return received sizes + done list; check free
  space) -> `put` chunks (declare file once, size/sha must not change; offset must equal current size else
  `{resume:true}`; fsync on last chunk; hash mismatch deletes file -> "<name> arrived damaged; it will be sent
  again.") -> `commit` (all declared done, sizes sum, has record.json, kind matches -> `kinds[kind].import_record`
  -> remove staging). `begin` prunes staging older than 7 d except the current one.
- Fetch: `open` writes `exports/.../record.json` + `index.json {name: abs path}` (audio served in place), prunes
  exports > 15 min; `fetch(name, offset)` -> `{data b64 (<=1 MiB), offset, size, eof}`; expired -> "Open the record
  again; its export has expired."

### `services/remote_records/kinds.py`
- `Bundle{record, files{name: path}}`, `manifest_bytes()` compact UTF-8 JSON, `ensure_ascii=False`.
- `_row_from_json` + `_coerce`: untrusted JSON -> SQLAlchemy column types (bool strict, int not bool, float, binary
  as `{"$b64": ...}`, Text <= 2 MiB / String <= 4096); unknown fields refuse unless `ignore_unknown`.
- `DictationRecords(recordings_folder, database)`: record `{kind:"dictation", format:1, entry{_ENTRY_FIELDS},
  entry_ext{entry_kind, cleanup_level, cleaned_text, language}, audio: "audio/<file>.wav"|null, [retention
  {max_recordings, max_bytes}]}`. Import checks id, required text/timestamp/model, id collision across origins, WAV
  magic, keeps an existing entry's audio filename, otherwise `_free_path` numbering `stem-2.ext`. Host retention via
  `HistoryManager.recordings_over_limit` on the device folder only. Failed-recording entries are never sent
  (`local_ids`).
- `MeetingRecords(meetings_root, repository, busy, in_use)`: record `{kind:"meeting", format:1, update, session (minus
  host_token, guest_token, app_pid, app_heartbeat_at, origin_device_id, origin_device_name, spool_dir), chunks
  (file_path basename), segments, participants, state_items, questions, events, files{audio/<name>: size}}`. Skips
  `*.pcm, *.recovery.json, *.orphan, *.tmp, playback.wav, playback.json`; only chunk-referenced files and
  `*_session.wav|json`. `ready()` reasons: "it hasn't finished", "it is still being processed", "its audio is still
  being transcribed", "its insights are still being written", "its insights are still being reviewed". Import: new
  host/guest tokens, spool `meetings/<id[:12]>` (or `-<6 hex>` if taken), `_SpoolSwap` with `.deleting-<id>-<hex>`
  tombstone and rollback; chunk ids remapped via `chunk_ref`; segments pointing to unknown chunks get `chunk_id
  None`; update prunes audio of dropped chunks. `_meeting_summary` strips tokens.

### `services/remote_records/sync.py`
- `RecordSync(kinds, connect, cache_dir)` singleton `record_sync` (lazy). Hooks: `record_saved`, `record_edited`,
  `record_deleted`, `cleared(kind, keep)` (row id `*`), `send_existing`, `set_location`. Worker thread
  `record-sync`: on start clears the audio cache, then `run_once(startup=True)`, then wakes every 60 s or on
  `wake()`. `run_once` under `_transfer_lock`.
- One pass: no pairing -> clear status; retarget pending rows without digest to the current host fp; digest-check
  meetings kept on both every 10 min; collect outbox rows (+ failed if `wake(now=True)`) and due checkouts (all on
  startup, else older than 1 h and not in use); respect retry backoff; open a **separate** connection; host without
  `records` capability -> waiting "Update OpenWhisper on <host> to keep records there.".
- `_process`: delete rows -> `records_clear` (300 s) or `records_delete` (60 s); missing record -> drop row; not
  ready -> waiting "Waiting to send a <kind>: <reason>."; host not keeping -> waiting "<host> isn't keeping records
  for other computers yet. Turn on \"Keep records for paired computers\" in its Settings → Remote engine." and stop
  pass; digest before upload; upload; `_uploaded` marks done, re-queues if edited during upload, deletes local copy
  (quietly) for move unless in use.
- `_record_failure`: forbidden -> host_keeps False and stop pass; busy / connection errors stop pass without counting;
  else attempts+1, failed at 5, `last_error[:500]`.
- `upload`: meeting -> `records_stat` (30 s) and drop unchanged files (same size), set `update:true`; items sorted +
  record.json last; on `bad_request` once -> `records_abort` and retry; `records_commit` (300 s). Timeouts: begin
  60 s, put 120 s.
- `SyncStatus{location, paired, host_name, host_keeps, host_supports, pending, waiting, active, error, stored,
  local_counts}`. `listing_wanted(kind)`.

### `services/remote_history/channel.py`, `client.py`
- Host: `HistoryBroker(registry)`: `start`, `serve(ws, device, token, enabled)` (replaces an existing peer of the
  same device, closes old with 1001 "history connection replaced"), `clients() -> [{device_id, name,
  status online|sharing_disabled|unavailable}]`, `query(device_id, op, params)` -> result or `ClientUnavailable`,
  `remove(device_id=None)` (None also stops accepting). Frames > 8 MiB or non-`history_result` end the session;
  auth rechecked per frame.
- Client: `HistoryClient(database, pairing, token, enabled)` loop thread `client-history`; `refresh()` closes the
  current connection to reconnect with new settings; `HistoryReader(database, enabled)` re-checks the opt-in
  before and after each query; results `model_dump(mode="json")` from a20's `HistoryStore`; oversize reply (> 8 MiB)
  replaced by `error: unavailable`; incoming query frame must be <= 64 KiB.

## Data and formats

### Settings keys (all `in_table=False`, defined in `services/settings.py:742-776`, a10)

| Key | Type | Default | Read by |
|---|---|---|---|
| `remote_engine_client` | optional dict `{host, port, fingerprint, host_name, device_id, paired_at, alternates[], via, [paired_name]}` | absent | `settings.py:load_client_pairing` |
| `remote_host_enabled` | bool | false | `host_enabled` |
| `remote_host_port` | int 1..65535 | 47821 | `host_port` |
| `remote_host_devices` | list of device entries (A17-08) | [] | `load_host_devices` |
| `remote_host_tailscale_trust` | bool | **true** | `host_tailscale_trust` |
| `remote_host_model_management` | bool | false | `host_model_management` |
| `remote_host_keep_records` | bool | false | `host_keeps_records` |
| `remote_host_manage_mcp` | bool | false | `host_manages_mcp` (backup restore must never carry it over, test_remote_mcp_control) |
| `remote_records_location` | choice local/host/both | local | `records_location` |
| `remote_client_history` | bool | false | `client_shares_history` |
| Also read/written: `local_asr_devices`, `local_asr_language`, `whisper_device`, `whisper_compute_type`, MCP keys | | | runtime.py, mcp_link.py |

### Credential store
- `OPENWHISPER_REMOTE_ENGINE_TOKEN` (installed app data dir) or `OPENWHISPER_REMOTE_ENGINE_TOKEN_<12 upper hex>`
  (other data roots). Through `services/credentials.py` (`resolve_credential`, `store().set/delete/request_access`).

### Database (a09/a14 schema)
- `record_sync(kind PK, record_id PK, host_fingerprint, action copy|move|delete, state pending|done|failed, attempts,
  last_error, content_digest, created_at, updated_at)`, index on `state`. Record id `*` = clear all of a kind.
- `transcription_history.origin_device_id/origin_device_name` (NULL = own); meeting session table same two columns.
  DB helpers used: `get_history_entry_by_id`, `get_history_entries(limit, origin=)`, `search_history_entries`,
  `put_history_entry`, `delete_history_entry`, `clear_history_audio_file`, `rename_history_origin`,
  `history_origin_counts`; meeting repo: `get_meeting`, `list_meetings`, `export_meeting_rows`,
  `replace_meeting_rows`, `list_past_meeting_summaries(origin=)`, `origin_spools`, `rename_origin`, `record_owners`,
  `count_unfinished_chunks`.

### Files on disk (data root = `config.user_data_path`)
- `remote_engine_host_cert.pem`, `remote_engine_host_key.pem`.
- `remote_records/staging/<device>/<kind>-<record>/{upload.json, files/...}`; `upload.json` = `{kind, record_id,
  files, bytes, declared{name:{size,sha256}}, done[], started}` written atomically via `.tmp` + replace.
- `remote_records/exports/<device>/<kind>-<record>/{record.json, index.json}`.
- `remote_records/cache/<record_id>/...` (client audio cache, cleared at worker start), `.download-*` temp dirs,
  `remote_records/tmp*` for checkout/bring back.
- Host-stored dictation audio: `<RECORDINGS_FOLDER>/devices/<device_id>/<name>.wav` (`audio_file` stored relative,
  forward slashes). Host-stored meetings: `<MEETINGS_FOLDER>/<id[:12]>[-xxxxxx]/`.

### Wire protocol summary
- TCP/TLS WebSocket, text frames = JSON control/replies, binary frames = requests. First message types: `probe`,
  `pair`, `pair_request`, `hello`. Server messages: `probe`, `paired`, `pair_commit`, `pair_reveal`, `ready`,
  `error{code, message, [retry_after_ms]}`. Error codes seen: `protocol`, `unauthorized`, `busy`, `pairing_closed`,
  `bad_code`, `tailscale_refused`, `approval_refused`, `approval_limited`, `approval_busy`, `pair_denied`,
  `pair_closed`, `pair_timeout`, `bad_request`, `engine_changed`, `engine_reloading`, `forbidden`, `unsupported`,
  `not_found`.
- Records ops and fields: `records_stat{kind, record_id} -> {stored{name:size}}`; `records_begin{kind, record_id,
  files, bytes} -> {received{name:size}, done[]}`; `records_put{kind, record_id, name, size, sha256, offset}+payload
  -> {received, done, [resume]}`; `records_commit -> {stored:true, audio|files}`; `records_abort -> {}`;
  `records_list{kind, query<=200, limit} -> {records[]}`; `records_open -> {files[{name,size}], bytes}` (record.json
  first); `records_fetch{name, offset} -> {data, offset, size, eof}`; `records_delete -> {deleted}`;
  `records_clear{kind} -> {deleted:n}`. `ready.records` = `{dictation:{count,bytes}, meeting:{count,bytes}}`.
- Direct history ops: `check`, `transcriptions`, `meetings` (q, limit 1..100 default 20, cursor <=2048, since,
  before), `search` (q required 1..500, kind all|meeting|transcription), `transcription{record_id}`,
  `meeting{meeting_id}`, `segments{meeting_id, limit, cursor, start_s, end_s}`, `segment{meeting_id, segment_id}`,
  `insights{meeting_id}`. Ids 1..200 chars, no `/\?#` or controls. Dates must have timezone, converted to UTC;
  since<before, start<end. Error codes: unavailable, sharing_disabled, not_found, invalid_query,
  snapshot_unavailable, unsupported.
- UDP discovery on 47821 (A17-34).
- **Compatibility**: an existing OpenWhisper install depends on: path, magic strings, SAS/commit domain strings,
  close codes, JSON field names, cert/key filenames, credential name, settings key names and the `record_sync`
  table. Keep all byte-identical if OpenWhisper_CPP should pair with OpenWhisper computers or take over an existing
  data dir.

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| `websockets` (sync server/client, ping/pong, close codes, max_size, max_queue) | Transport | Qt WebSockets (`QWebSocketServer` SecureMode, `QWebSocket`) with `QSslConfiguration`; or Boost.Beast / IXWebSocket over OpenSSL if a thread-per-connection blocking model is preferred |
| `ssl` | TLS contexts, peer DER | `QSslSocket`/`QSslConfiguration` (`setPeerVerifyMode(VerifyNone)`, `peerCertificate().digest(Sha256)`, protocol `TlsV1_2OrLater`); no ticket control in Qt: verify, or use OpenSSL `SSL_CTX_set_num_tickets(0)` |
| `cryptography` (EC P-256 self-signed cert) | Host identity | OpenSSL `EVP_PKEY_EC`/`X509_*` (vcpkg `openssl`); Qt cannot generate certificates |
| `hashlib`, `hmac.compare_digest`, `secrets` | SHA-256, constant-time compare, tokens/nonces/codes | `QCryptographicHash`/OpenSSL `SHA256`, `CRYPTO_memcmp`, `QRandomGenerator::system()` or `RAND_bytes`; urlsafe base64 no padding for tokens |
| `socket` (UDP broadcast, TCP knock) | Discovery, sweep | `QUdpSocket` (`ShareAddress`, broadcast), `QTcpSocket` with 0.4 s timeout, or plain sockets in the Qt-free core |
| `ctypes` + `getifaddrs` / `GetAdaptersAddresses` | Interface list | Direct `getifaddrs` / `GetAdaptersAddresses` calls; `QNetworkInterface` gives names/prefix/type but not Windows IfType description detail reliably; keep native |
| `ipaddress` | Private/link-local/CGNAT checks | `QHostAddress::isPrivateUse/isLinkLocal/isLoopback/isInSubnet` |
| `subprocess` | `tailscale` CLI, PowerShell `Get-NetConnectionProfile` | `QProcess`; Windows profiles natively via `INetworkListManager` (COM) instead of PowerShell |
| `threading`, `ThreadPoolExecutor`, `deque`, `Condition`, `WeakValueDictionary` | Concurrency | `std::thread`/`QThreadPool`, `std::condition_variable`, `std::deque`, `std::weak_ptr` map |
| `json` | Everything | `QJsonDocument` or nlohmann/json in the core |
| `numpy` | PCM conversion | plain loops over `int16_t` |
| `sqlalchemy` (column introspection for coercion) | Record import | explicit per-table field schemas in C++ |
| `pydantic` | History query validation | hand-written validator |
| `shutil.disk_usage`, `os.fsync`, `os.replace` | Store | `std::filesystem::space`, `QFile::flush`+`fsync`/`FlushFileBuffers`, `std::filesystem::rename` / `QSaveFile` |
| `services.credentials` (keyring) | Token | a10's C++ credential store (Windows Credential Manager / Keychain / libsecret, e.g. QtKeychain) |

## Platform-specific code
- Windows: `GetAdaptersAddresses` classification; network profile category via PowerShell (`reachability.py:80-99`)
  and `ms-settings:` links; ICMP port-unreachable `OSError` on UDP recv ignored (discovery `:246-253`, `:378-381`);
  per-card broadcast because 255.255.255.255 leaves one card only (`:291-296`); firewall: unicast replies to a
  broadcast allowed 3 s, so the searcher needs no rule; `num_tickets=0` stall fix (tls `:100-105`); Tailscale CLI in
  Program Files; `CREATE_NO_WINDOW`; tests bind 127.0.0.1 to avoid firewall prompt.
- macOS: getifaddrs BSD sockaddr layout (`lan_address.py:135-141`); Tailscale app binary needs `TAILSCALE_BE_CLI=1`
  (`tailscale.py:126-129`); Keychain recovery (`retry_pairing_save`, "Open Keychain Access" in a18); Apple Silicon
  runtime branches (drop).
- Linux: sysfs tunnel/physical detection; Tailscale under /usr/bin, /usr/local/bin, /snap/bin. Omarchy/Hyprland
  updater calls `update_status`/`cancel_update` via `services/omarchy_controls.py:216-224` (a04 calls
  `prepare_update` from `services/runtime/transcription.py:316`). No X11/Wayland-specific code here.

## Engine-specific code
- Drop: `engines.py:WhisperEngine`, `whisper_language`, Whisper part of `host_models` (`:214-221`); `runtime.py`
  CTranslate2 / compute types / `parakeet_mlx` / Apple Silicon / Moonshine / Vulkan branches; most of
  `dependencies.py` (component ids per backend, CUDA compute capability check); `model_management.model_choices`
  Whisper entries; `SpeechWorkerEngine` (Python worker process plumbing).
- Adapt for in-process Parakeet: a `HostEngine` implementation that forwards `transcribe(audio, language, phrases)` to
  `EngineWorker` via a queued call and **blocks the connection thread on a future** (never call `Recognizer` from a
  network thread; CLAUDE.md). It must share the decode queue with the host's own dictation (the Python worker's
  decode lock did this). `identity` = (family, model path/id, device cpu|gpu runtime, language); `describe()` keys
  unchanged. `stream` -> "The host's engine has no live stream" (Parakeet TDT is offline-only; clients preview with
  `transcribe` windows). `accepts_phrases` = whether OpenWhisper_CPP's Parakeet path uses word boosting (likely false ->
  `recognition_hints:false`). Nemotron Streaming (same runtime, undecided) would implement `stream`.
- `runtime_state` reduces to device choice between installed NeMo CPU/GPU runtimes (loaded by path, never unloaded:
  a runtime switch may need restart, cf. `restart_required`).

## Dependencies on other areas
- Calls out: a10 `services/settings.py` (`SettingsKey`, `settings_manager.load_settings/save_setting/update_settings`),
  a10 `services/credentials.py`; a01 `config.py` (`user_data_path`, `data_root`, `local_app_dir`, `config.*`
  folders/Whisper options); a14 `services/local_asr/catalog.py` (MODELS, BACKENDS, runtime_id, resolve_runtime,
  selected_device), `services/local_asr/cache.py`, `services/local_asr/languages.py`, `services/components.py`,
  `services/component_catalog.py`, `services/component_runtime.py`, `services/hf_access.py`, `services/gpu_info.py`,
  `transcriber/optional_backend.py`, `transcriber/local_backend.py`, `services/models.py`; a09
  `services/database.py`, `services/history_manager.py` (delete_entry `kept_on_host`, `HistoryManager` retention);
  a28 `meeting/persist/repository.py`, `meeting/persist/data_lifecycle.py`; meeting web `meeting/web/auth.py:
  generate_token_pair`; a20 `services/agent_api/store.py` (`HistoryStore`, `InvalidQuery`, `NotFound`,
  `SnapshotUnavailable`), `services/agent_mcp/host_control.py` (`HostMcpControl`, `PERMISSIONS`),
  `services/agent_mcp/runtime.py` (`runtime`, `DEFAULT_PORT`, `ServerStatus`); `services/recognition_context.py`
  (hotwords, drop).
- Called by: a03 `services/application_controller.py:299-321, 753-754, 916, 1340` (constructs service and
  `record_sync`, backup pauses, start/stop); a04 `services/runtime/remote.py` (switch/configure/validate, sync wake),
  `services/runtime/transcription.py:316,1677`, `services/runtime/meeting.py:1070,1937,2211` (record hooks); a14
  `transcriber/remote_backend.py` (RemoteConnection, pairing, errors, protocol); a25 `meeting/asr/remote.py`
  (meeting's own connection, own fingerprint to avoid self-pairing); a18 `ui_qt/dialogs/settings_remote.py`,
  `ui_qt/widgets/host_dashboard.py`, `ui_qt/dialogs/pair_request_dialog.py`; a21 `ui_qt/dialogs/settings_mcp.py`
  (mcp_link); a09 `services/history_manager.py:600-635`, `services/history_calendar.py:400`,
  `ui_qt/widgets/history_sidebar.py:165`, `history_playback.py:264`, `history_calendar_dialog.py:1036`,
  `past_meetings_panel.py:47`, `ui_qt/ui_controller.py:580-585`; a20 `services/agent_api/app.py`, `federation.py`
  (`ClientUnavailable`), `services/agent_mcp/app.py:594`, `events_ext.py:430`, `runtime.py:38` (tailscale); a26/meeting
  web `meeting/web/server.py:47` (`discover_lan_ipv4`); `services/omarchy_controls.py`; `scripts/bench_dictation_overhead.py`.

## Tests
Fakes: `tests/fakes/remote.py`. Most tests run a real TLS host on 127.0.0.1 with port 0 and discovery port 0.

| File | Count | Pins |
|---|---|---|
| `test_remote_engine.py` | 103 | Frame round trip and rejects, `parse_address`, short fp, identity reuse, token stored as digest, wrong codes close pairing, unknown token, cert pinning before token, wrong path, transcribe round trip, per-connection streams, engine switch pushes reconnect, device removal, cross-thread close, keepalive RTT/close, unreachable wording, refusing remote/cloud engines, recording reservation + update deferral, reload refusal, cancel keeps connection, timing split (`host_ms`), model list/switch semantics, Tailscale status parsing/ownership/never raises/CLI paths, probe has no secrets, Tailscale pairing allow/refuse, alternates, scan_tailnet, busy retry. Many also cover a14/a18 (`remote_backend`, settings page). |
| `test_remote_pairing_ux.py` | 44 | SAS equality only through the same cert; query padding / answer size; nonce/fp validation; allowed senders; responder rate limit; taken UDP port; sweep; probe fp+approval; LAN+tailnet merge; own answer excluded; approval flows (allow, deny, cancel, one at a time, rate limit, outside network, timeout, stop during wait, 2 s grace); broken commitment; `remember_host_addresses` cases; pairing equality after move; rediscovery; forged discovery gets no token; PowerShell profile parsing; reachability notes; page/dialog tests (a18). |
| `test_remote_host_admission.py` | 9 | Connection cap busy + retry_after, handshake slots, FIFO job queue, disconnected waiter releases slot, `host_ms` includes queue wait, records bypass pool, meeting connect busy. |
| `test_remote_engine_push.py` | 10 | Engine-change push, handshake race, identity tuples (device, compute type), share line, VPN label. |
| `test_remote_records.py` | 25 | `safe_name`, payload frames, ready counts, host/both/local modes, lock cleanup, storage off still readable, isolation between devices, damaged file resend, resume, clear, bring back, retention, meeting move/wait/in-use/edit-without-audio/checkout, traversal refused, delete/rename device. |
| `test_host_backup_recovery.py` | 5 | Orphan recovery assignment, removal with records, busy during backup never burns attempts, gate drain/timeout. |
| `test_client_history.py` | 9 | Direct queries, pagination, allow-list, permission default off, revocation, multiple clients, timeout, auth, unsupported. |
| `test_remote_model_management.py` | 17 | Opt-in boolean, per-request recheck, field allow-lists, bundled only, catalog safety, download lifecycle and claims. |
| `test_remote_dependencies.py` | 16 | Runtime install options/lifecycle (mostly drop). |
| `test_remote_runtime.py` | 18 | runtime_state/validate_runtime bounds, reload wait, CPU fallback (mostly drop/adapt). |
| `test_remote_mcp_control.py` / `test_remote_mcp_link.py` | 15 / 10 | MCP opt-in, rechecks, configure validation, link polling/optimistic/rollback/facades. |
| `test_remote_rename.py` | 12 | Device and host rename semantics. |
| `test_remote_registry.py` | 3 | Shared registry between history and host; `last_seen` serialization. |
| `test_remote_token_scope.py` | 4 | Credential name scoping and legacy fallback. |
| `test_remote_keychain_recovery.py` | 5 | Pending pairing retry / forget. |
| `test_lan_address.py` | 11 | Classification on Windows/Linux/macOS fixtures, ranking, route probe fallback, no hostname resolution when listed. |
| `test_remote_tailscale_cli.py` | 1 | macOS `TAILSCALE_BE_CLI`. |
| Others touching the area | | `test_meeting_remote.py`, `test_personalize_s9_remote_records.py`, `test_personalize_fix*_*`, `test_backup_record_sync.py`, `test_history_calendar*.py`, `test_mcp_host_tabs.py`, `test_settings_polish.py`, `test_typed_settings.py`. |

Golden-vector candidates (pure, deterministic): `protocol.pack_request/unpack_frame/encode_audio/decode_audio`,
`header_phrases`, `parse_address/format_address`, `short_fingerprint`, `pairing_commitment/pairing_sas/format_sas`,
`clean_name`; `discovery.make_query/parse_query/encode_reply/parse_reply/allowed_sender`; `tailscale.parse_status`
(+ `is_tailscale_address`); `reachability.parse_profiles/host_notes`; `lan_address.classify` and ranking given a
fixed interface list; `settings.remember_host_addresses` (as a pure function of pairing + inputs);
`host_store.safe_name/safe_record_id/_chunk_fields`; history `_query_values`; `activity.HostActivity` with a fake
clock.

## Porting notes
- **Decision first**: whether the remote-computer engine is kept. If kept, decide whether OpenWhisper_CPP must
  interoperate with OpenWhisper computers (then every string in "Compatibility" stays byte-identical, including the
  `openwhisper` path, magics and credential name).
- Decomposition:
  - Qt-free core lib (`src/remote`, namespace `fv::remote`): protocol codec, SAS/commit, address parsing, discovery
    codec, Tailscale JSON parser, reachability notes, LAN classification, registry logic, activity, record store
    (filesystem + validation), record bundle/import schemas, history query validator. All unit-testable with
    golden vectors.
  - Transport layer: either Qt Network + Qt WebSockets in the app library (event loop on a dedicated `QThread`, engine
    calls marshalled to `EngineWorker`), or Boost.Beast/OpenSSL with a thread per connection mirroring Python (simpler
    one-to-one port of the blocking handlers, admission semaphores and FIFO pool). Cert generation needs OpenSSL in
    either case.
  - Service facade (`RemoteEngineService`) as a QObject emitting a `changed(QString kind)` signal instead of
    listeners on arbitrary threads (UI no longer has to re-post).
  - Record sync and history client as worker QThreads; DB access through a09's C++ store.
- Order: protocol + TLS identity + pinning client/host for `transcribe` only -> code pairing -> registry/settings/
  credential -> admission/busy -> ready/engine identity push -> discovery + LAN address -> Tailscale -> approval SAS
  -> activity/host_state -> model switch -> records store + sync -> direct history -> MCP control -> update hold.
- Risks: (1) one-thread-per-connection blocking design versus Qt's async model; the FIFO admission, 30 s queue wait,
  `still_connected` polling and 2 s stop grace must be re-expressed carefully. (2) TLS details: self-signed cert
  generation, no hostname checks, pin before sending token, TLS 1.3 tickets. (3) Engine threading: decoding must go
  through `EngineWorker`, while the host also dictates locally; identity changes must close idle clients. (4) Records
  import writes into history/meeting DB tables owned by other areas; schema coercion must be as strict as
  `_row_from_json`. (5) Wire compatibility with live OpenWhisper peers. (6) Large surface for little use if the
  owner only wants local Parakeet.
- Rough C++ size: core ~3 500, transport/host/client ~2 500, service ~1 000, records (store+kinds+sync) ~2 300,
  history ~400, MCP link ~250, LAN address ~400 -> about **10 000** lines without tests; ~6 500 if model management,
  runtime install and MCP control are trimmed for Parakeet-only.
- Native gains: in-process engine removes the worker-process hop on the host (lower `host_ms`); steady-clock RTT
  without a custom heartbeat; zero-copy PCM16 framing; native `INetworkListManager` instead of spawning PowerShell
  (~0.3 s); `getifaddrs`/`GetAdaptersAddresses` without ctypes; per-request registry auth can cache token digests in
  memory instead of re-reading settings (Python note at `settings.py:340-345` measured 0.7-3.9 ms per request).
