# Meeting state schema, patches, store, repository, exports

Area `a28-meeting-state-persist-export`. Source root `D:\coding\whisper_local`. Files (20, ~6.9k
lines; `meeting/persist/repository.py` is 1549 lines, larger than areas.json counts):

`meeting/state/{schema,patches,store,review,custom_reports,repair,segment_ops}.py`,
`meeting/persist/{repository,data_lifecycle}.py`, `meeting/export/{__init__,bulk,json_export,markdown,transcript_txt}.py`,
`meeting/{content,corrections,stored,highlights}.py`.

This area has **no widgets**. It is the Meeting Mode data core: the in-memory dashboard document
(`MeetingState`), the single op vocabulary every writer uses, the single-writer store with seq
numbering/undo/fan-out, the SQLite repository for every `meeting_*` table, deletion of meetings
and their audio spools, reversible transcript corrections, history-row helpers, and the Markdown /
JSON / TXT exporters. UI (Past Meetings panel, export dialog, dashboard) lives in a31 / a09 / web
areas and only calls in.

## What the user gets

- A meeting dashboard whose every change (agent, host, guest, diarizer, undo) is validated,
  attributed, persisted to SQLite before it becomes visible, audited and undoable by the host.
- Human edits always win: anything a human edited, confirmed or pinned, and spoken voice-command
  notes, can no longer be changed or removed by the AI agent; human speaker pins cannot be
  reverted by the diarizer; human participant names cannot be overwritten by the agent.
- Reversible transcript corrections: meeting-wide term fixes ("Kubernetes" for "cooper netties")
  and one-occurrence fixes on a single passage, applied on read; raw ASR text is never rewritten,
  and deleting the note undoes it.
- Tailored "custom reports" asked for in the host's own words (lifecycle running/ready/failed,
  max 20 per meeting).
- Post-meeting insight review questions (accurate/incorrect/owner/deadline...) and the notes they
  produce.
- Deterministic repair after a meeting: timeline beats, topic, summary and title filled from
  existing synthesized key points/notes when the agent left them blank.
- Past Meetings list data: bounded, searchable, keyset-paged list with content summary (has
  audio / transcript, preview text), insights pill ("Ready", "Incomplete", "Saved for later",
  "Unavailable", "Off", "Failed start", "Empty"), title fallback ("Untitled meeting" /
  "Failed meeting").
- Safe deletion of one meeting or bulk clear, with the audio spool tombstoned so a DB failure
  never loses audio; orphan spool cleanup.
- Exports: Markdown (human document), JSON (complete machine record, tokens stripped), plain
  timestamped transcript TXT; bulk export of many meetings to one combined document or one file
  per meeting.
- Full-text transcript search across meetings (FTS5).

## Feature inventory

| ID | Feature | Exact user-visible behavior | Source | Disposition |
|---|---|---|---|---|
| A28-01 | MeetingState document + JSON round-trip | Fields and defaults listed under Data and formats; `report_views` default `["ribbon","brief","signal"]`; `capture` dict has 5 fixed keys; `to_dict()` key order is the wire/DB order | `meeting/state/schema.py:MeetingState` | port |
| A28-02 | Card items | 7 cards `key_points, decisions, action_items, risks, timeline, live_notes, user_notes`; status `proposed/edited/confirmed/removed`; `protected` = pinned or edited/confirmed or `author_type=system && author_id=voice_command` | `schema.py:CARD_KEYS, CardItem.protected` | port |
| A28-03 | Finalization state + history pill | Statuses `pending,running,completed,disabled,unavailable,failed`; pill precedence: live (active/paused/ending) none; deferred+incomplete "Saved for later"/warning; failed or completed-with-failed-step "Incomplete"/warning; unavailable "Unavailable"/warning; completed "Ready"/success; disabled "Off"/neutral | `schema.py:FinalizationState.history_pill` | port |
| A28-04 | Legacy/historical finalization normalization | Messages: "AI insights are off for this meeting.", "Final insights were not recorded for this meeting.", "Final insights were interrupted before they finished.", step detail "Interrupted before this step finished." | `schema.py:infer_legacy, coerce, normalize_historical` | port |
| A28-05 | Op vocabulary + per-op validation | Rejection reasons are stable strings (list under Data and formats); each op succeeds/fails independently; handler exceptions become `internal_error` | `meeting/state/patches.py:apply_ops,_HANDLERS` | port |
| A28-06 | Actor permissions | Agent ops only `AGENT_OPS`; agent must cite evidence on every op; `resolve_question`, `revise_segment_text` agent/system only (`agent_only`); `set_topic/set_rolling_summary/set_title/set_meeting_intent` refuse `user` (`host_only`) | `patches.py:AGENT_OPS, AGENT_ONLY_OPS, HOST_ONLY_OPS` | port |
| A28-07 | Item add with duplicate rejection | Agent add rejected `duplicate_item` when token Jaccard >= 0.60 or containment >= 0.75 (>=4 tokens) on same card; 0.75/0.90 across cards; live_notes vs other cards never compared; user_notes skipped; removed items skipped | `patches.py:_duplicates_live_item,_item_text_too_similar` | port |
| A28-08 | Limits | text 4000, topic 500, name 120, intent 2000, summary 8000, evidence refs 20, data JSON 4096 chars, 500 items/card, 200 participants, 7 open questions (agent) / 50 (human), occurrence index 0..1000, term 120 chars | `patches.py:1-62`, `corrections.py:23` | port |
| A28-09 | Item update / remove / pin / confirm | Agent update/remove need `base_revision == revision` (`revision_mismatch` with `current_revision`); human update sets status `edited`; evidence is unioned, not replaced; live_notes keep `heading`/`start_s` on partial data; review and citation_check cleared on update; remove is soft (`status=removed`) | `patches.py:_op_update_item,_op_remove_item,_op_pin_item,_op_confirm_item` | port |
| A28-10 | Timeline anchors | Agent/system timeline add requires numeric `data.start_s >= 0` (`missing_start_s`/`invalid_start_s`), coerced to float | `patches.py:_check_timeline_data` | port |
| A28-11 | Topic, rolling summary, title, intent, cloud flag | Topic appends history `{text,ts,evidence,actor_type}`; blank summary only by system; title via `services/titles.py` (1-200 chars, no control chars, stripped); intent blank = clear; `set_cloud_enabled` host/system | `patches.py:_op_set_topic.._op_set_cloud_enabled` | port |
| A28-12 | Participants | Agent mints only `others_cluster`, always provisional, cannot rename `name_source=human`; humans/app non-provisional; `me`/`guest` only by system; no removal | `patches.py:_op_upsert_participant,_op_suggest_participant_name,_op_rename_participant` | port |
| A28-13 | Quiet-inbox questions | Agent resolve: confidence >= 0.8 resolves (`answer_source=audio`), >= 0.4 sets suggestion, else `low_confidence`; human answer appends thread, resolves `answer_source=user`, clears suggestion; dismiss/reopen | `patches.py:_op_ask_question.._op_reopen_question` | port |
| A28-14 | Segment ops (speaker reassign, text polish) | Validated here, applied by store's segment handler; diarizer cannot overwrite a pinned segment (predicate failure fails closed); agent cannot polish a segment carrying an occurrence correction (`human_scoped_correction`); agent must cite the segment | `patches.py:_op_reassign_segment_speaker,_op_revise_segment_text`, `segment_ops.py` | port |
| A28-15 | Note-taker op filter | Only `add_item` to `live_notes` and update/remove of known live_notes ids survive | `patches.py:filter_notes_ops, live_note_ids` | port |
| A28-16 | Single-writer store | RLock; copy-on-write candidate; persist before adopt; seq += 1 per applied op; subscribers called under lock with `(max_seq, applied)`; persistence failure marks every applied op `persistence_error`, stale selection `stale_selection` | `meeting/state/store.py:MeetingStateStore.apply` | port |
| A28-17 | Host undo | Replays recorded inverse as `system` with `_undo_event_seq`; idempotent (`event_is_undone`); audit action `undo:<seq>` | `store.py:undo`, `repository.py:on_ops_applied,list_events,event_is_undone` | port |
| A28-18 | Runtime field updates | `update_runtime_fields(**)` persists status/finalization/etc. first; entering finalization `running` invalidates citation checks and open review questions | `store.py:update_runtime_fields`, `review.py:invalidate_checks` | port |
| A28-19 | Insight review ops | Consent `typesafe-text-v1`; strings "Checking the final insights…", "AI insights are off. Review was stopped.", "AI insights and insight review must both be on for this meeting.", "Waiting for the updated final insights."; answer labels (Confirmed as accurate / Agreed / Only offered, not agreed / Incorrect; removed / Risk resolved / Risk remains open / Corrected); text prefixes "Offered, not agreed: ", "Resolved: " | `meeting/state/review.py` | undecided backend (needs cloud "typesafe" reviewer) |
| A28-20 | Review notes and clarifications | Answer writes/rewrites a `user_notes` item "<label> — <text>[ Owner: X.][ Deadline: Y.]"; appends "\n\nUser clarification (supersedes earlier wording on this point): ..." to live_notes sharing evidence (max 4000 chars); later edit marks it superseded ("Earlier review, superseded by a later insight edit: ...") | `review.py:answer_review,_clarify_note_blocks,item_effect` | undecided backend |
| A28-21 | Custom reports ops | Host `request_custom_report` only when status ended and finalization not pending/running; one running at a time; cap 20; request <= 2000 chars; message "Reading the meeting and writing your report…"; worker actor `custom-report` finishes (markdown <= 60000, title <= 200, message <= 500 chars; `ready` requires non-empty markdown); host remove (also unsticks a crashed run) | `meeting/state/custom_reports.py` | undecided backend (LLM worker) |
| A28-22 | Deterministic state repair | Timeline backfill from key points (else notes), max 8 beats, text clipped to 180 chars with "..."; coverage beats with token Jaccard < 0.45 (tokens >= 4 chars, need >= 3 tokens); summary = first 6 items as sentences (<= 8000); topic from first key point / note heading (<= 500); blank title copied from topic only when status ended/needs_recovery/failed; timeline repairs only when `ribbon` in report_views; actor `system`/`state_repair` | `meeting/state/repair.py:repair_meeting_state` | port |
| A28-23 | Meeting-wide term corrections | `user_notes` item with `data.kind=term_correction`, by a user (or voice command `fix_transcript`); whole-word, case-insensitive, single pass, longest first, later note wins | `meeting/corrections.py:term_rules,correct_text` (+ `services/vocabulary.py:replace_terms`) | port |
| A28-24 | One-occurrence corrections | `data.kind=occurrence_correction`, exactly one evidence segment, `occurrence_index`, `base_fingerprint` FNV-1a 64 of the globally corrected text ("fnv1a64:%016x"); suspended while the base text differs; overlapping spans skipped; committed only if fingerprint still matches inside the write transaction (`stale_selection`) | `corrections.py:correct_segment_text,segment_fingerprint`; `repository.py:on_ops_applied` | port |
| A28-25 | ASR vocabulary hint | Up to 12 corrected spellings, <= 160 chars, joined ", " with trailing "." | `corrections.py:vocabulary_terms,vocabulary_hint` | port (adapt: Parakeet has no initial_prompt; see Engine-specific) |
| A28-26 | Agent guidance prompt block | "## HUMAN MEETING GUIDANCE" + fixed paragraph + JSON list of notes | `corrections.py:guidance_prompt` | undecided backend (agent LLM) |
| A28-27 | Stored-row loading | `load_state(historical=)`; corrupt/missing snapshot -> fresh state keeping row title and cloud flag; historical: row id/title/status/cloud win, running review -> unavailable "Review was interrupted. Retry when ready.", running report -> failed "This report was interrupted. Ask for it again." | `meeting/stored.py:load_state,_historical_payload,open_store` | port |
| A28-28 | History-row helpers | Display title SQL title > state.title > topic; fallback "Failed meeting"/"Untitled meeting"; pill "Failed start"/"Empty"; preview ~100 chars + "…"; content summary | `meeting/content.py` | port |
| A28-29 | Meeting row CRUD + rename | create/update/get/list; rename in one `BEGIN IMMEDIATE` transaction updating column and snapshot; token replace | `repository.py:create_meeting..replace_tokens` | port |
| A28-30 | Past Meetings page query | limit clamp 1..501 (default 101), hides active/paused/ending unless asked, keyset cursor `(started_at,id)` desc, month bounds, origin filter, literal LIKE search over title/started_at/status/state_json/transcript, 3 aggregate reads | `repository.py:list_past_meeting_summaries,_content_aggregates,_search_filter` | port |
| A28-31 | Paired-computer records | `record_owners` ("Unnamed computer"), `rename_origin`, `origin_spools`, `export_meeting_rows` / `replace_meeting_rows` whole-meeting copy with chunk id remap via `chunk_ref` | `repository.py:record_owners..replace_meeting_rows` | undecided backend (remote-computer sync) |
| A28-32 | Audio chunk durability + retry | attempts max 3; `processing` is retryable; done never regresses; idempotent registration by (meeting,channel,seq); error truncated to 2000 chars; connection failure defers without spending an attempt | `repository.py:register_chunk*..mark_chunks_done` | port |
| A28-33 | Segment storage + atomic chunk commit | `commit_chunk_transcription` writes segments and marks chunk done in one transaction, idempotent; reads project corrections into `text` and keep raw in `original_text` | `repository.py:commit_chunk_transcription,_segment_to_dict` | port |
| A28-34 | Final transcript replace with remap | Offline pass replaces live draft: one-to-one IoU >= 0.25 match per channel, pinned label always wins, unpinned live label only fills gaps, unmatched pinned rows retained, evidence on cards/questions/summary/live_highlights remapped (unique match only), citation checks cleared | `repository.py:replace_final_transcript,_match_live_to_offline,_remap_state_evidence` | port |
| A28-35 | Agent delivery pages + cursors | delivery seq from trigger table; page limit 1..301; consumers `cards`/`notes`; cursor never moves back (`max()` upsert) | `repository.py:get_segments_delivery_page,advance_delivery_cursor` | port (agent consumer undecided) |
| A28-36 | Write-through mirrors + audit | Every applied op -> `meeting_events` row; item/question/participant/segment mirrors; snapshot `state_json` + `state_seq` + `cloud_enabled`; title only changed by explicit `set_title` | `repository.py:on_ops_applied,_mirror_effect,persist_state` | port |
| A28-37 | Audit list | `list_events` limit 1..500, newest first, `undoable` = has inverse and not already undone | `repository.py:list_events` | port |
| A28-38 | Transcript FTS search | Terms individually quoted; snippet `[`...`]` with " … ", 12 tokens; newest meeting first; limit <= 200; failure -> [] | `repository.py:search_transcripts` | port |
| A28-39 | Cross-meeting candidate search | Stop-word filtered terms (<= 20), half budget lexical bm25, half "recent minute passages"; `shared_only` keeps cloud-enabled meetings | `repository.py:search_candidates` | undecided backend (feeds cloud ranking) |
| A28-40 | Meeting deletion with spool tombstone | Spool must be a direct child of meetings root (else ValueError); rename to `.deleting-<id>-<hex>`, mtime refreshed, DB delete is commit point, restore on failure, then rmtree; clears replayable automation events; tells record sync unless origin device | `meeting/persist/data_lifecycle.py:delete_meeting_data` | port (record-sync part undecided) |
| A28-41 | Bulk clear + orphan purge | Skips live rows, `skip_ids`, paired-computer rows; purges unkept spool dirs and tombstones older than 3600 s | `data_lifecycle.py:clear_meetings,_purge_orphan_spools` | port |
| A28-42 | Markdown export | Layout and strings under Modules/markdown.py; toggles `include_transcript`, `include_intelligence` | `meeting/export/markdown.py:export_markdown` | port |
| A28-43 | JSON export | `{"format_version":1,"meeting","state","segments"}`, indent 2, UTF-8, strips `host_token, guest_token, app_pid, app_heartbeat_at, state_json` | `meeting/export/json_export.py` | port |
| A28-44 | Transcript TXT export | Header title / date / "Participants: ..." then `[hh:mm:ss] Speaker: text`; "(no transcript)" | `meeting/export/transcript_txt.py` | port |
| A28-45 | Bulk export | Combined Markdown header "# Past Meetings Export\n\nN meeting(s) · exported YYYY-MM-DD HH:MM\n\n---\n\n", entries joined "\n\n---\n\n"; TXT joined by `"\n\n" + 60 "=" + "\n\n"` (no trailing separator); JSON combined envelope `{format_version, exported_at (local ISO seconds), meetings:[...]}` where each entry is itself a full single-meeting envelope (it carries its own `format_version`); per-file `YYYYMMDD_HHMMSS_slug.<fmt>` never overwriting (`-2`, `-3`) | `meeting/export/bulk.py` | port |
| A28-46 | Meeting pulse labels | decision "Decision", disagreement "Disagreement", commitment "Dated commitment", number "Number", takeaway "Takeaways" | `meeting/highlights.py:PULSE_LABELS` | port |

### Added by check

| ID | Feature | Exact behavior | Source | Disposition |
|---|---|---|---|---|
| A28-47 | Initial finalization for a new meeting | `FinalizationState.default_for_cloud(cloud_enabled)`: cloud on -> `pending` with empty message; off -> `disabled` with "AI insights are off for this meeting." | `schema.py:FinalizationState.default_for_cloud` | port |
| A28-48 | Question op edge rules | `answer_question`: agent forbidden, works on a question in any status (including already resolved), appends `{author_type, author_id, text, ts}` to `thread`. `dismiss_question`: rejected only when already `dismissed` (`already_closed`), allowed from `resolved`, inverse `reopen_question` only if it was `open`. `reopen_question`: agent forbidden, `already_open` if open, clears `resolved_at/by`, never has an inverse. `resolve_question` below 0.4 -> `low_confidence`, non-numeric -> `invalid_confidence`, only on `open` (`already_closed`) | `patches.py:_op_answer_question.._op_reopen_question` | port |
| A28-49 | Handler-level host gates beyond `HOST_ONLY_OPS` | `set_title`, `set_meeting_intent`, `set_cloud_enabled` reject any actor other than `host`/`system` with `host_only` (so a `user` guest and the agent are refused); `rename_participant` and `answer_question`/`reopen_question`/`reassign_segment_speaker` reject the agent with `agent_forbidden` | `patches.py:_op_set_title,_op_set_meeting_intent,_op_set_cloud_enabled,_op_rename_participant` | port |
| A28-50 | Rolling-summary text errors | Too long (> 8000) or non-string yields `invalid_text` (not `text_too_long`, unlike every other text field); blank only by `system` | `patches.py:_op_set_rolling_summary` | port |
| A28-51 | Pin / confirm quirks | `pin_item`/`unpin_item` work on any status (including `removed`), no actor check, no revision bump, inverse is the opposite pin op. `confirm_item` works on a `removed` item too (status becomes `confirmed`, revision++), inverse restores the previous status via forced `update_item` | `patches.py:_op_pin_item,_op_confirm_item` | port |
| A28-52 | Review question lifecycle ops | `review_skip` needs the question `open` (else `already_closed`); `review_reopen` has no status check (reopens answered/skipped too); both go through the host-only `_question` gate; a question whose item revision or evidence changed, or that is `superseded`, is `review_changed` | `review.py:skip_review,reopen_review,_question` | undecided backend |
| A28-53 | Block-level supersede text | When a later insight edit supersedes an answered review, each live_notes block that held "\n\nUser clarification (supersedes earlier wording on this point): <note>" gets "\n\nEarlier clarification, superseded by a later insight edit: <note>", and the user note itself becomes "Earlier review, superseded by a later insight edit: <note>" (both revision++) | `review.py:item_effect` | undecided backend |
| A28-54 | Content summary shape | Dict keys `has_audio, has_loopback_audio, has_transcript, is_empty, audio_chunks, transcript_segments, can_rerun_speakers (= has_loopback_audio), preview_text`; chunks count only when `duration_s > 0`, segments only when `trim(text) != ""`; unknown meeting -> all false/0, `is_empty` true | `repository.py:_content_summary,get_meeting_content_summary`, `content.py:summarize_meeting_content` | port |
| A28-55 | Past Meetings page parameters | `list_past_meeting_summaries(limit, query, origin, cursor_started_at+cursor_id, include_running, started_after, meeting_id, started_from, started_before)`. `origin`: `None` = this computer only (`origin_device_id IS NULL`), a device id = that paired computer only, omitted (sentinel) = all. `started_from` is `>=`, `started_before` is `<`, `started_after` compares with `julianday`. Order `started_at desc, id desc`. Every returned row is the full meeting dict (including tokens and `state_json`) plus `content_summary` | `repository.py:list_past_meeting_summaries,_origin_filter` | port (origin part undecided) |
| A28-56 | History calendar feed | `list_past_meeting_times()` returns `(id, started_at, ended_at, paused_total_s as float, origin_device_name)` for every non-live meeting, without snapshots | `repository.py:list_past_meeting_times` | port |
| A28-57 | Stored row and segment dict shapes | Meeting dict keys: `id,title,status,started_at,ended_at,paused_total_s,host_token,guest_token,cloud_enabled,asr_model,asr_remote_json,agent_provider,agent_model,agent_endpoint_json,spool_dir,state_json,state_seq,app_pid,app_heartbeat_at,origin_device_id,origin_device_name`. Chunk dict: `id,meeting_id,channel,seq,file_path,start_s,duration_s,sample_rate,asr_status,asr_attempts,asr_error`. Segment dict: `id,meeting_id,chunk_id,channel,start_s,end_s,text (corrected),original_text (raw),speaker_participant_id,speaker_source,speaker_pinned,created_at` (embedding BLOB is never returned) | `repository.py:_session_to_dict,_chunk_to_dict,_segment_to_dict` | port |
| A28-58 | Segment read/write odds and ends | `add_segments` bulk insert stamps one `created_at` = now and stores `embedding`; `get_segments(after_start_s=-1.0, limit)` is strict `>` on `start_s`, `limit` falsy means unbounded; `get_last_segments(count)` returns chronological order; `update_segment_speaker` raises `ValueError` if the participant is not a row of this meeting; `_mirror_segment_speaker` does the same check and defaults `source` to "human"; `_mirror_segment_text` also rewrites `effect["segment"]` with the stored row | `repository.py:add_segments,get_segments,get_last_segments,update_segment_speaker,_mirror_*` | port |
| A28-59 | Audit event reads | `get_event(meeting, seq)` returns `{seq,ts,actor_type,actor_id,action,target_id,payload,inverse}`; `list_events(before_seq, limit)` returns the same without `inverse` plus `undoable`; events store `payload_json`/`inverse_json` as `ensure_ascii=False` JSON; `seq` of an event is the op's seq (0 when none) | `repository.py:get_event,list_events` | port |
| A28-60 | Search result shapes and caps | `search_transcripts` rows `{segment_id, meeting_id, start_s, end_s, text, title, started_at, snippet}` ordered `started_at desc, start_s`; `text` there is the raw stored text (no corrections); empty query -> `[]`; optional `exclude_meeting_id`. `search_candidates(query, exclude_meeting_id, limit<=48, shared_only)` terms from `\w+` > 2 chars minus stop words (max 20) joined with ` OR ` and quoted; recent-passage arm picks the longest passage > 20 chars per meeting per minute, one per meeting first | `repository.py:search_transcripts,search_candidates` | port / undecided (candidates) |
| A28-61 | Keep-recording delete and clear | `delete_meeting_data(..., delete_spool=False)` removes DB rows only and leaves the spool; `clear_meetings(delete_spools=False)` skips orphan purging; a blank `spool_dir` or `meetings_root` means no spool handling; `_validated_spool_path` raises `ValueError` for a path equal to the root or not a direct child; delete returns False for an unknown id; `delete_meeting` calls `dictation_stats.count_features()` first (best effort) so Stats keep counting the meeting | `data_lifecycle.py`, `repository.py:delete_meeting` | port |
| A28-62 | Export date semantics | All export dates go through `as_local_time`: aware ("Z"/offset) stamps convert to the OS local zone, legacy naive stamps are kept as wall time; unparsable `started_at` is printed raw in the header; title fallback "Meeting YYYY-MM-DD" uses the same conversion; `meeting_duration_s` = (ended - started) minus `paused_total_s`, clamped >= 0, None when a stamp is missing | `meeting/time_utils.py` (a24) used by `transcript_txt.py`, `markdown.py`, `bulk.py` | port |
| A28-63 | JSON export leaves some row fields in | Only `host_token, guest_token, app_pid, app_heartbeat_at, state_json` are stripped; `spool_dir` (a local path), `asr_remote_json`, `agent_endpoint_json`, `origin_device_*`, `state_seq` stay in `meeting`; segments carry both `text` and `original_text`. Decide whether OpenWhisper_CPP keeps this or also strips `spool_dir` | `json_export.py:_VOLATILE_MEETING_FIELDS` | port (consider stripping `spool_dir`) |
| A28-64 | Markdown card rendering odds | Empty or removed items are skipped on every card; `decisions` and `user_notes` render as plain `- text` bullets (with the review/spoken/advisory prefixes and suffix); no transcript section at all when there are no non-blank segments (TXT prints "(no transcript)" instead); intelligence toggle off still keeps `## Meeting Brief`; `{:.0%}` confidence uses Python rounding | `markdown.py:_card_lines,export_markdown,_append_questions` | port |
| A28-65 | Corrections author and rule bounds | `is_correction_author`: `author_type == "user"`, or `system` + `author_id == "voice_command"` + `data.source == "voice_command"` + `data.command == "fix_transcript"`; removed notes never count; term/replacement trimmed length 1..120; occurrence index must be a non-bool int 0..1000; `valid_segment_fingerprint` regex `fnv1a64:[0-9a-f]{16}`; `vocabulary_terms` dedupes replacements case-insensitively, oldest rule first, cost = len + 2 per extra term; `repository_term_rules` provider re-reads only when `state_seq` changes and returns `{}` on any error | `corrections.py` | port |

## Modules

### meeting/state/schema.py (745 lines)
Responsibility: dataclasses for the dashboard document and its JSON contract. Pure, no I/O.

- `new_id(prefix)` -> `f"{prefix}_{uuid4().hex[:12]}"`; prefixes used: `it` item, `q`
  question, `p` participant, `rep` report (`sg` segment ids come from the engine).
- `parse_state_json(raw)` (`:63`): dict passthrough; falsy -> None; bad JSON or non-object -> None.
- `CardItem` (`:84`): fields id, card, text, data, status="proposed", author_type="agent",
  author_id, pinned, revision=1, evidence, created_at/updated_at (UTC ISO "Z"), review,
  citation_check. `protected` (`:103`). `from_dict` requires `id`, `card`; defaults others.
- `Question` (`:140`), `CustomReport` (`:184`, unknown status -> "failed" at `:225`),
  `Participant` (`:235`, kind default `others_cluster`, name_source `default`), `TopicState`,
  `MeetingIntent` (`:281`, tolerant of non-dict), `FinalizationState` (`:312`).
- `FinalizationState.coerce` (`:422`): any unknown shape/status falls back to `infer_legacy`;
  `card_deferred` coerced from bool/number/"1"/"true"/"yes" (`_coerce_card_deferred` `:577`).
- `normalize_historical` (`:492`): only for terminal statuses `ended, failed, needs_recovery`
  and only when fin is pending/running: cloud off -> disabled; running -> failed with running/
  pending steps marked failed; pending -> unavailable. Always keeps stage/steps/stats/deferred.
- `MeetingState.from_dict` (`:679`): missing `finalization` key -> `infer_legacy`, present ->
  `coerce`; `capture` rebuilt to exactly 5 keys; cards only for `CARD_KEYS` (unknown card keys
  dropped); questions dict keyed by id (list in JSON); custom_reports skip entries without id;
  `report_views` empty -> default 3.
- `to_dict` emits `questions` as a list, `participants` and `cards` as objects.
- Edge: `created_at` default is "now" when absent, so round-tripping a legacy snapshot invents
  timestamps (keep for parity).

### meeting/state/patches.py (1070 lines)
Responsibility: validation + application of every op. Pure over a `MeetingState`; caller owns
locking/seq/persist.

- `OpContext(actor_type, actor_id, segment_exists, segment_pinned)`; `is_agent` = "agent";
  `is_human` = "user" or "host". `system` is neither (undo, repair, workers, app).
- `apply_ops` (`:1024`) order of gates: malformed (`malformed_op`) -> unknown (`unknown_op`) ->
  agent not in AGENT_OPS (`agent_forbidden`) -> agent evidence check on every op (so even
  `update_item` needs evidence at top level) -> AGENT_ONLY_OPS (`agent_only`) -> HOST_ONLY_OPS
  for `user` (`host_only`) -> handler (exception -> `internal_error`).
- `_check_evidence` (`:187`): agent with empty evidence `missing_evidence`; non-list or > 20
  `invalid_evidence`; any id failing `segment_exists` `unknown_evidence`.
- `_check_data_size` (`:201`): `len(json.dumps(data)) > 4096` with **Python default
  `ensure_ascii=True` and `", "`/`": "` separators** -> `data_too_large`; unserializable ->
  `invalid_data`. The C++ size must be computed the same way.
- `_check_text`: non-str or blank `invalid_text`; longer than limit (code points)
  `text_too_long`.
- `_normalize_item_text`: `lower()` then `[^a-z0-9]+` -> space (non-ASCII letters become
  separators).
- `_op_add_item` (`:289`): order unknown_card, human_only_card (agent to user_notes), text,
  evidence, data dict, data size + card data (occurrence correction validation `:339`, timeline
  start_s for agent/system `:334`), `card_full` (500), `duplicate_item` (agent only). Human add ->
  status `edited`, author_type `user`; agent -> `proposed`, author_type actor. Effect
  `{"entity":"item","item":...}`, inverse `remove_item`.
- Occurrence correction (`:339`): human only, exactly one non-empty evidence id that exists,
  `selected_text`/`replacement` 1..120 stripped, `occurrence_index` int (not bool) 0..1000,
  `base_fingerprint` matches `fnv1a64:[0-9a-f]{16}`.
- `_op_update_item` (`:403`): `force` honored only for system; agent blocked when protected
  (`human_edited`) or revision mismatch; empty `set` without `restore_status` -> `empty_update`;
  agent rewrite of a `state_repair` item sets `data.insight_synthesized=true` (`:423`); evidence
  union (`:495`) or exact restore on force; status: `restore_status` if forced, else `edited` for
  humans; review/citation_check cleared; revision++; inverse restores text/data/evidence/status
  with `force:true`. Effect from `review.item_effect` (may supersede review questions).
- `_op_remove_item`: `already_removed`; inverse is forced update restoring status.
- `_op_pin_item` / unpin: no revision bump. `_op_confirm_item`: status confirmed, revision++.
- `_op_set_topic`: inverse only if previous topic non-empty (first topic is not undoable).
- `_op_set_title`: inverse only if previous title non-empty.
- `_op_set_meeting_intent`: `author_id` = actor id; inverse sets previous text.
- `_op_upsert_participant` (`:671`): with `id` = rename (agent blocked on human name), inverse
  `rename_participant` with private `_name_source`; without id = create (`participant_limit`
  200, kind must be me/others_cluster/guest), no inverse.
- `_op_rename_participant`: system may restore `_name_source`; otherwise `human`,
  `is_provisional=false`.
- `_op_ask_question`: cap counted on open questions; system uncapped. Inverse dismiss.
- `_op_resolve_question` thresholds `RESOLVE_CONFIDENCE=0.8`, `SUGGEST_CONFIDENCE=0.4`; suggestion
  path has no inverse; evidence unioned.
- `_op_dismiss_question`: inverse reopen only if it was open.
- `_op_reassign_segment_speaker` (`:912`): agent forbidden; `participant_id` may be null; effect
  `source` = system `_source` (default diarizer) / human / diarizer; `pinned` = system `_pinned`
  else is_human.
- `_op_revise_segment_text` (`:958`): effect text stripped.
- Exported constants used elsewhere: `MAX_NAME_LEN` (content.py), `MAX_OPEN_QUESTIONS`
  (live_signals), `MAX_INTENT_LEN` (engine, meeting_mode_tab), `RESOLVE/SUGGEST_CONFIDENCE`
  (agent tool specs), `filter_notes_ops`, `live_note_ids` (tool_policy).
- `_HANDLERS` merges `REVIEW_HANDLERS`, `FAST_HANDLERS` (a29 `meeting/fast_state.py`:
  `publish_highlights`, `citation_check`, `invalidate_citations`, `voice_feedback`),
  `CUSTOM_REPORT_HANDLERS`.

### meeting/state/store.py (469 lines)
Responsibility: thread-safe owner of one meeting's state.

- Public: `MeetingStateStore(state, repository, segment_handler, segment_exists, segment_pinned,
  segment_lookup)`; `meeting_id`, `seq`, `snapshot()`, `with_state(fn)`, `apply(actor_type,
  actor_id, ops) -> [OpResult]`, `update_runtime_fields(**fields) -> bool`, `refresh_title()`,
  `replace_document(state)`, `undo(event_seq, actor_id)`, `subscribe/unsubscribe(cb(seq,
  applied))`. Free functions `repository_segment_lookup`.
- Threading: one `threading.RLock`; called from engine threads, agent threads, web server loop
  threads, Qt GUI. Subscribers run under the lock and must not block (web hub marshals onto its
  loop).
- `apply` (`:210`): candidate = `_copy_for_ops` (shallow copy; deep copy only domains listed in
  `_OP_DOMAINS` `:47`; unknown op -> full deepcopy); batch predicates from one bulk
  `segment_flags` lookup over ids under keys `segment_id`, `evidence`, `segment_ids` up to depth
  2 (`:117`); ops are deep-copied before handlers; `_sequence_results` runs segment handler for
  SEGMENT_OPS (no handler -> `segments_unavailable`; exception -> `segment_error`) and numbers
  each still-ok op; persist via `repository.on_ops_applied`; on success adopt candidate and
  notify with max seq.
- Seq: rejected ops never consume a seq (`test_rejected_ops_do_not_bump_seq`).
- `_reconcile_persisted` (`:321`): with `state_metadata_only=True` (Sql repo) only `title` is
  adopted from the persisted dict; otherwise full `from_dict`.
- `update_runtime_fields` (`:282`): unknown attr -> AttributeError; `finalization` coerced;
  transition into `running` calls `invalidate_checks` on copies; persisted via
  `persist_state`; failure returns False without changing live state.
- `undo` (`:358`): needs repository; unknown event / no inverse / already undone -> `[]`.
- Test `test_all_registered_handlers_have_explicit_mutation_domains` pins that every handler has
  an `_OP_DOMAINS` entry.

### meeting/state/review.py (379 lines)
Responsibility: insight-review ops (system publishes, host answers). Consent constant
`REVIEW_CONSENT="typesafe-text-v1"` must equal `services/settings.py:INSIGHT_REVIEW_CONSENT`.

- `review_begin` (system; needs `insight_review.enabled`, `cloud_enabled`, consent; meeting
  `ended` and finalization not pending/running; not already running).
- `review_finish`: run_id must match running review (`stale_review`); cloud/consent revoked ->
  status unavailable; assessments applied only to unprotected, non-removed items at the same
  revision; new questions one per item, deduped by normalized item text; skipped
  `(item_id,revision,field)` stays skipped; `questions` = last 50 answered + new (only when status
  ready/partial); `checked_at`.
- `_question` gate (host only; meeting ready; question exists; not superseded; item revision and
  evidence unchanged; removed item only if question had a correction) -> `review_changed`.
- `review_answer`: choices from the question; text 1..3500; owner/deadline edits (<= 120 chars;
  changing owner/deadline without changing wording -> `review_wording_required`); data <= 4096;
  user_notes cap 500 (`item_limit`); item status `removed` for `incorrect` else `edited`;
  item.review `{"state":"human",...}`; history last 10.
- `review_skip`, `review_reopen`, `review_unavailable`.
- `item_effect(state,item)` (`:337`): called by update/remove; marks related questions superseded
  and rewrites linked notes; returns `{"entity":"review",...,"items":[...]}`.
- `invalidate_checks(state)` (`:364`): clears every citation_check; if review enabled: status
  pending, unanswered questions superseded, non-human item reviews cleared.

### meeting/state/custom_reports.py (161 lines)
See A28-21. Effect `{"entity":"custom_report","report":...,"removed":bool}`. No inverse ops (not
undoable). `WORKER_ACTOR="custom-report"`. Constants `MAX_REQUEST_CHARS=2000` (also used by web
api), `MAX_REPORT_CHARS=60000`, `MAX_TITLE_CHARS=200`, `MAX_MESSAGE_CHARS=500`; `_clip` truncates
silently (unlike request, which is rejected).

### meeting/state/repair.py (399 lines)
See A28-22. Works on `to_dict()` snapshots and segment dicts, emits ops applied as
`store.apply("system","state_repair",ops)` in three batches (timeline backfill, timeline
coverage, summary+topic) then title (`content.build_untitled_title_ops`). Each step re-snapshots;
snapshot failure stops. `_synthesized_items` excludes unreviewed legacy `state_repair` samples
(not edited/confirmed/pinned and no `insight_synthesized`). Note beat text `"heading: text"`
unless text already starts with heading. Note `_clip_text` replaces newlines with spaces.
Callers: engine end of capture, agent scheduler, reinsight, refinalize, benchmarks.
Note: test file `test_meeting_repair.py` also covers evidence-id fuzzy repair which lives in a29
(`tool_policy`), not here.

### meeting/state/segment_ops.py (90 lines)
`handle_segment_op(repository, meeting_id, result)` reads prior row and returns the inverse:
text revision -> `revise_segment_text` restoring `original_text` (raw, not the corrected view)
and enriches effect with full `segment` dict (text and original_text both new text); speaker ->
`reassign_segment_speaker` with prior participant, `_source` (default "channel"), `_pinned`,
`force:true`. No prior row -> None (op still applies but is not undoable).
`make_segment_handler(repository, meeting_id)`.

### meeting/persist/repository.py (1549 lines)
Responsibility: every SQL statement for Meeting Mode. `SqlMeetingRepository(db=None)` uses the
app `DatabaseManager` (a09 `services/database.py`): SQLAlchemy, SQLite WAL, `foreign_keys=ON`,
`timeout=30`, `check_same_thread=False`, `get_session()` commits on exit and rolls back on
exception, `expire_on_commit=False`. All methods take/return plain dicts.

Public surface (grouped):
- Meetings: `create_meeting(**)`, `update_meeting(id, **)` (unknown id -> warning, no-op),
  `rename_meeting` (BEGIN IMMEDIATE; corrupt snapshot replaced with `{meeting_id,title}`),
  `replace_tokens`, `get_meeting`, `list_meetings` (started_at desc), `list_past_meeting_summaries`,
  `get_meeting_content_summary`, `list_past_meeting_times` (tuples for History calendar),
  `delete_meeting` (explicit child deletes, not FK cascade, because FTS triggers do not fire on
  cascade `:794`; calls `services.dictation_stats.count_features()` first), `heartbeat`
  (`app_pid=os.getpid()`), `find_interrupted_meetings` (status active/paused/ending/
  needs_recovery or any chunk not done).
- Paired computers: `record_owners`, `rename_origin`, `origin_spools`, `export_meeting_rows`,
  `replace_meeting_rows` (`_COPY_TABLES` order chunks, segments, participants, state_items,
  questions, events).
- Chunks: `register_chunk`, `register_chunk_if_missing -> (id, created)`, `set_chunk_status`
  (done is terminal; entering processing increments attempts), `get_pending_chunks(limit,
  exclude_ids)` (statuses pending/failed/processing, attempts < 3, order start_s,id),
  `count_unfinished_chunks`, `reset_unfinished_chunks(force)` (non-force skips `blocked` and
  exhausted; force resets attempts and error), `defer_chunk_after_connection_failure`,
  `get_audio_chunks` (order start_s, channel, seq), `next_chunk_seq`, `mark_chunks_done`.
- Segments: `add_segments`, `commit_chunk_transcription`, `get_segment`, `segment_exists`,
  `segment_flags` (500-id batches), `update_segment_speaker`, `get_segments(after_start_s,
  limit)`, `get_segments_page` (keyset, limit 1..1001), `get_last_segments`,
  `replace_final_transcript -> (rows, deleted_ids, id_map)`.
- Delivery: `get_segments_delivery_page`, `get_delivery_cursor`, `advance_delivery_cursor`,
  `get_delivery_high_water`.
- State write-through: `on_ops_applied`, `persist_state`, `_mirror_effect`; `state_metadata_only
  = True`.
- Audit: `get_event`, `list_events`, `event_is_undone`.
- Search: `search_candidates`, `search_transcripts`.

Edge cases a port must keep:
- `_segment_to_dict` (`:94`) applies term + occurrence corrections from the meeting's state on
  every read, cached per session/transaction; `text` = corrected, `original_text` = raw.
- `interval_iou` returns 0 for empty/negative intersections or unions (`:48`).
- `_best_evidence_match` (`:118`): explicit id_map first, then retained ids, else unique IoU match
  on the same channel; ambiguous -> dropped.
- `_remap_state_evidence` drops non-`sg_` ids, drops `live_highlights` pulses whose segment is
  unmapped, clears card `citation_check`.
- `_store_offline_segments` refuses segments of another meeting; keeps `created_at` of existing
  rows.
- `commit_chunk_transcription` returns existing rows unchanged when the chunk is already done
  (idempotent, `committed=False`).
- `on_ops_applied` (`:1288`): BEGIN IMMEDIATE; scoped-correction fingerprint revalidation against
  raw row text (or same-batch revised text) after global rules -> raises
  `StaleCorrectionSelection`; one `MeetingEvent` per result (action `undo:<seq>` for system undo);
  mirrors; title column changes only on explicit `set_title`; `state["title"] = row.title`;
  writes `state_json` (ensure_ascii False), `state_seq`, `cloud_enabled`.
- `persist_state` also writes `status` column.
- `_mirror_segment_text` rejects empty text (raises -> batch `persistence_error`).
- `search_transcripts` quotes each whitespace term with `"` doubled; `search_candidates` stop
  words: the, did, what, when, where, how, was, were, about, with, that, meeting; terms > 2 chars.
- `_search_filter` escapes `\`, `%`, `_` with `\`; SQLite `LIKE` is ASCII case-insensitive only.
- `_content_summary` preview: first non-blank segment by (start_s,id), > 100 chars -> first 100
  rstripped + "…".

### meeting/persist/data_lifecycle.py (226 lines)
See A28-40/41. Constants `DELETION_TOMBSTONE_STALE_S=3600`, prefix `.deleting-`. `_deleted`
calls `services.automation.events.clear()` and `services.remote_records.sync.record_sync.
record_deleted("meeting", id)` unless the row has `origin_device_id`. Path checks use
`realpath` (symlinks resolved) and require `dirname(spool) == root`.

### meeting/content.py (167 lines)
History-row helpers (A28-28): `meeting_display_title`, `fallback_meeting_title`,
`untitled_title_from_topic` (<= 120 chars), `build_untitled_title_ops`, `meeting_insights_pill`
("Failed start"/warning for status failed; "Empty"/warning when content_summary.is_empty; else
finalization pill), `summarize_meeting_content` (uses repo aggregate when available; fallback
builds preview by joining segment texts until >= 100 chars), `meeting_preview_text`.

### meeting/corrections.py (250 lines)
See A28-23..26. `is_correction_author` (user, or system voice_command with data.source
voice_command and command fix_transcript). `term_rules_from_items` later wins; key lowercased.
`correct_segment_text` (`:123`): global first, fingerprint of corrected base, regex
`(?<!\w)<escaped>(?!\w)` IGNORECASE (Unicode `\w`), occurrence by index among matches,
overlapping spans skipped, applied right-to-left. `repository_term_rules` provider caches by
`state_seq`. `correct_text` delegates to `services/vocabulary.py:replace_terms` (a12): casefold
with origin mapping, longest-first alternation, refuses to split a character that folded to
several. `StaleCorrectionSelection(ValueError)`.

### meeting/stored.py (221 lines)
`stored_state_dict`, `stored_endpoint` (agent_endpoint_json parse), `meeting_endpoint` (via
`services.text_llm.snapshot_from_meeting`, undecided), `finalization_from_meeting_row`,
`compact_finalization_list_fields` (`finalization_status`, `finalization_deferred`,
`insights_pill`, `insights_tone`), `load_state`, `open_store` (store wired with segment
handler, `segment_exists`, `segment_pinned`, bulk lookup). Fresh state reads report views from
settings (`resolve_meeting_report_views`, falls back to defaults on error).

### meeting/highlights.py (9 lines)
`PULSE_LABELS` (A28-46). Used by markdown export and a29 fast_state.

### meeting/export/transcript_txt.py (188 lines)
`format_clock` `hh:mm:ss` (hour widens, negatives clamp, int truncation), `format_mmss` `mm:ss`
(minutes exceed 59), `resolve_title` (row title > state title > "Meeting YYYY-MM-DD" > "Meeting"),
`format_meeting_date` (`YYYY-MM-DD HH:MM` local, raw string if unparsable, None if absent),
`participant_names` (`me` first, stable order, blanks dropped), `speaker_name` (participant name
else "Me" for mic, "Others" otherwise), `transcript_lines`, `export_transcript_txt`
(`title\n[date\n][Participants: a, b\n]\n<lines or "(no transcript)">\n`).

### meeting/export/markdown.py (525 lines)
`export_markdown(meeting, state, segments, include_transcript=True, include_intelligence=True)`
order:
1. `# <title>`; blank; metadata line joined " · ": date, `Duration hh:mm:ss` (max segment
   end_s if > 0 else row wall time minus pauses), `Participants: ...`.
2. `## Meeting Brief` with each line `> ` (always, even without intelligence).
3. Intelligence only: `## User Clarifications` ("These corrections supersede earlier wording on
   the same points." + bullets from non-superseded review corrections); `## Topic` + current +
   "Previously:" bullets `- text (HH:MM)` for history except last; `## Summary`; `## Meeting
   Pulses` ("Automatically detected moments; labels are advisory.", sorted `- [mm:ss]
   **Label:** text`, only known kinds with finite numeric start_s); cards in order Meeting Notes
   (live_notes), Key Points, Decisions, Action Items, Risks, Timeline, Notes (user_notes);
   `## Questions`; `## Requested Reports`.
4. `### Transcript` lines with two trailing spaces each.
Card rendering: item text prefixed by labels "Needs verification" / "Provisional" / "User
clarified" (from review.state unsupported/provisional/human) and "Spoken note"/"Spoken request"
for voice_command items, joined ": "; suffix ` _(Advisory: <label>)_` only when
citation_check.revision == item.revision (labels: Citation supports claim / conflicts with claim
/ Check citation / Missing citation / Citation uncertain / Citation check unavailable / Citation
needs recheck). Action items: ` — Due: X` from deadline or legacy due_date; grouped `**Mine**`,
`**Others**` (` — <name>`, "Unknown"), `**Unassigned**` only when some owner resolves, else flat.
Risks `- **[severity]** text`. Timeline `- [mm:ss] text`. Key points `[mm:ss]` from earliest
evidence segment. Live notes sorted by start_s (else created_at), whitespace collapsed,
`**mm:ss heading** — body`. Questions: open `- **Open:** q` + `  - Suggested answer (60%
confidence): s` (`{:.0%}`); resolved `- **Resolved:** q` + `  - Answer: a — _answered from
audio_` / `_answered by <name>_` / `_answered by participant_`; dismissed omitted. Reports: only
`ready` with markdown; `### <title or request or "Report">`, `> Requested: <request>`, body with
a leading `# <title>` stripped and headings demoted by 3 (cap 6, not inside ``` or ~~~ fences).
Document ends with "\n". `demote_headings` is public.

### meeting/export/json_export.py (61 lines)
A28-43. `json.dumps(indent=2, ensure_ascii=False)` with default separators (`", "` items end
lines; `": "`).

### meeting/export/bulk.py (251 lines)
A28-45. `list_export_meetings` skips live statuses and fills `content_summary`.
`filter_export_meetings(from_dt,to_dt,only_with_transcript)`: naive local bounds inclusive;
unparsable started_at excluded when a bound is set; unknown transcript kept.
`collect_meeting_export` -> `{meeting,state,segments}` or None. `render_meeting_document`
(toggles only affect Markdown). `meeting_file_stem`: local `%Y%m%d_%H%M%S` or "unknown_date",
title whitespace -> "-", keep `[\w\-.]`, strip "-._", 60 chars. `write_per_meeting_files` opens
with `"x"` (exclusive create) and uses the **format key as extension** (`.markdown`, `.txt`,
`.json`): the export dialog passes `"markdown"`, so per-meeting Markdown files end `.markdown`
while single-file saves use `.md` (a09 `export_dialog_base.py:78`). Decide whether to keep.

## Data and formats

Settings keys read in this area (definitions in a10 `services/settings.py`):
- `meeting_report_ribbon` / `meeting_report_brief` / `meeting_report_signal` (bool, via
  `resolve_meeting_report_views`, fallback `("ribbon",)` when all off) -> fresh state
  `report_views` (`stored.py:_fresh_state`).
- Review consent value must be `typesafe-text-v1` (`meeting_insight_review_consent`); the state
  stores `insight_review = {enabled, consent, sensitivity, status, message, run_id, questions,
  checked_at}` (shape set by a29 `meeting/insight_review.py`).
- Meetings root directory (`meetings_root`) is passed in by callers (a24/a31).

State document (`meeting_sessions.state_json`, WS `hello`, REST, JSON export) top-level keys in
order: `meeting_id, seq, status, cloud_enabled, intelligence_online, diarization_available,
diarization_reason, speech, title, intent{text,updated_at,author_id}, topic{current,history[]},
rolling_summary, rolling_summary_evidence, capture{mic_available,loopback_available,message,
audio_incomplete,integrity_error}, participants{id:{...}}, cards{card:[item]}, questions[...],
finalization{status,message,stage,current_step,total_steps,step_details,steps,summary_stats,
card_deferred}, report_views, insight_review, live_highlights, live_highlights_status,
voice_feedback, custom_reports[]`. Meeting status values: `active, paused, ending, ended, failed,
needs_recovery`. Item `data` keys in use: `start_s`, `heading`, `owner_participant_id`,
`owner_name`, `deadline`, legacy `due_date`, `severity`, `commitment`, `resolution`, `kind`
(`term_correction`, `occurrence_correction`, `agent_insight`), `selected_text`, `replacement`,
`occurrence_index`, `base_fingerprint`, `review_item_id`, `insight_synthesized`, `source`,
`command`.

Op wire shape: `{"op": name, ...}`; fields per op: add_item `card,text,data,evidence`;
update_item `id,set{text,data,evidence},base_revision,evidence,restore_status,force`;
remove/pin/unpin/confirm `id[,base_revision]`; set_topic/set_rolling_summary `text,evidence`;
set_title/set_meeting_intent `text`; set_cloud_enabled `enabled`; upsert_participant
`[id],display_name,kind,is_provisional,evidence`; suggest/rename `participant_id,display_name
[,_name_source]`; ask_question `text,evidence`; resolve_question `question_id,answer_text,
confidence,evidence`; answer/dismiss/reopen `question_id[,answer_text]`; reassign_segment_speaker
`segment_id,participant_id[,force,_source,_pinned]`; revise_segment_text `segment_id,text,
evidence`; review_* `run_id,status,message,assessments[{item_id,revision,review}],questions[{id,
item_id,revision,field,choices,evidence,...}]`, review_answer `question_id,answer,text,
owner_participant_id,owner_name,deadline`; custom report `request,run_id` / `report_id,run_id,
status,markdown,title,message,sources`. Private `_undo_event_seq` on undo.

OpResult (a24 `meeting/interfaces.py:85`): `ok, op, reason, target_id, effect, inverse,
current_revision, seq`. Effect entities: `item, topic, rolling_summary, title, intent,
cloud_enabled, participant, question, segment_speaker, segment_text, review, custom_report`
(+ a29 fast ops). Rejection reasons: `malformed_op, unknown_op, agent_forbidden, agent_only,
host_only, system_only, internal_error, missing_evidence, invalid_evidence, unknown_evidence,
invalid_text, text_too_long, invalid_data, data_too_large, unknown_card, human_only_card,
card_full, duplicate_item, missing_start_s, invalid_start_s, unknown_item, human_edited,
revision_mismatch, empty_update, already_removed, invalid_kind, unknown_participant,
human_named, participant_limit, question_limit, unknown_question, already_closed, already_open,
invalid_confidence, low_confidence, invalid_segment, unknown_segment, segment_pinned,
human_scoped_correction, review_disabled, meeting_not_ready, review_running, stale_review,
unknown_review_question, review_changed, invalid_review_answer, invalid_review_text,
invalid_owner, invalid_deadline, review_wording_required, item_limit, invalid_request,
request_too_long, invalid_run_id, report_running, report_limit_reached, unknown_report,
stale_report, invalid_report_status, empty_report`; store-level `segments_unavailable,
segment_error, persistence_error, stale_selection`. These reach the agent and web clients
verbatim.

SQLite tables (models in a14 `services/models.py:191-391`, created/migrated by a09
`services/database.py`; the C++ port must open the **existing** `openwhisper.db`):
- `meeting_sessions(id PK, title, status, started_at, ended_at, paused_total_s, host_token,
  guest_token, cloud_enabled, asr_model, asr_remote_json, agent_provider, agent_model,
  agent_endpoint_json, spool_dir, state_json, state_seq, app_pid, app_heartbeat_at,
  origin_device_id, origin_device_name)`, index `idx_msessions_started`.
- `meeting_audio_chunks(id INTEGER PK AUTOINCREMENT, meeting_id FK, channel mic|loopback, seq,
  file_path, start_s, duration_s, sample_rate, asr_status pending|processing|done|failed|blocked,
  asr_attempts, asr_error)`, unique `(meeting_id,channel,seq)`.
- `meeting_segments(id PK "sg_...", meeting_id FK, chunk_id FK, channel, start_s, end_s, text,
  speaker_participant_id, speaker_source channel|diarizer|human, speaker_pinned, embedding BLOB
  float32, created_at)`, index `(meeting_id,start_s)`.
- `meeting_segment_delivery(seq PK AUTOINCREMENT, segment_id UNIQUE FK, meeting_id)` filled by
  trigger `meeting_segments_delivery_ai`.
- `meeting_agent_delivery_cursors(meeting_id, consumer, seq)` PK `(meeting_id,consumer)`.
- `meeting_participants`, `meeting_state_items` (`data_json`, `evidence_json`),
  `meeting_questions` (`answer_text, answer_confidence, suggested_answer_text, thread_json,
  evidence_json`), `meeting_events(id, meeting_id, seq, ts, actor_type, actor_id, action,
  target_id, payload_json, inverse_json)`.
- FTS5 `meeting_segments_fts(text, content='meeting_segments', content_rowid='rowid')` with
  insert/delete/update triggers. Requires an SQLite build with FTS5.
- Timestamps are strings: new ones UTC ISO with "Z" (`time_utils.utc_now_iso`), legacy ones
  naive local; `started_at` comparisons are string comparisons except `started_after` which uses
  `julianday`.

Files on disk: meeting spool directories are direct children of the meetings root; deletion
tombstones `.deleting-<meeting_id>-<32 hex>`; exports written where the user picks.

Wire/other programs: the JSON export envelope (`format_version` 1) and the state document are
consumed by the web dashboard (JS renderer computes the same FNV-1a fingerprint over UTF-8),
MCP tools, and the remote-records sync (row copies via `export_meeting_rows`). REST download
`/api/export/{md|json|txt}` lives in the web area and calls these renderers.

### Added by check

- `MeetingState` defaults (what a fresh or legacy snapshot gets): `seq 0`, `status "active"`, `cloud_enabled/intelligence_online/diarization_available false`, `diarization_reason ""`, `speech {}`, `title ""`, `rolling_summary ""`, `insight_review {}`, `live_highlights []`, `live_highlights_status "unknown"`, `voice_feedback {}`, `custom_reports []`, all seven cards present and empty. `from_dict` requires `meeting_id` (a missing key raises, and `load_state` then falls back to a fresh state). Card items require `id` and `card`; questions and participants require `id`; a card item's `revision` defaults to 1, `author_type` to "agent". Unknown top-level keys are dropped on round trip.
- Constants in `review.py` not listed above: `_MAX_REVIEW_TEXT 3500`, `_MAX_FIELD_CHARS 120`, `_MAX_DATA_BYTES 4096` (same ASCII-escaped `json.dumps` length rule), `_MAX_USER_NOTES 500`, `_MAX_NOTE_BLOCK_CHARS 4000` (a clarification that would push a live_notes block past 4000 chars is silently not appended).
- Constants in `repair.py`: `_MAX_TIMELINE_BEATS 8`, `_MAX_BEAT_TEXT 180`; summary sentences are each item text with trailing "." stripped and one "." added, first 6 items, evidence gathered from those items (deduped, max 20); topic evidence deduped, max 20.
- Constants in `repository.py`: `MAX_CHUNK_ATTEMPTS 3`, `MIN_MATCH_IOU 0.25`; `get_segments_page` limit clamp 1..1001 default 500 (keyset `(start_s,id)`); `get_segments_delivery_page` default 300; event `list_events` default 100; `search_candidates` limit default 48.
- Other constants: `custom_reports.py` `WORKER_ACTOR "custom-report"`; `corrections.py` `MAX_TERM_CHARS 120`, `VOCABULARY_MAX_TERMS 12`, `VOCABULARY_MAX_CHARS 160`, guidance kinds `agent_insight, term_correction, occurrence_correction`; `content.py` `_PREVIEW_LIMIT 100`; `bulk.py` `EXPORT_FORMATS ("markdown","txt","json")`, `_FILE_STEM_MAX_TITLE 60`; `transcript_txt.py` fallback speakers "Me" (mic) / "Others"; `data_lifecycle.py` `DELETION_TOMBSTONE_STALE_S 3600`.
- Meeting dicts handed to the exporters are the raw row dicts, and the exported `state` for bulk and single exports is parsed from the row's stored `state_json` (not a live store snapshot), so an export of a running meeting reflects the last persisted write.
- Strings written into the audit log: event `action` is the op name, or `undo:<seq>` only when actor is `system` and the op carries an integer `_undo_event_seq`; an event with an inverse is `undoable` until an `undo:<seq>` row exists.
- Meeting-status gating strings: live statuses hidden from Past Meetings and bulk export are `active, paused, ending`; terminal statuses for historical normalization are `ended, failed, needs_recovery`.

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| `dataclasses`, `copy.deepcopy` | state model, copy-on-write | plain structs with value semantics; copy only touched domains (or immutable/COW containers) |
| `json` | state_json, exports, data size check | nlohmann::json `ordered_json` (preserve insertion order; UTF-8 output). Write a Python-compatible `dumps` (ensure_ascii + `", "`/`": "` separators) for the 4096 data-size rule and an `indent=2` writer matching Python for exports |
| `uuid` | ids | `QUuid::createUuid()` or random 48-bit hex (12 hex chars) |
| `threading.RLock` | store lock | `std::recursive_mutex` |
| `re` (Unicode `\w`, lookbehind, IGNORECASE) | corrections, slugs, heading demotion, normalization | PCRE2 (UTF + UCP) in the Qt-free core, or `QRegularExpression` if QtCore is allowed there; std::regex lacks lookbehind and Unicode classes |
| `str.lower`/`casefold` | normalization, term keys | ICU or `QString::toLower`/`toCaseFolded`; must match Python on non-ASCII |
| SQLAlchemy ORM + `sqlite_insert ... on_conflict_do_update` | repository | sqlite3 C API (vcpkg `sqlite3[fts5]`) with a thin RAII wrapper, or Qt SQL `QSQLITE` (check FTS5 is compiled in). Keep `BEGIN IMMEDIATE`, WAL, foreign_keys, 30 s busy timeout |
| `os`, `shutil.rmtree`, `os.replace`, `os.utime`, `realpath` | spool tombstone and purge | `std::filesystem::rename/remove_all/last_write_time/canonical` (or QDir/QFile) |
| `datetime`, `astimezone` | local display of UTC stamps, export stamps | `std::chrono` + `zoned_time` (C++20 tzdb) or `QDateTime::toLocalTime` |
| `math.isfinite` | pulse filtering | `std::isfinite` |

## Platform-specific code

None in this area. Path validation uses `realpath` (resolves symlinks and on Windows normalizes
case/8.3 names differently); `os.replace` on Windows fails if the target exists or a file is open
in the spool (playback); the C++ port should keep the restore-on-failure path. Per-meeting export
files use exclusive create (`"x"`). Local-time formatting depends on the OS time zone.

## Engine-specific code

- `corrections.py:vocabulary_hint` exists for Whisper's `initial_prompt`. Parakeet TDT on
  NeMo-Speech.cpp has no prompt input; check whether the runtime's `speech_contexts` option
  (pointers must outlive the request) can carry the terms, else drop the hint and keep only the
  post-ASR `correct_text`. `correct_text` itself is engine-agnostic and must be kept (a24's ASR
  engine applies it to every chunk).
- `repository.defer_chunk_after_connection_failure` and `asr_remote_json` serve the
  remote-computer engine (undecided).
- `MAX_CHUNK_ATTEMPTS=3` and the comment about "a crash mid-Whisper" apply unchanged to an
  in-process Parakeet decode; with in-process decoding a crash takes the app down, so the
  `processing`-is-retryable rule is what prevents lost chunks.
- Nothing in this area depends on faster-whisper, Moonshine, Qwen3-ASR or MLX.

## Dependencies on other areas

Calls out:
- a24 meeting engine: `meeting/interfaces.py` (`OpResult`, `TranscriptSegment`),
  `meeting/time_utils.py` (`utc_now_iso`, `parse_meeting_time`, `as_local_time`,
  `meeting_duration_s`).
- a29 meeting intelligence: `meeting/fast_state.py:FAST_HANDLERS` (dispatch table member).
- a09 history: `services/titles.py` (`title_error`, `normalize_title`, 200-char contract),
  `services/database.py` (DatabaseManager, schema, FTS and delivery triggers),
  `services/dictation_stats.count_features` (before delete).
- a14: `services/models.py` table definitions.
- a12: `services/vocabulary.py:replace_terms`.
- a10: `services/settings.py:resolve_meeting_report_views`, consent constant.
- `services/automation/events.clear()`, `services/remote_records/sync.record_sync`,
  `services/text_llm.snapshot_from_meeting` (undecided cloud/sync areas).

Called in by: a24 `meeting/engine.py` (store, segment handler, repository, repair, delete,
intent), `meeting/recovery.py`, `meeting/asr/engine.py` (correct_text, vocabulary_hint),
a29 agent (`agent/prompts.py`, `tool_specs.py`, `tool_policy.py`, `scheduler.py`,
`insight_review.py`, `custom_report.py`, `citation_verifier.py`, `live_signals.py`,
`voice_actions.py`, `reinsight.py`, `refinalize.py`), web (`meeting/web/api.py` exports, delete,
state; `meeting/web/archive.py` open_store), a31 Qt (`past_meetings_panel.py`,
`meeting_mode_tab.py`, `meeting_export_dialog.py`), a04 `services/runtime/meeting.py`, a09
`services/history_calendar.py`, `services/remote_records/kinds.py`, benchmarks.

## Tests

| Test file | Pins |
|---|---|
| `tests/test_meeting_state.py` (1230 l) | op rules: duplicates (same/cross card, live_notes exemption), timeline start_s, revision gating, protection, soft delete, evidence union and undo restore, actor permissions, participant kinds/provisional, question caps/thresholds, speaker pins, seq, subscribers, persistence failure atomicity, snapshot round trip, finalization coerce/legacy/historical/pill/card_deferred, spoken-note protection |
| `tests/test_meeting_state_copy_on_write.py` | candidate isolation on failed write, ownership of op/runtime values, `_OP_DOMAINS` covers every handler, title-only reconciliation |
| `tests/test_meeting_repository.py` (629 l) | schema/WAL/migration, CRUD, interrupted detection, rename, past-meeting page (bounded/search/aggregates), chunk lifecycle/retry budget/idempotent commit, keyset paging ties, meeting scoping, FTS search + orphan-free delete, mirrors and audit, final transcript remap rules, segment_flags, mark_chunks_done |
| `tests/test_meeting_delivery_cursor.py` | delivery seq never reused, late old-start segments, monotonic cursor, upgrade backfill |
| `tests/test_meeting_corrections.py` | term rule eligibility, whole-word/casefold/single-pass/longest, literal regex chars, vocabulary bounds, guidance prompt, corrected reads with raw preserved, provider caching |
| `tests/test_meeting_scoped_corrections.py` | FNV-1a on UTF-8, occurrence corrections persisted/exported/undoable, suspension on base change, stale selection rejection, polish cannot bake corrections |
| `tests/test_meeting_repair.py` | repair builders and store application (evidence-id fuzzy repair tests there belong to a29) |
| `tests/test_meeting_insight_review.py` | review op flow (many tests also cover the a29 worker/HTTP side) |
| `tests/test_meeting_custom_report.py` | report ops lifecycle, export section, heading demotion; rest is a29 worker / web |
| `tests/test_meeting_intent.py` | intent op, undo, round trip, export brief |
| `tests/test_meeting_title_consistency.py` | title contract across repo/store/MCP/web |
| `tests/test_meeting_revise_segment_text.py` | segment text op |
| `tests/test_meeting_stored.py`, `tests/test_meeting_content.py` | load_state modes, content summary, preview |
| `tests/test_meeting_data_lifecycle.py` | delete/tombstone/restore/clear/orphans (playback tests belong to another area) |
| `tests/test_meeting_export.py`, `tests/test_meeting_export_bulk.py`, `tests/test_meeting_export_features.py` | export structure, toggles, token stripping, bulk filters/files/stems, UTC vs legacy local dates |

Golden-vector candidates (pure, deterministic given fixed ids/clock): `apply_ops` over op scripts
(inject id and clock generators), `MeetingState.from_dict/to_dict`, `FinalizationState.coerce/
normalize_historical/history_pill`, `corrections.correct_segment_text` + `segment_fingerprint` +
`replace_terms`, `repair.build_*_ops`, `export_markdown/export_json/export_transcript_txt`,
`bulk.meeting_file_stem`/`filter_export_meetings`, `repository._match_live_to_offline` +
`_remap_state_evidence` (operate on dicts), `demote_headings`. Fix the TZ when generating.

## Porting notes

Suggested decomposition (namespace `fv::meeting`):
- Qt-free core library `src/meeting` (like `src/engine`): `state/schema` (structs + JSON),
  `state/patches` (one function per op, same reason strings), `state/review`,
  `state/custom_reports`, `state/repair`, `corrections`, `export/*`, `content`/`stored` helpers,
  and a `MeetingRepository` interface. Needs a JSON library (nlohmann ordered_json) and a
  Unicode regex (PCRE2) unless the owner allows QtCore here.
- Persistence library: `SqliteMeetingRepository` over sqlite3 (FTS5) sharing one connection pool
  with the app database owner (a09). One writer connection guarded by a mutex mirrors SQLite's
  single-writer reality; keep `BEGIN IMMEDIATE` where Python uses it.
- Store: `MeetingStateStore` with `std::recursive_mutex`, subscriber callbacks; the Qt layer wraps
  it in a QObject that re-emits batches as queued signals (subscribers must stay non-blocking).
- `data_lifecycle` with std::filesystem.

Order: schema + JSON round trip (golden vectors from Python snapshots) -> patches + store (with an
in-memory repo) -> corrections -> repository (against a copy of a real `openwhisper.db`) ->
stored/content -> exports -> repair -> review/custom reports (once the cloud decision is made)
-> data_lifecycle.

Rough size: ~6,500 C++ lines (schema 700, patches 1,100, review 400, custom reports 150, store
400, repair 350, corrections 300, stored/content 350, repository 1,700, data_lifecycle 200,
exports 800) plus tests.

Risks:
- Python string semantics: lengths are code points, `lower()` is Unicode, regex `\w` is Unicode;
  C++ byte lengths would change which ops are rejected and where corrections land.
- JSON byte-compatibility: data-size rule uses ASCII-escaped `json.dumps`; `state_json` LIKE search
  needs raw UTF-8 (no `\u` escapes); exports use indent 2; Python floats print as `90.0` vs `90`
  (an int `start_s` is coerced to float only for agent/system timeline adds).
- Shared database with the Python app during migration: schema, triggers, FTS and string
  timestamp formats must stay identical; SQLAlchemy booleans are stored as 0/1 ints.
- Deep-copy semantics in copy-on-write: the persist-before-publish guarantee must hold.
- Re-entrancy: subscribers called under the store lock; Qt must not do blocking work there.

Native gains: no per-batch deepcopy of large report trees (structural sharing or move semantics),
faster snapshot serialization (Python serializes the whole document to SQLite on every applied
op; consider writing only changed rows plus throttled snapshots, but only if `state_json` stays
the source of truth for compatibility), and FTS/aggregate queries without ORM overhead.
