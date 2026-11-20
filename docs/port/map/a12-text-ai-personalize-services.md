# Text models and AI cleanup, transforms, dictionary, vocabulary, snippets, TypeSafe

Area id: `a12-text-ai-personalize-services`. 19 Python files, about 6,700 lines, all under
`D:\coding\whisper_local\services\`. Every file was read in full. This area has **no Qt widgets**:
it is the service layer that the Personalize UI (a13), Settings → Models (a11/a15), the
dictation pipeline (a05), runtime/command mode (a04), actions (a19) and Meeting Mode (a24-a29)
call into. Paths below are relative to `services/` unless stated.

## What the user gets

- **Text-model endpoints ("profiles")**: one place that knows how to talk to a chat model.
  Built-ins: OpenAI, OpenRouter, five local servers (Ollama, LM Studio, vLLM, llama.cpp,
  LocalAI), Groq, OpenCode Go, OpenCode Zen; plus up to 50 user "custom" OpenAI-compatible
  endpoints (Chat Completions or Responses protocol). API keys are never in settings: only the
  name of the variable, resolved through the OS keyring, then the environment, then `.env`.
- **Model catalog and protocols**: model lists from each endpoint (OpenAI list filtered to chat
  models, OpenRouter server-side sort, Groq minus speech models, local servers asked for real
  context size/tools/thinking). A reviewed table routes OpenCode models over chat, Responses,
  Anthropic Messages or Google `generateContent`.
- **AI cleanup** of every transcript: Light / Medium / High presets or a custom prompt, plus
  learned rules, dictionary block, snippet guard, app-context blocks (composed in a05). Runs on
  any endpoint above or through an installed coding agent (Claude Code, Codex, OpenCode CLI).
  Hard wall-clock deadline and cancel so a slow model never holds the paste. Any failure pastes
  the raw text.
- **Thinking level** picker (Off … Ultra) that shows only the levels the selected runner/model
  accepts and sends the nearest accepted one.
- **Cleanup profiles** (named output formats with optional hotkey; starters "Support ticket",
  "Email") and **Transforms** (saved rewrite instructions; starters "Polish", "Make concise",
  "Fix grammar", "Prompt engineer").
- **Rewrite / Command Mode / Scratchpad** engine: apply an instruction to selected text, or write
  new text at the cursor; user-facing refusal strings.
- **Personal dictionary**: up to 500 words, starred first, "sounds like" variants that are
  replaced in every transcript, vocabulary block in the cleanup prompt, hints to the speech
  model, and **learning from corrections** (re-read the field 4 s after paste; a word swapped
  twice is learned; a word the user put back after AI cleanup changed it is learned at once and
  protected).
- **Snippets**: say a trigger phrase, get exact text (optionally light Markdown pasted as rich
  text), protected through AI cleanup via `[[S1]]` placeholders.
- **Fast judgments (TypeSafe "Jev")**: typed yes/no and multiple-choice judgments used by Meeting
  Mode and voice routing, over TypeSafe directly, OpenRouter's System One API, or one of the
  user's own text models (which can keep it all local).
- **Speech-to-text server presets** for the cloud/API transcription backend (OpenAI, Groq,
  Speaches, LocalAI, vLLM, whisper.cpp, Custom). Undecided backend.
- First-run **offer to use a signed-in Claude Code / Codex** as the cleanup runner.

## Feature inventory

Status legend: **port**, **drop** (engine-specific), **undecided** (undecided backend),
**other-platform** (only on another OS).

| ID | Feature | Exact user-visible behavior | Source | Status |
|---|---|---|---|---|
| A12-01 | Built-in text endpoints | 10 built-ins in this order: OpenAI (no base URL, `OPENAI_API_KEY`), OpenRouter (`https://openrouter.ai/api/v1`, `OPENROUTER_API_KEY`), Ollama `http://localhost:11434/v1`, LM Studio `http://127.0.0.1:1234/v1`, vLLM `http://127.0.0.1:8000/v1`, llama.cpp `http://127.0.0.1:9931/v1`, LocalAI `http://127.0.0.1:8080/v1` (all keyless by default), Groq `https://api.groq.com/openai/v1` `GROQ_API_KEY`, OpenCode Go `https://opencode.ai/zen/go/v1` `OPENCODE_GO_API_KEY`, OpenCode Zen `https://opencode.ai/zen/v1` `OPENCODE_ZEN_API_KEY` | text_llm.py:builtin_profiles, LOCAL_SERVER_DEFAULT_URLS | port |
| A12-02 | Local server URL/key override | Each local preset saves `<id>_base_url` and `<id>_api_key_env`; bare host gets `/v1`; credentials or query in URL rejected: "Use a server URL without credentials or query parameters."; a bad hand-edited URL falls back to the default silently | text_llm.py:save_local_server, normalize_local_server_url, _configured_builtin | port |
| A12-03 | Custom endpoints | Up to 50 (`custom_` + 8 hex id); name 1-80 chars ("Endpoint name is required", "Endpoint name must be 80 characters or fewer"); URL http/https ("Base URL is required", "Base URL must use http or https", "Base URL is missing a host"); key var empty or `^[A-Z_][A-Z0-9_]*$` ("API key variable must be empty or an uppercase identifier (A–Z, digits, underscore)"); protocol chat/responses ("Choose Chat Completions or Responses for this endpoint."); "Cannot edit a built-in endpoint", "Built-in endpoints cannot be deleted", "At most 50 custom endpoints can be saved" | text_llm.py:upsert_custom_profile, remove_custom_profile, parse_custom_profile | port |
| A12-04 | Credential status copy | "No API key required" / "{label} found" / "Requires {label} — add it in Settings → App → API keys"; labels "OpenAI API key", "OpenRouter API key", "{name} API key" for other built-ins, raw var name for customs | text_llm.py:credential_label, credential_status | port |
| A12-05 | API key verification (text) | One authenticated request (OpenRouter: `GET /auth/key`; others `GET /models`), 10 s. Messages: "{host} rejected the key (HTTP 401).", "{host} accepted the key but denied access (HTTP 403).", "Couldn't reach {host}.", "{host} answered HTTP {code}.", "Verification failed ({ExcName}).", OK "{host} accepted the key."; OpenCode OK: "Catalog reachable. OpenCode's public catalog cannot verify a key; a model request is required." Never shows or logs the error body | text_llm.py:verify_api_key | port |
| A12-06 | Model list fetch | OpenAI: keep ids starting `gpt- chatgpt- o1 o3 o4` without `audio realtime tts whisper embedding moderation dall-e transcribe image search instruct`; OpenRouter: `sort` query unless "alphabetical" (server order kept); Groq: drop `whisper tts orpheus guard compound`; others sorted A-Z; self-hosted → server discovery (A12-08). Failure propagates (UI lets user type an id) | text_llm.py:list_chat_models, filter_openai_chat_models | port |
| A12-07 | OpenRouter model sort options | alphabetical, most-popular, top-weekly, newest, pricing-low-to-high, pricing-high-to-low, context-high-to-low, throughput-high-to-low, latency-low-to-high (setting `transcript_cleanup_model_sort`) | settings.py:TranscriptCleanupModelSort; text_llm.py:list_chat_models | port |
| A12-08 | Local server model discovery | Reads each server's own route for context, tools, thinking; drops embedding/speech/rerank models; remembers specs per base URL for the session (asked once, 3 s budget before first cleanup/judgment) | local_text_servers.py:discover_chat_models; text_llm.py:learn_server_models | port |
| A12-09 | Local server detection | Probes loopback URLs in parallel (0.6 s timeout, no retry) and names the server: LocalAI `/.well-known/localai.json`, Ollama `GET /` "Ollama is running", llama.cpp `/props` build_info or 401 "Invalid API Key", vLLM `/version` + models owned_by/401, LM Studio `/api/v1/models` or `/api/v0/models` | local_text_servers.py:detect_running_servers, identify_server, _identify | port |
| A12-10 | Model spec / protocol routing | OpenAI (id lowercased, `ft:` prefix and everything after the first `:` of a fine-tune id dropped first): o1/o3/o4/gpt-5/gpt-6 → Responses + reasoning, gpt-4o/gpt-4.1 → Responses only when the id has none of `audio realtime search tts transcribe`, else chat; ctx 131072 / out 16384; gpt-6-luna maps Off→"none". OpenCode: fixed route table (responses/chat/anthropic/google) + capability table; unknown id: "{model} has no supported API route for {name}. Choose a supported model." Groq speech model: "Choose a Groq text-generation model." Empty: "Choose a text model first." | text_model_catalog.py:model_spec, _ROUTES, _CAPABILITIES | port |
| A12-11 | Multi-protocol generation | One `generate()` for Chat Completions, Responses (`store:false`, encrypted reasoning), Anthropic Messages (`anthropic-version: 2023-06-01`), Google `generateContent`; tool calls round-trip opaque provider data; truncation/refusal/filter → error ("Model output was incomplete ({reason}).", "Model refused the request.", "Model returned non-text content.", "Model returned no candidate (possibly blocked).", "This model does not support tool calls.") | text_generation.py:generate and adapters | port |
| A12-12 | Provider headers | OpenRouter `X-Title: OpenWhisper`; OpenCode `User-Agent: OpenWhisper`, Go adds `x-opencode-session: <uuid>` | text_llm.py:provider_headers | port (decide branding string) |
| A12-13 | Consent/destination copy | "your local server at {host}", "OpenRouter (openrouter.ai)", "OpenAI (api.openai.com)", "{name} ({host})" | text_llm.py:consent_destination | port |
| A12-14 | Endpoint snapshots for meetings | Non-secret JSON (`profile_id,name,kind,base_url,api_key_env[,model_metadata][,protocol]`) stored on the meeting row so reruns use the original endpoint even after deletion; a deleted endpoint never falls back to a cloud provider | text_llm.py:TextLLMSnapshot, snapshot_from_mapping, snapshot_from_meeting, profile_from_agent_config | port |
| A12-15 | AI cleanup runner | `TranscriptCleanup.cleanup()`: empty → unchanged; unavailable → raw; system+user messages; empty reply/failure/timeout/cancel → raw with `last_error` set ("empty input", "cleanup unavailable", "empty response", "canceled", "timed out after N s", exception text) | transcript_cleanup.py:TranscriptCleanup.cleanup | port |
| A12-16 | Cleanup deadline and cancel | Hosted: 8 s per attempt, 1 retry, give up at 8×2+1 = 17 s wall clock; self-hosted 120 s per attempt; agents 60 s; batch (`timeout_s` given) uses 2 retries; `deadline_s` caps the whole call; cancel checked every 50 ms; request left running on a daemon thread, result discarded | transcript_cleanup.py:_call_with_deadline, _attempt_budget | port |
| A12-17 | Cleanup via installed agent | Provider `claude_code`/`codex`/`opencode_cli`: runs the agent with tools disabled, closing "Return only the finished text requested by the instructions, without commentary."; cancel also stops the CLI | transcript_cleanup.py:_cleanup_with_agent | port (needs a20/a26) |
| A12-18 | Reasoning request options | Off or non-OpenAI/OpenRouter → `temperature: 0`; OpenRouter → `extra_body.reasoning.effort`; OpenAI → `reasoning_effort`; hosted catalogs strip these and use their spec | text_llm.py:chat_request_options; text_generation.py:_set_chat_options, _set_native_options | port |
| A12-19 | Thinking levels | Order off, minimal, low, medium, high, xhigh, max, ultra; labels "Off","Minimal","Low","Medium","High","Extra high","Max","Ultra"; endpoint base off/low/medium/high ± catalog; Claude Code low..max; Codex per-model from its cache (intersection fallback); others low/medium/high; nearest offered, lower on tie; none → "off" | thinking_levels.py | port |
| A12-20 | Cleanup levels / presets | Off / Light / Medium (default) / High built from config sentences; "Custom" label when a non-built-in custom prompt is saved; a saved copy of an old default counts as no custom prompt | cleanup_prompts.py; config.py:541-640 | port |
| A12-21 | Inline-lists block | Medium/High without custom prompt: unknown app → "never add line breaks" sentence; terminal → terminal sentence; else nothing | cleanup_prompts.py:inline_lists_block; config.py:591-601 | port |
| A12-22 | Learned rule polishing | Turns a spoken instruction into one rule via `TRANSCRIPT_CLEANUP_RULE_POLISH_PROMPT`; returns (rule, error), falls back to stripped instruction; "empty instruction" | transcript_cleanup.py:polish_cleanup_rule | port |
| A12-23 | Cleanup profiles | Starters shown only until a library is saved; id/name/instructions required; hotkey normalized; `use_learned_rules` default true; validation: "Give the profile a name.", "Use a name with 80 characters or fewer.", "Add instructions for the output you want.", "A profile with that name already exists.", "That shortcut is already used by {x}. Choose another."; deleting the Quick Record profile clears it | cleanup_profiles.py | port |
| A12-24 | Profile prompt | Fixed preamble + spoken-corrections sentence + "Output instructions:\n…"; with rules appends learned rules and the conflict sentence | cleanup_profiles.py:compose_profile_prompt | port |
| A12-25 | cleanup_may_run | True if cleanup on, a Quick Record profile chosen, any profile/transform hotkey, or a Command Mode hotkey (used to decide whether to warm the client) | cleanup_profiles.py:cleanup_may_run | port |
| A12-26 | Transforms | Max 50; starters until saved; name ≤ 80; "Give the transform a name.", "Use a name with 80 characters or fewer.", "Add an instruction for how to change the text.", "A transform with that name already exists.", "You can keep up to 50 transforms. Delete one to add another.", "This transform has no id.", shortcut conflict text | text_transforms.py | port |
| A12-27 | Rewrite / generate at cursor | Selection → rewrite prompt + guard; none → generate prompt with `<<NEEDS_SELECTION>>` sentinel; 30 s budget, 60 s when message > 2000 chars, never below client attempt timeout; > 20,000 chars refused. Messages: "Didn't catch an instruction", "Select the text to change first", "Select less text to rewrite (up to 20,000 characters)", "Set up AI cleanup to rewrite text", "Canceled", "The AI model took too long. Try a faster one in AI cleanup", "The AI model sent back nothing", "The AI model failed: {first line ≤120 chars…}" | text_rewrite.py | port |
| A12-28 | Provider readiness | Ready when profile + model + key (or agent usable); non-blocking variant returns None while an agent lookup runs | text_rewrite.py:provider_ready, provider_readiness | port |
| A12-29 | Dictionary storage | ≤ 500 terms, newest first, ≤ 64 chars, ≤ 5 "sounds like" each, dedupe casefold keeping first, stable ids `t`+sha1[:11] for id-less entries | dictionary.py:_parse, _stable_id | port |
| A12-30 | Dictionary edits | Add on top or merge; errors "Type a word or name first.", "Keep words under 64 characters.", "Keep each “sounds like” under 64 characters.", "Up to 5 “sounds like” spellings per word.", "That's already in your dictionary.", "Your dictionary is full (500 words).", "That word is no longer in your dictionary."; star, delete, undo learned, clear New badges; summary "No words yet" / "12 words · 2 new" | dictionary.py:add_term…summary | port |
| A12-31 | "Sounds like" replacement | Whole-word, case-insensitive (Unicode casefold incl. İ, ß), longest first, single pass, starred term wins a shared variant, learned terms never replace | dictionary.py:apply_replacements; vocabulary.py:replace_terms | port |
| A12-32 | Dictionary prompt block | "Vocabulary — when one of these is what was said, including near-miss mishearings, spell it exactly: a, b." ≤ 200 terms / 2400 chars; protected (≤ 50) adds " The speaker put these back after an earlier cleanup changed them; never replace or respell them: …." | dictionary.py:prompt_block | port |
| A12-33 | Recognition hints | Starred first, first 50 terms, only while "Steer the speech model" (`dictionary_steer_recognition`, default on) | dictionary.py:recognition_phrases | port (Parakeet support TBD) |
| A12-34 | Spelling rules → dictionary | Learned rules ≤ 120 chars containing "spell…" and no except/unless/when/whenever/if/only/but, with 1 quoted term, or 2 quoted with "as"/"not", move into the dictionary | dictionary.py:spelling_rule, move_spelling_rules | port |
| A12-35 | Learning from corrections | 4 s after paste, re-read the field (only if the job captured caret text from another app, learning and text reading on); exactly one pasted word swapped for one word, ≥ 2 anchors, ≥ 3 pasted words, word ≥ 3 chars, has a letter, not only case/apostrophe/hyphen change, not in COMMON_WORDS; 2 sightings (in-memory), or 1 if it reverts an AI-cleanup change (then protected); notifies a callback | dictionary.py:schedule_learning … _learn | port |
| A12-36 | Snippets storage/validation | ≤ 200; trigger 2-60 chars; text ≤ 10,000; messages "Add a trigger phrase to say.", "Use words you can say for the trigger.", "Use 2 to 60 characters for the trigger.", "Add the text to insert.", "Keep the text to 10,000 characters or fewer.", "Another snippet already uses this trigger. Choose a different phrase.", "You have 200 snippets, the most OpenWhisper keeps. Delete one to add another." | snippets.py:load_snippets, validate_snippet | port (rename brand in copy) |
| A12-37 | Snippet trigger notes | "Another snippet already uses this trigger.", "Includes “X”. Saying the full phrase inserts this snippet.", "Also part of “X”, which wins when you say it in full.", "Sounds close to “X”; speech might mix them up." (SequenceMatcher ratio ≥ 0.85), "A single word can come up in everyday speech; a short phrase is safer." | snippets.py:trigger_note | port |
| A12-38 | Snippet expansion | Whole utterance = trigger → skip cleanup, paste snippet; inner triggers → `[[S1]]`… placeholders (longest first, no overlap, words separated only by spaces/commas/dashes), prompt guard sentence, expanded after cleanup; lost/duplicated/invented placeholder → fall back to uncleaned text with expansions | snippets.py:plan_expansion, prompt_guard, expand | port |
| A12-39 | Light Markdown snippets | Bold, italic, links (http/https/mailto/tel, `www.` → https), bare URLs, line breaks, ul/ol (with `start`), escapes; plain-text variant drops markers and writes "label (url)" | snippets.py:markdown_to_html, markdown_to_plain | port |
| A12-40 | TypeSafe judge (Jev) | POST `https://api.typesafe.ai/v1/systemone`, model `jev-1.13.0`, key `TYPESAFE_API_KEY`, 4 s budget, request ≤ 24,000 chars, strict answer validation, every failure → no judgment, warnings at most 1/min | typesafe.py:TypeSafeJudge | port |
| A12-41 | Jev via OpenRouter | `https://openrouter.ai/api/v1/systemone`, model `typesafe/jev-1.13`, OpenRouter key and header | typesafe.py:OPENROUTER_ROUTE | port |
| A12-42 | Judgments by own text model | Same questions asked of the AI insights or cleanup model (30 s budget, temperature 0, JSON schema, thinking off); noul and choice only; confidence `(p-1/n)/(1-1/n)`; context-window check; route is local when the profile is loopback | text_judge.py; typesafe.py:text_model_route | port |
| A12-43 | Keep-alive connection pool | ≤ 4 idle per origin, 115 s idle window, reused connection gets half the budget (≥ 1 s), one fresh resend on drop; proxied URLs go through a one-shot request | typesafe.py:_KeepAlivePool, _http_post | port |
| A12-44 | Judge key verification | "Paste a key first.", "{host} accepted the key and {model} answered in {t:.2f} s.", 401/402/403/404(OpenRouter)/429/other messages, "Couldn't reach {host} ({Exc}).", "{host} accepted the key but answered oddly: {exc}."; text route: "Choose a model for {name} on the Models page first.", context-too-small advice per server, "{where} answered in {t} s." | typesafe.py:verify_key, _status_outcome; text_judge.py:verify_text_model | port |
| A12-45 | FollowingJudge | Looks up the configured judge per question so a running meeting follows a route change | typesafe.py:FollowingJudge | port |
| A12-46 | Speech server presets | OpenAI (curated models), Groq (fixed `whisper-large-v3-turbo`, `whisper-large-v3`), Speaches, LocalAI, vLLM, whisper.cpp (`language=auto`), Custom; 120 s hosted / 600 s self-hosted; "Audio stays on this computer: it goes to …" / "Audio is uploaded to …" | speech_endpoints.py | undecided |
| A12-47 | Recognition context to engines | language code + phrase budgets: faster-whisper hotwords ≤ 300 chars; OpenAI keywords ≤ 50×64; Whisper prompt ≤ 600 chars "a, b."; engine_support model/after | recognition_context.py | port (struct) / drop (whisper) / undecided (OpenAI) |
| A12-48 | Cleanup agent offer | Once, when cleanup is on, provider never chosen, offer unanswered and Claude Code or Codex is installed and signed in (Claude first); accept sets provider and remembered model; decline never asks again | cleanup_agent_offer.py | port |
| A12-49 | Verified TLS context | System roots plus bundled CA bundle, built once and shared | http_tls.py | port (mostly free in Qt) |
| A12-50 | Lazy SDK import | openai SDK imported on first client (0.7 s); `defer_client=True` | text_llm.py:_openai_class; transcript_cleanup.py:__init__ | drop (Python-only; keep "no network at startup") |

### Added by check

Behavior the table above does not state (all **port** unless marked). Sources are in `services/`.

| ID | Feature | Exact behavior | Source |
|---|---|---|---|
| A12-51 | Cleanup prompt text is data | Port the sentences verbatim: `config.py:541-690` holds `TRANSCRIPT_CLEANUP_PROMPT` (legacy default, also the fallback when `cleanup()` gets an empty system prompt), INTRO, BASICS, LIGHT_ONLY, TIGHTEN, SPOKEN_CORRECTIONS, SPOKEN_LISTS, PARAGRAPHS, UNKNOWN_APP_LINES, TERMINAL_LINES, LANGUAGE, GUARD and `TRANSCRIPT_CLEANUP_RULE_POLISH_PROMPT`. Level order: light = INTRO BASICS LIGHT_ONLY LANGUAGE GUARD; medium = INTRO BASICS SPOKEN_CORRECTIONS SPOKEN_LISTS PARAGRAPHS LANGUAGE GUARD; high = medium with TIGHTEN inserted after BASICS; joined by one space. Starter profile/transform instructions (cleanup_profiles.py:26-43, text_transforms.py:26-52), the rewrite/generate/guard prompts (text_rewrite.py:43-65), text_judge `SYSTEM_PROMPT` and the profile preamble (A12-24) are likewise verbatim data | config.py; text_rewrite.py; text_judge.py |
| A12-52 | Default cleanup model per endpoint | OpenRouter (id or kind) → `openrouter/free`; OpenAI → `gpt-6-luna`; every other endpoint → "" (user must pick). Default provider `openrouter` | text_llm.py:default_model_for_profile; config.py:533-535 |
| A12-53 | Provider display names | `claude_code` → "Claude Code", `codex` → "Codex", `opencode_cli` → "OpenCode"; unknown id → the raw id; also `provider_env_key` returns "" for agents and falls back to `OPENAI_API_KEY` for an unknown id | text_llm.py:profile_display_name; transcript_cleanup.py:provider_env_key |
| A12-54 | Key verification host fallback | Host shown is the base URL hostname, `api.openai.com` when the profile has no base URL, "the endpoint" when the URL has no host | text_llm.py:verify_api_key |
| A12-55 | Client construction errors | "No API key found for {name} (set {VAR})" from client creation and from server discovery; "'{id}' is not a built-in local server." (save_local_server); "Invalid saved text model metadata"; "Unsupported saved text protocol."; "Unknown text model endpoint '{id}'" (list_cleanup_models) | text_llm.py; text_model_catalog.py; transcript_cleanup.py |
| A12-56 | Cleanup request timeout overrides | `cleanup(timeout_s=)` sets a per-request HTTP timeout and switches to 2 retries; rewrite passes `timeout_s = deadline_s = wait_s`, so its single wall-clock limit is 30 s (60 s long text) or the client attempt timeout if larger (120 s on a self-hosted server); a self-hosted profile triggers `learn_server_models` before each request | transcript_cleanup.py:cleanup, _attempt_budget, _generate; text_rewrite.py:rewrite_with |
| A12-57 | Dictionary: re-add and edit semantics | Re-adding an existing word (case-insensitive) keeps its place, takes the new spelling, merges "sounds like" variants (still capped at 5), ORs the star, and clears `learned` and `new` (the user now owns it); no change → "That's already in your dictionary."; editing a word to another existing word is the same error; `update_term` replaces spelling and variants, clears `learned`/`new`; `set_starred` also clears `new`; `delete_term` returns the removed entry (or none), `undo_learned` only removes entries still marked learned; `clear_new` returns how many badges it cleared | dictionary.py:add_term, update_term, set_starred, delete_term, undo_learned, clear_new |
| A12-58 | Dictionary: "sounds like" entry parsing | `parse_heard` splits a field at commas, semicolons and newlines and collapses whitespace per piece; variants equal to the term (case-insensitive), empty, or over 64 chars are dropped; more than 5 distinct → "Up to 5 “sounds like” spellings per word."; whitespace in all terms is collapsed to single spaces (`_clean`) | dictionary.py:parse_heard, _clean, _variants, _checked_heard |
| A12-59 | Dictionary: add_learned rules | No-op when `dictionary_learn_enabled` is off, word empty or > 64 chars, dictionary full, or word already present; a present word is changed (and returned) only when `protected` is requested and it was not protected yet; a new learned word goes on top with `learned` and `new` set; `find_term` looks a word up case-insensitively; learning logs only "Dictionary learned a word from a correction" (never the word); the learned callback gets the term text and exceptions in it are swallowed | dictionary.py:add_learned, find_term, _learn |
| A12-60 | Learning: tokenization and word list | Tokens are whitespace-split pieces with `.,!?;:"“”'‘’()[]{}<>«»…` stripped from both ends; a candidate must fully match `[^\\W_]+(?:['’-][^\\W_]+)*`, so tokens with `_ / @` are never learned; case-folded, aligned with SequenceMatcher (autojunk off); an insert at the very start or end of the aligned text is ignored; the learned-word stoplist `COMMON_WORDS` (articles, pronouns, auxiliaries, weekdays, months, number words 1-10 and 100/1000, greetings, "yes ok okay thanks please sorry" and similar, about 250 words) is data to port verbatim; `reverted_cleanup` = restored word is in the pre-cleanup text and the replaced word is not; the field is `before + selected + after` of the re-read context | dictionary.py:_tokens, _correction, _learnable, reverted_cleanup, COMMON_WORDS |
| A12-61 | Learning gate and timing | Scheduled only when pasted text is non-blank, a job exists, its focus snapshot has an identity that is not the app itself, and caret text was known at start; delay `LEARN_DELAY_S = 4.0`; at re-read time both `dictionary_learn_enabled` and `app_context_read_text` are re-read from fresh settings; the file write is handed to a second 0-delay timer so the capture thread never writes | dictionary.py:schedule_learning, _reread, _on_reread |
| A12-62 | Dictionary spelling-rule shapes | Rule must contain "spell/spells/spelled/spelling" and none of except/unless/when/whenever/if/only/but (word match, case-insensitive), be ≤ 120 chars, quotes are straight or curly double quotes, quoted text ≤ 64 chars and non-empty. One quoted → term; two quoted with "as" → (second = term, first = heard); two with "not" → (first = term, second = heard); "as" is checked before "not". `move_spelling_rules`: a rule whose word cannot be added stays a rule, unless the word is already in the dictionary (then it counts as moved); `spelling_rules` lists the candidates for the Learned rules page | dictionary.py:spelling_rule, spelling_rules, move_spelling_rules |
| A12-63 | Dictionary prompt block budget | Costs `len(term)+2` per term against the 2400-char budget and 200-term cap, starred first; protected list is built from the ranked list (starred first), up to 50; protected terms past the vocabulary cap still appear in the protected sentence | dictionary.py:prompt_block |
| A12-64 | Snippet save/delete semantics | `save_snippet` validates, strips the trigger, coerces `formatted` to bool, replaces in place by id or appends; `delete_snippet` silently ignores an unknown id; ids are `uuid4().hex`; trigger length rules: normalized words must be ≥ 2 chars, the raw trigger ≤ 60 chars; a trigger containing a `[[S1]]`-style placeholder is rejected with "Use words you can say for the trigger."; text must be non-blank and ≤ 10,000 chars; the 200 cap does not block editing an existing snippet; `load_snippets` silently drops invalid, duplicate-id and duplicate-trigger rows | snippets.py:validate_snippet, save_snippet, delete_snippet, load_snippets, new_snippet_id |
| A12-65 | Snippet trigger-note order | Duplicate first; then, per other snippet in list order, "Includes" (other trigger is inside this one) before "Also part of" (this one is inside the other), both on whole-word boundaries; then near-match (ratio ≥ 0.85, character-level SequenceMatcher on normalized text) for any other; then the single-word note; `find_duplicate` is by normalized trigger and ignores `exclude_id` | snippets.py:trigger_note, find_duplicate |
| A12-66 | Snippet placeholder guard text | One: "The transcript contains the placeholder [[S1]], which stands for text inserted later; keep it exactly once, unchanged and in its place." Several: "The transcript contains the placeholders [[S1]], [[S2]] and [[S3]], each standing for text inserted later; keep every one exactly once, unchanged and in its place." (Oxford-comma-free, last joined with " and ") | snippets.py:prompt_guard |
| A12-67 | Snippet whole-utterance and rich output | A whole-utterance match returns `(plain_text, snippet_html, True)` regardless of cleanup text; `html` is built only when any used snippet is `formatted` (then non-formatted text between placeholders is HTML-escaped with `quote=False`, lines joined by `<br>`, and an unformatted snippet inside a rich paste is escaped the same way); plain text of a formatted snippet = Markdown stripped | snippets.py:expand, plain_text, snippet_html, _escape_lines |
| A12-68 | Light-Markdown grammar | Inline alternatives in priority order: backslash escape of one of ``\\ ` * _ [ ] ( ) # + - . !``; `[label](href)` (href no spaces/`)`; label rendered recursively; unsafe scheme → left as literal text; allowed `http:// https:// mailto: tel:`, `www.` → `https://`); bare `http(s)://` URL (trailing `.,;:!?)]'` not included); `**bold**` and `__bold__` (word-bounded, no inner edge spaces); `*italic*` and `_italic_` (word-bounded); lists: up to 3 leading spaces then `-*+` or `1-9 digits` + `.` or `)` then space/tab; `<ol start="N">` when the first number is not 1; consecutive same-kind lines form one list; blank lines next to a list are trimmed; text blocks joined with `<br>`; CR and CRLF normalized; plain-text variant keeps the list markers and numbers but strips inline markers; link plain form is the bare href when label equals href, else "label (href)". Attribute values escape quotes (`html.escape` default), body text does not | snippets.py:_INLINE, _LIST_ITEM, _inline_html, _inline_plain, markdown_to_html, markdown_to_plain |
| A12-69 | Rewrite prompt and outcomes | Rewrite sends the selection as the user message and the instruction in the system prompt; write-at-cursor sends the instruction as the user message; refused selections (> 20,000 chars) are checked only when a selection exists; a selection of only whitespace counts as no selection; an empty/blank instruction → "Didn't catch an instruction" before anything else; an empty result → "The AI model sent back nothing"; result is stripped; `<<NEEDS_SELECTION>>` anywhere in a write-at-cursor result → "Select the text to change first"; `rewrite_standalone` uses its own deferred cleaner so it never touches the dictation client, and answers "Set up AI cleanup to rewrite text" when not ready, not configurable or unavailable; `dictionary_block` swallows errors and returns "" | text_rewrite.py |
| A12-70 | Cleanup `last_error` is the contract | Callers distinguish "no change" from failure only by `last_error is None`; initial value "not run"; agent path errors: "Agent cleanup failed" (no detail), "empty response"; a cancel that arrives before the call returns "canceled" without sending | transcript_cleanup.py |
| A12-71 | Fast-judgment usage and labels | Each judge counts `requests`, `failures`, `input_tokens` (provider-reported), `last_latency_s`, under a lock; log label "TypeSafe", "TypeSafe via OpenRouter" or "Fast judgments on {name} ({model})"; warnings are at most one per 60 s, others at debug level; `model` is exposed (FollowingJudge reports "" when it has no judge); the TypeSafe judge refuses an empty key ("{name} API key is required") and an empty question set; a non-JSON-serializable state is skipped; a `ValueError` for a text-model route without a profile | typesafe.py:_RouteJudge, TypeSafeJudge, FollowingJudge; text_judge.py:TextModelJudge |
| A12-72 | Judge verification copy (exact) | 401 "{host} rejected the key (HTTP 401)."; 402 "{host} accepted the key, but the account is out of credits (HTTP 402)."; 403 "{host} accepted the key but denied access (HTTP 403)."; 404 on the OpenRouter route only "{host} has no route to {model} for this key (HTTP 404). Check the provider and privacy settings on your OpenRouter account."; 429 is a success "{host} accepted the key but is rate limiting it (HTTP 429)."; other non-200 "{host} answered HTTP {n}."; text route: 401/403 "{host} rejected the key (HTTP n).", 404 "{host} has no model {model} (HTTP 404).", timeout "{where} did not answer within {t} s.", unusable answer "{where} answered, but not with a usable judgment ({why}). Models that follow JSON instructions, with thinking off, work best.", truncated "{where} did not finish an answer ({why}).", context advice `ollama` "In Ollama, give the model a num_ctx of {tokens} or more (PARAMETER num_ctx in its Modelfile).", `lm_studio` "In LM Studio, load the model with a context length of {tokens} or more.", `vllm` "Start vLLM with --max-model-len {tokens} or more.", `llama_cpp` "Start llama-server with --ctx-size {tokens} or more.", `localai` "In LocalAI, set context_size to {tokens} or more in the model's configuration.", other "Load the model with a context window of {tokens} tokens or more."; `where` = "{name} · {model}"; a self-hosted server's model list is re-read (3 s) before the check; the check also proves a full `MAX_STATE_CHARS` judgment would fit | typesafe.py:_status_outcome; text_judge.py:verify_text_model, _failure_reason, _MORE_CONTEXT |
| A12-73 | Text-model judge wire details | Sent: system = `SYSTEM_PROMPT`, user = JSON `{"state", "questions"}` (`ensure_ascii=False`); response_format `json_schema` `{name:"judgments", strict:true}` with each yes/no as `number` and each choice as `{option: enum of "1".."n", probability}`; `temperature 0`, thinking off, `max_tokens = 64 + 40 × questions`; choice options are numbered 1..n in dict order and shown as `{name, means?}`; a `score` question is unsupported and raises; the reply's outermost `{…}` is parsed after removing `<think>…</think>`; a yes/no is validated downstream (must be in [0,1]); a choice with an unknown option number yields a null choice that validation rejects; the request budget left after the model-list read is passed as the client timeout; a context-window check (`(len(SYSTEM_PROMPT)+len(content))//3 + 32 + max_tokens > window`) fails before sending; client cache key = (profile id, kind, base URL, key variable, key) | text_judge.py:render, parse, request, _fit_context, _client |
| A12-74 | Judge route resolution | `text_model_route`: `typesafe_text_model` = cleanup → cleanup provider+model, else meeting LLM profile+model; unknown profile → OpenRouter built-in; endpoint = profile base URL or `https://api.openai.com/v1`; route name = profile name; 30 s budget; headers from `provider_headers`. `route_key`: no credential variable → dummy key; `key_present`/`is_configured` split "off" from "on but unusable"; `judge_from_settings` returns a `TypeSafeJudge` for Jev routes and a `TextModelJudge` for the text route; `route_is_local` is true only for a loopback text route | typesafe.py:text_model_route, route_from_settings, route_key, key_present, is_configured, judge_from_settings |
| A12-75 | Judge route copy helpers | `key_label` "{name} API key"; `key_phrase` adds "an" before a vowel-initial name, else "a" (for "Add …" copy) | typesafe.py:JudgeRoute |
| A12-76 | Thinking-level edge rules | Agents never offer Off; Codex: model from the setting or the configured default, per-model efforts from its cache, intersection of all models for an unknown model, `low/medium/high` when the cache is empty; endpoint levels for OpenAI are offered only if the spec is reasoning or no model is chosen yet; a catalog `thinking_levels` entry with a null value removes that level from the picker and any other key adds it; no offered levels → "off" is sent | thinking_levels.py |
| A12-77 | Reasoning formats | A model spec's `reasoning_format` is one of `openai`, `deepseek`, `qwen` or ""; chat sends `reasoning_effort` for `openai` (temperature dropped unless the level is "none"), `extra_body.thinking.type = enabled` plus `reasoning_effort` high/max for `deepseek`; `qwen` is accepted from saved specs but no request option is built for it; native protocols send `reasoning.effort` only for `openai` format; Responses adds `include: ["reasoning.encrypted_content"]` and drops `temperature` for reasoning models | text_generation.py:_set_chat_options, _set_native_options; text_model_catalog.py |
| A12-78 | Speech-server preset table (undecided) | OpenAI: no base URL, `OPENAI_API_KEY`, default `config.DEFAULT_API_MODEL`, not editable, "OpenAI's transcription models. Needs an OpenAI API key."; Groq: `https://api.groq.com/openai/v1`, `GROQ_API_KEY`, default `whisper-large-v3-turbo`, no catalog call, not editable, "Open-weights Whisper models hosted by Groq. Needs a Groq API key."; Speaches `http://127.0.0.1:8000/v1`, default `Systran/faster-whisper-small`, "A Speaches server. Lists the models it has downloaded."; LocalAI `http://127.0.0.1:8080/v1`, default `whisper-1`, catalog `/models/capabilities`, "A LocalAI server with a transcription model installed."; vLLM `http://127.0.0.1:8000/v1`, default `openai/whisper-large-v3-turbo`, "A vLLM server running a Whisper model (vllm serve)."; whisper.cpp `http://127.0.0.1:8080/v1`, default `whisper-1`, no catalog, `auto_language="auto"`, "whisper.cpp's whisper-server, started with --inference-path /v1/audio/transcriptions."; "Custom server" (id `custom`, no URL, default `whisper-1`, "Any server with an OpenAI-compatible /audio/transcriptions route."). Self-hosted = configurable non-custom preset or loopback; Custom decided by its address alone. Errors: "Unknown transcription server: {id}", "{id} has no server settings." | speech_endpoints.py |
| A12-79 | Recognition-context constants | `MAX_PHRASE_CHARS = 64`, `WHISPER_HOTWORDS_MAX_CHARS = 300`, `OPENAI_MAX_KEYWORDS = 50`, `OPENAI_PROMPT_MAX_CHARS = 600`, `config.MAX_RECOGNITION_PHRASES = 50`; context is passed to a backend only when it declares `supports_recognition is True`, otherwise the transcriber is called exactly as without context | recognition_context.py; config.py |
| A12-80 | Cleanup agent offer detail | `pending` also needs `transcript_cleanup_provider` never stored; `accept` keeps the remembered model for that agent (or "" for the agent's own default), writes it back into `cleanup_model_memory`, sets provider, model and the answered flag in one settings mutation, and rejects any id other than `claude_code`/`codex`; `decline` only sets the answered flag | cleanup_agent_offer.py |

## Modules

### text_llm.py (919 lines) - endpoint profiles, credentials, clients, catalogs
- Responsibility: profile model (`TextLLMProfile`, frozen), snapshots, settings persistence for
  custom endpoints and local-server overrides, key resolution, OpenAI-compatible client creation,
  key verification, chat-model listing, consent copy.
- Public surface: constants `PROFILE_KIND_*`, `LOCAL_SERVER_DEFAULT_URLS`, `LOCAL_SERVER_NAMES`,
  `LOCAL_SERVER_PROFILE_IDS`, `HOSTED_CATALOG_PROFILE_IDS`, `BUILTIN_KINDS`, `PROFILE_KINDS`,
  `AUTH_FREE_API_KEY="dummy"`, `SIDECAR_API_KEY_ENV="OPENWHISPER_LLM_API_KEY"` (used by a26
  sidecar), `MAX_PROFILE_NAME_LEN=80`, `MAX_CUSTOM_PROFILES=50`. Functions: `is_loopback_url`,
  `builtin_profiles/builtin_profile`, `normalize_base_url`, `validate_profile_name`,
  `validate_api_key_env`, `validate_text_protocol`, `new_custom_profile_id`,
  `snapshot_from_profile`, `snapshot_from_mapping`, `parse_custom_profile`,
  `list_custom_profiles`, `list_profiles`, `normalize_local_server_url`, `save_local_server`,
  `get_profile`, `known_profile_ids`, `custom_profiles_payload`, `upsert_custom_profile`,
  `remove_custom_profile`, `profile_display_name` (maps `claude_code`→"Claude Code",
  `codex`→"Codex", `opencode_cli`→"OpenCode"), `default_model_for_profile`, `lookup_env_value`,
  `resolve_api_key`, `credential_label`, `credential_status`, `connection_fingerprint`,
  `provider_headers`, `create_openai_client`, `verify_api_key`, `filter_openai_chat_models`,
  `list_chat_models`, `discover_server_models`, `learn_server_models`, `chat_request_options`,
  `consent_destination`, `destination_is_remote`, `snapshot_from_meeting`,
  `profile_from_agent_config`.
- Profile properties: `requires_api_key`, `is_local` (host in 127.0.0.1/localhost/::1),
  `self_hosted` (local preset kind or loopback), `refuses_unsupported_tools` (OpenAI/OpenRouter).
- Threading: pure functions plus blocking HTTP; `learn_server_models` must never run on the GUI
  thread (text_llm.py:813-838).
- Edge cases to keep:
  - `normalize_base_url` strips trailing `/`, drops fragment, keeps query (text_llm.py:221-232);
    local-server and speech URLs additionally reject user/password/query and add `/v1` when there
    is no path (text_llm.py:456-465).
  - `snapshot_from_mapping` accepts a dict or a JSON string; returns None on bad JSON, empty id,
    invalid URL, or non-OpenAI kind without URL; unknown kind → custom; invalid key var → "";
    `model_metadata` must have string `model` else raises ValueError (text_llm.py:295-363).
  - `to_profile` sets `builtin` only when id is a built-in id and kind equals id (text_llm.py:172-175).
  - `list_custom_profiles` skips non-`custom_` ids and duplicates, caps at 50 (text_llm.py:410-424).
  - `upsert_custom_profile` keeps an existing profile's protocol when none given (text_llm.py:536-538).
  - `resolve_api_key`: no key var → "dummy"; otherwise credential or None (text_llm.py:623-627).
  - `verify_api_key` never logs/surfaces provider error bodies (text_llm.py:699-737).
  - `_profile_or_meeting_default`: a missing provider resolves to the user's *current meeting*
    endpoint, never a fixed cloud one (text_llm.py:871-885). `snapshot_from_meeting` prefers
    `agent_endpoint_json`, then `agent_provider`, then fallback `openrouter`.
  - `create_openai_client` raises "No API key found for {name} (set {VAR})"; `max_retries=None`
    keeps SDK default 2.

### text_model_catalog.py (798 lines) - model specs and routes
- `TextModelSpec` (protocol chat, tools true, context_window 32768, max_output_tokens 4096,
  reasoning false, reasoning_format "", pi_compat None, thinking_levels None); `to_dict`.
- `_ROUTES` (OpenCode Go/Zen model → protocol) and `_CAPABILITIES` (per model context, output,
  reasoning, reasoning_format, pi_compat, thinking_levels) are data tables dated 2026-09-06;
  port verbatim (generate a C++ table or a JSON resource from the Python).
- `model_spec(profile, model)` decision order (text_model_catalog.py:139-168): empty → error;
  OpenAI recomputed every time (ignores saved metadata); saved metadata with same model; custom
  Responses endpoint → reasoning if prefix o1/o3/o4/gpt-5/gpt-6; self-hosted → reported spec else
  default; OpenCode route table; Groq (reject speech markers); else (OpenRouter, other custom)
  reasoning = kind in openai/openrouter, large limits.
- `spec_from_mapping` clamps context to [1024, 2,000,000] and output to [16, 131072]; protocol
  must be chat/responses/anthropic/google; unknown reasoning_format → "".
- Server spec cache: module dict keyed by base URL with a lock (`remember_server_specs`,
  `server_specs_known`); process lifetime, never invalidated except by re-discovery.
- `pi_compat` is passed through to the Pi meeting sidecar (a26); this area never reads it.

### local_text_servers.py (357 lines) - server introspection
- `server_root` strips trailing `/v1`. `discover_chat_models(base_url, server, api_key,
  timeout=5)`: `GET {base}/models` (raises if it fails), identify server if None, call the
  per-server describer, on describer failure fall back to name heuristic
  (`embed|rerank|whisper|(^|[-_/])tts([-_.]|$)`, case-insensitive), remember specs, return sorted
  chat ids.
- `_spec(context)`: context clamped [1024, 2M], max_output = min(4096, context // 2); `effort`
  present → reasoning_format "openai" and thinking_levels map.
- Per server: Ollama `POST /api/show` per model (4 parallel), needs "completion", `num_ctx` from
  Modelfile parameters else 4096, thinking → Off maps to "none"; vLLM `max_model_len`, Off→none;
  llama.cpp `meta.n_ctx` or `context_length`, Off→none; LM Studio v1 (`type` llm/vlm, loaded
  instance context else max, `capabilities.reasoning.allowed_options`, Off→None i.e. omitted,
  `trained_for_tool_use`), legacy v0 (`max_context_length`, "tool_use"); LocalAI
  `/v1/models/capabilities` ("chat", "tools", "thinking", `context_size`), unknown → by name.
- Identification order: owners in `/models` rows (`vllm`, `llamacpp`) → LocalAI well-known →
  Ollama → llama.cpp → vLLM → LM Studio. A failed probe on the first well-known route returns
  None (no server).
- `detect_running_servers`: loopback only, deduped, a thread per URL, 0.6 s.
- Uses httpx with `follow_redirects=False` and Bearer header only when a key is set.

### text_generation.py (345 lines) - protocol adapters
- `TextResult{text, tool_calls, assistant_message, usage, finish_reason}`, `ToolCall{id,
  function{name, arguments(JSON string)}}`, `TextGenerationError`.
- `generate(client, profile, *, model, messages, tools, cancel_event, session_id,
  reasoning_level, **options)`; cancel checked before and after the request (not mid-flight).
  OpenCode adds session headers. Hosted catalog profiles (groq, opencode_go, opencode_zen) drop
  caller `temperature`, `reasoning_effort`, `extra_body`. `level = spec.thinking_levels.get(level,
  level)`.
- Chat: strips keys starting with `_` from messages; `finish_reason` length/content_filter →
  error; refusal → error; copies `reasoning_content`, `reasoning`, `reasoning_details` into the
  assistant message for replay (text_generation.py:107-110). Options: openai format →
  `reasoning_effort` (drop temperature unless "none"); deepseek format → `extra_body.thinking`
  {type enabled}, plus `reasoning_effort` for high/max; hosted → default `max_tokens`.
- Responses: input items rebuilt from `_responses` opaque output; tools flattened `{type:function,
  strict:false, ...}`; `text.format json_object` for JSON mode; `include:
  ["reasoning.encrypted_content"]` for openai reasoning; status incomplete/failed/cancelled or
  error → error; refusal part → error.
- Anthropic: `POST /messages`, system joined with blank lines, same-role turns merged, tool
  results as user `tool_result`, stop_reason max_tokens/refusal/pause_turn → error.
- Google: `POST /models/{quoted model}:generateContent`, `systemInstruction`,
  `functionDeclarations` with `parametersJsonSchema`, `responseMimeType` for JSON, thought parts
  excluded, finishReason other than STOP/"" → error, call id fallback `call_{index}`.
- Usage normalized to `{prompt_tokens, completion_tokens, total_tokens}`.

### transcript_cleanup.py (475 lines) - cleanup client
- `CleanupInfo{provider, model, elapsed_s, level}` (level: light/medium/high, "custom",
  "profile", "") recorded in history.
- `TranscriptCleanup(provider, model, api_key, reasoning, cancel_event, *, defer_client)`;
  `configure(provider, model, reasoning)` rebuilds only when provider or
  `connection_fingerprint` (id, kind, url, key var, resolved key) changes; `attempt_timeout_s`;
  `is_available()`; `cleanup(text, system_prompt, timeout_s, deadline_s)`; `last_error`
  ("not run" initially, None after success).
- Module functions: `provider_env_key`, `find_api_key` (used by meeting engine/rerun),
  `list_cleanup_models`, `polish_cleanup_rule`; `CANCELED_REASON="canceled"`.
- Threading: `cleanup()` blocks its caller (a job worker thread); the HTTP call runs on a daemon
  thread and the caller polls a finished event every 50 ms, checking `cancel_event`
  (transcript_cleanup.py:52-91). Never closes the socket from the waiter (POSIX race).
- Timeouts: hosted 8 s per attempt (`TRANSCRIPT_CLEANUP_TIMEOUT_S`), self-hosted 120 s, agent
  60 s; retries 1 for dictation, 2 for batch (`timeout_s` given → `client.with_options`);
  give-up = attempt × (retries+1) + 1.0 s; `deadline_s` overrides.
- `_normalize_provider`: known profile id or built-in/agent id, else config default "openrouter".
- Before generating on a self-hosted profile it calls `learn_server_models` (first request of the
  session asks the server about its models).
- Each request gets a new `session_id` uuid4 (OpenCode Go header).

### thinking_levels.py (116 lines)
Pure logic: `ORDER`, `LABELS`, `agent_levels` (Codex reads `installed_agents.codex_model_efforts`
and `configured_default_model`), `endpoint_levels` (OpenAI: only if reasoning spec or no model
yet; OpenRouter always; others only if spec reasoning_format openai/deepseek; then apply
thinking_levels add/remove), `levels_for`, `nearest` (closest index, lower on tie; unknown level
treated as index 0), `effective_level`.

### cleanup_prompts.py (127 lines)
`CleanupLevel` (NONE "none", LIGHT, MEDIUM, HIGH, STORED, LABELS), `CUSTOM_LABEL="Custom"`,
`is_built_in` (whitespace-normalized compare against legacy default and three presets),
`resolve_level` (cleanup switch off → "none"), `custom_prompt`, `preset` (unknown → Medium),
`base_prompt`, `inline_lists_block(settings, snapshot)` (uses `synthetic_keys.is_terminal`),
`level_label`.

### cleanup_profiles.py (185 lines)
`CleanupProfile{id,name,instructions,hotkey,use_learned_rules}`, `STARTER_PROFILES`,
`load_cleanup_profiles` (stored None → starters; non-list → []; skips blank/duplicate ids),
`find_cleanup_profile`, `cleanup_may_run`, `normalize_hotkey` (`kp_`→`kp `, then
parse/format via a06 hotkey_manager), `profile_hotkey_conflict` (a06 hotkey_conflicts),
`validate_profile`, `save_cleanup_profile`, `delete_cleanup_profile`, `compose_profile_prompt`.
Note: validation does not cap profile count (transforms do).

### text_transforms.py (160 lines)
`Transform{id,name,instruction,hotkey}`, `STARTER_TRANSFORMS`, `load_transforms` (None →
starters; caps at 50; invalid hotkey → ""), `find_transform`, `transform_hotkey_conflict`,
`validate_transform`, `save_transform` (strips fields, normalizes hotkey first),
`delete_transform`.

### text_rewrite.py (263 lines)
Constants listed in A12-27; `REFUSALS` set (the four user-action messages, used by the UI to show
them as hints not errors). `compose_rewrite_prompt(instruction, has_selection,
dictionary_block, context_block)`: blocks joined by blank lines, guard last for a rewrite.
`rewrite_with(cleaner, text, instruction, …)` returns `(result, None)` or `("", message)`; never
returns the input on failure. `provider_ready`, `provider_readiness`, `configure_cleaner`,
`dictionary_block`, `rewrite_standalone` (own deferred client; call off the GUI thread).
Edge: a selection containing the sentinel is still rewritten (sentinel only checked without
selection, text_rewrite.py:160).

### dictionary.py (642 lines)
See A12-29..A12-35. Mutators take a settings dict and are run inside
`settings_manager.mutate_settings` (a10). Learning machinery: module-level `_sightings` dict +
lock (process lifetime, not persisted), `threading.Timer` named "dictionary-learning" (daemon),
`focus_context.get_service().reread(identity, callback)` runs on the capture service's thread and
its callback hands file writes to another timer (dictionary.py:599-622). `set_learned_callback`
is how the UI shows the "learned a word" toast. Never log terms.
Edge cases: `_parse` regenerates colliding ids with `_stable_id(id+term)` loop
(dictionary.py:125-126); learned terms drop `heard`; `new` only for learned; `protected` saved
only when true. `_tokens` strips `_EDGE` punctuation `.,!?;:"“”'‘’()[]{}<>«»…`; `_WORD` regex
`[^\W_]+(?:['’-][^\W_]+)*`. `_correction` uses `difflib.SequenceMatcher(autojunk=False)` on
casefolded tokens; inserts at either end are ignored; exactly one change, a 1-to-1 replace.

### vocabulary.py (79 lines)
`replace_terms(text, rules)`: fold with origin map (İ→I first, casefold, drop combining dot
U+0307 after "i"), regex `(?<!\w)(?:k1|k2…)(?!\w)` longest first, skip matches that start/end
inside a character that folded to several, single pass. Also used by Meeting corrections (a28).
Perf note: linear in text, no per-char Python calls (pinned by fold_speed test).

### snippets.py (468 lines)
See A12-36..A12-39. `normalize` = casefolded `\w+(?:['’.]\w+)*` words joined by spaces (’→').
Placeholder regex `\[\[\s*[Ss]\s*(\d{1,4})\s*\]\]`; text already containing one → no plan.
`_WORD_GAP` `[\s,\-‐‑‒–—]*`. Triggers grouped by first word, sorted by (-word count, -char
length, list order). `expand` returns (plain, html, ok); html only when any used snippet is
formatted; non-formatted parts HTML-escaped with `<br>` line breaks.

### recognition_context.py (115 lines)
`RecognitionContext{language, phrases}` (falsy when both empty), `SUPPORT_MODEL`/`SUPPORT_AFTER`,
`transcribe(incremental, backend, path, recognition)` passes context only to backends with
`supports_recognition is True`, `engine_support(engine)` (local_whisper/api → model, remote → "",
nemotron → depends on `WORD_BOOSTING`, else after; **"parakeet" → after today**),
`language_code` ("auto"/empty → "", `pt_BR` → "pt"), `hotwords`, `keywords`, `vocabulary_prompt`.

### speech_endpoints.py (376 lines) - undecided
`SpeechEndpoint` (connection profile, default_model, configurable, description, models_path,
known_models, auto_language; `is_openai`, `is_local`, `is_configured`, `self_hosted`,
`timeout_s`). `ENDPOINTS` table (A12-46). `configured_endpoint` (unknown id → OpenAI; invalid
saved → preset with warning), `selected_endpoint` (Custom without URL → OpenAI),
`endpoint_model`, `selected_model`, `valid_server_url`, `save_selection`, `save_model` ("Enter a
model id.", "OpenAI has no transcription model {m}."), `save_server` ("{name} can't be
edited."), `api_key`, `destination`, `audio_destination_sentence`, `credential_owners`,
`list_models` (404 → []; Speaches `task == automatic-speech-recognition`; LocalAI capabilities
contain "transcript").

### typesafe.py (773 lines)
- Data: `JudgeRoute{id,name,endpoint,model,credential_env,headers,timeout_s,profile}` with
  `is_local`, `host`, `key_label` ("{name} API key"), `key_phrase` ("a"/"an" + label).
  `TYPESAFE_ROUTE`, `OPENROUTER_ROUTE`, `ROUTES`, `ChoiceAnswer`, `JudgeUsage`, `TypeSafeError`.
- Wire protocol (both Jev routes): POST JSON `{"model", "state", "questions": {id: {"type":
  "noul"|"choice"|"score", "instructions", "criteria"?}}}`, headers Authorization Bearer,
  Content-Type/Accept JSON, User-Agent "OpenWhisper", plus route headers (cannot override auth).
  Response `{"answers": {id: {"type":"noul","noul":p} | {"type":"choice","choice":k,
  "confidence":c,"probabilities":{…}} | {"type":"score","score":s,"confidence":c}}, "usage":
  {"input_tokens": n}}`. `validate_answers` requires exactly the asked ids and matching types;
  noul in [0,1] and not bool; choice key in criteria; score in [0, len(criteria)-1].
- Judges: `_Primitives.noul/choice` convenience; `_RouteJudge` (budget only shortens, usage
  counters under lock, warn spacing 60 s); `TypeSafeJudge.ask` never raises; `same_connection`;
  `FollowingJudge(provider)`.
- Settings helpers: `text_model_route`, `route_from_settings`, `route_is_local`, `route_key`,
  `key_present`, `is_configured`, `judge_from_settings` (None when off or key missing).
- Verification: `VERIFY_HOST`, `VERIFY_TIMEOUT_S=10`, state `{"text":"ok"}`, question
  `Is \`text\` non-empty?`.
- Transport (typesafe.py:185-368): module-wide keep-alive pool; HTTP error bodies discarded; only
  ConnectionError/SSLEOF/Timeout on a *reused* connection triggers one fresh resend if ≥ 0.1 s
  budget remains; `will_close` responses are not pooled; proxy check per request
  (`urllib.request.getproxies` + `proxy_bypass`), proxied → urllib one-shot.
- Thread-safe: judges are shared across meeting worker threads.

### text_judge.py (390 lines)
`SYSTEM_PROMPT` (verbatim), `render(questions)` → (shown questions with numbered options, labels,
strict JSON schema), `_json_object` (strips `<think>…</think>`, takes outermost `{…}`),
`confidence`, `parse`, `ContextTooSmall` ("the model's context of {w:,} tokens is too small for
a judgment of about {n:,} tokens"), `_fit_context` (chars/3 + 32 + max_tokens), shared client
cache (≤ 4, LRU-ish by insertion, `max_retries=0`), `_NO_SCHEMA` set (endpoint+model that gave
400/422 mentioning response_format/json_schema → retried once without schema and remembered),
`TextModelJudge.ask/request/same_connection`, `verify_text_model`, `_failure_reason`. max_tokens
= 64 + 40 per question. `_MORE_CONTEXT` advice strings per server; suggested tokens rounded up to
a power of two.

### http_tls.py (44 lines)
Process-wide verified SSL context (system roots + certifi bundle), double-checked lock;
`new_verified_context` for callers that mutate. Used by typesafe, app_update, components, main.

### cleanup_agent_offer.py (64 lines)
`OFFERED=(claude_code, codex)`, `pending`, `offerable(scan)` (installed, no problem,
`signed_in is True`), `accept(agent_id)` (sets provider, model from `cleanup_model_memory`,
answered flag), `decline()`.

## Data and formats

Settings keys (JSON file `openwhisper_settings.json`, a10 owns the store and readers):

| Key | Type | Default | Read in |
|---|---|---|---|
| `text_llm_profiles` | list of `{id,name,base_url,api_key_env,protocol}` | [] | text_llm.py:_custom_payload |
| `ollama_base_url`, `lm_studio_base_url`, `vllm_base_url`, `llama_cpp_base_url`, `localai_base_url` | str | preset URL | text_llm.py:_configured_builtin |
| `<same ids>_api_key_env` | str | "" | same |
| `transcript_cleanup_enabled` | bool | false | cleanup_prompts, cleanup_profiles, cleanup_agent_offer |
| `transcript_cleanup_provider` | str (profile id or agent id) | "openrouter" | settings.resolve_transcript_cleanup_provider |
| `transcript_cleanup_model` | str | "openrouter/free" (OpenAI "gpt-4o-mini" read as "gpt-6-luna") | settings.resolve_transcript_cleanup_model |
| `transcript_cleanup_model_sort` | enum | "alphabetical" | settings / list_chat_models |
| `transcript_cleanup_reasoning` | enum off..ultra | "off" | transcript_cleanup, thinking_levels |
| `transcript_cleanup_level` | light/medium/high | "medium" | cleanup_prompts |
| `transcript_cleanup_prompt` | str | "" | cleanup_prompts.custom_prompt |
| `transcript_cleanup_rules` | list[str] ≤ 50 | [] | dictionary.spelling_rules, settings |
| `transcript_cleanup_profiles` | raw list or absent | absent (→ starters) | cleanup_profiles |
| `quick_record_profile` | str | "" | cleanup_profiles |
| `hotkeys.command_mode` | str | - | cleanup_profiles.cleanup_may_run |
| `text_transforms` | optional list `{id,name,instruction,hotkey}` | absent (→ starters) | text_transforms |
| `dictation_dictionary` | list `{id,term,starred,heard[],learned,new,protected?}` | [] | dictionary |
| `dictionary_learn_enabled` | bool | true | dictionary |
| `dictionary_steer_recognition` | bool | true | dictionary, a05 |
| `app_context_read_text` | bool | (a10) | dictionary._reread |
| `dictation_snippets` | list `{id,trigger,text,formatted}` | [] | snippets |
| `snippets_enabled` | bool | (a10) | a05 dictation_pipeline |
| `cleanup_model_memory` | dict provider→model | {} | cleanup_agent_offer |
| `cleanup_agent_offer_answered` | bool | false | cleanup_agent_offer |
| `typesafe_enabled` | bool | false | typesafe |
| `typesafe_provider` | typesafe/openrouter/text_model | "typesafe" | typesafe |
| `typesafe_text_model` | meeting/cleanup | "meeting" | typesafe.text_model_route |
| meeting LLM provider/model keys | via `resolve_meeting_llm_profile/model` | (a10) | typesafe.text_model_route |
| `api_transcription_endpoint` | str | "openai" | speech_endpoints |
| `api_transcription_servers` | dict id→`{base_url,api_key_env,model}` | absent | speech_endpoints |
| `api_transcription_model` | str | config.DEFAULT_API_MODEL | speech_endpoints (OpenAI only) |

Other persisted data:
- Meeting rows (a28 DB `openwhisper.db`): `agent_endpoint_json` (TextLLMSnapshot.to_dict JSON,
  with `model_metadata` = `{"model", ...TextModelSpec fields}`), `agent_provider`. Old rows lack
  both → OpenRouter.
- Credentials: keyring entries named by variable (`OPENAI_API_KEY`, `OPENROUTER_API_KEY`,
  `GROQ_API_KEY`, `OPENCODE_GO_API_KEY`, `OPENCODE_ZEN_API_KEY`, `TYPESAFE_API_KEY`, custom
  names), then environment, then `.env` (a10 credentials.py). Existing installs depend on these
  names.
- In-memory only: server spec cache, judge client cache, `_NO_SCHEMA`, dictionary sightings,
  keep-alive pool.

Wire protocols used (all JSON over HTTPS/HTTP):
- OpenAI-compatible: `GET /models`, `POST /chat/completions`, `POST /responses`; OpenRouter
  `GET /auth/key`, `GET /models?sort=`; Anthropic `POST /messages`; Google
  `POST /models/{m}:generateContent`.
- Local servers: Ollama `POST /api/show`, `GET /`; vLLM `GET /version`; llama.cpp `GET /props`;
  LM Studio `GET /api/v1/models`, `/api/v0/models`; LocalAI `GET /v1/models/capabilities`,
  `/.well-known/localai.json`.
- TypeSafe System One `POST /v1/systemone` (shape above).
- Speech: `GET {base}/models` or `/models/capabilities`; uploads themselves live in a14.

## External dependencies

| Python | Used for | C++ / Qt 6 equivalent |
|---|---|---|
| `openai` SDK | clients, retries (default 2, ~0.5 s backoff, 408/409/429/5xx + connection errors, Retry-After), timeouts, `with_options`, error classes by status | Own thin client over `QNetworkAccessManager` (or libcurl/cpr in a Qt-free core): JSON via `QJsonDocument` or nlohmann/json; reimplement retry policy and status → error mapping |
| `httpx` | local server probes, speech model lists, OpenRouter `/auth/key` | same client |
| `http.client`, `urllib`, `select`, `ssl` | TypeSafe keep-alive pool and proxy fallback | `QNetworkAccessManager` already pools keep-alive connections per host (6) and honors system proxy (`QNetworkProxyFactory::setUseSystemConfiguration`); keep the "reused connection silent → resend once" behavior via timeout + one retry |
| `certifi`, `ssl` | verified TLS with bundled roots | Qt uses OS store; ship a CA bundle only if needed on Linux (`QSslConfiguration::addCaCertificates`) |
| `threading` (Thread, Event, Lock, Timer) | deadline wait, learning timers, caches | `std::thread`/`QThreadPool`, `std::condition_variable`, `std::mutex`, `QTimer::singleShot` on a worker |
| `concurrent.futures.ThreadPoolExecutor` | parallel probes | `QtConcurrent::mapped` or N async network replies |
| `difflib.SequenceMatcher` | correction alignment, trigger similarity | Port the algorithm exactly (Ratcliff/Obershelp with autojunk off, `get_opcodes`, `ratio`); small, ~200 lines |
| `re` (Unicode `\w`, casefold) | dictionary, snippets, Markdown, vocabulary | `QRegularExpression` (PCRE2, `UseUnicodePropertiesOption`) in the Qt layer, or ICU/PCRE2 in core; full Unicode casefold needs ICU (`u_strFoldCase`) or `QString::toCaseFolded` (simple folding only: ß stays ß - mismatch, see risks) |
| `hashlib.sha1` | stable dictionary ids | `QCryptographicHash::Sha1` or a small SHA-1 in core |
| `uuid`, `secrets` | ids | `QUuid::createUuid().toString(QUuid::Id128)`, `QRandomGenerator::system()` |
| `html.escape` | snippet HTML | `QString::toHtmlEscaped` (escapes `"` too; Python used quote=False except href; match exactly) |
| `json` | everything | `QJsonDocument` / nlohmann::json (preserve key order where prompts embed JSON: text_judge content uses `ensure_ascii=False`) |

## Platform-specific code

- None directly in this area. Indirect: `inline_lists_block` relies on `synthetic_keys.is_terminal`
  (a06, per-OS terminal detection incl. Linux terminals); learning relies on
  `focus_context` re-read (a06: UI Automation on Windows, AX on macOS, AT-SPI on Linux; Wayland
  likely no caret text, so learning silently does nothing). TLS roots: frozen Python on Linux
  needed certifi; Qt on Linux uses OpenSSL default paths.

## Engine-specific code

- **Drop**: `recognition_context.hotwords` and `WHISPER_HOTWORDS_MAX_CHARS` (faster-whisper);
  `engine_support` branches for `local_whisper`; `_openai_class` lazy import trick; the
  "isolated worker" JSON-option constraint mentioned in tests (s4_recognition) is Python worker
  plumbing.
- **Undecided**: `speech_endpoints.py` whole (cloud/self-hosted Whisper-style API transcription);
  `recognition_context.keywords`/`vocabulary_prompt` (OpenAI transcription);
  `engine_support("nemotron")` (Nemotron Streaming on the same NeMo runtime, word boosting);
  `engine_support("remote")` (remote-computer engine).
- **Adapt to in-process Parakeet**: `RecognitionContext` should become the per-call option passed
  to `Recognizer::transcribe` (language + phrases). NeMo-Speech.cpp's C ABI already has
  `nemo_speech_asr_speech_context{phrases, phrase_count, boost}` (asr.h:160-178) and `max_boost`;
  whether Parakeet TDT honors it decides `engine_support("parakeet")` = model vs after (today it
  is "after": phrases only drive replacements and the cleanup prompt). Phrase strings must
  outlive the request (CLAUDE.md engine rule). `transcribe()` with the incremental slot maps to
  FioriVoce's live-preview window reuse.

## Dependencies on other areas

Calls out:
- a10 settings/credentials: `SettingsKey.*`, `settings_manager.load_all_settings/update_settings/
  mutate_settings/save_setting`, resolvers (`resolve_transcript_cleanup_*`,
  `resolve_meeting_llm_profile/model`, `resolve_typesafe_*`, `resolve_dictionary_*`,
  `resolve_app_context_read_text`, `compose_transcript_cleanup_prompt`,
  `default_transcript_cleanup_model`, `api_model_choices`, `resolve_api_transcription_model`),
  `credentials.resolve_credential`. Note the cycle: settings.py imports text_llm and
  cleanup_prompts lazily.
- a06: `hotkey_manager.parse_hotkey/format_hotkey`, `hotkey_conflicts.hotkey_conflict`
  (PROFILE/TRANSFORM), `synthetic_keys.is_terminal`, `focus_context.get_service().reread`, job
  `snapshot(timeout)` → `FocusSnapshot{identity{is_self}, text{before, selected, after,
  caret_known}}`.
- a20: `installed_agents.resolve_agent`, `known_agent`, `codex_model_efforts`,
  `configured_default_model`, `InstalledAgent{usable, problem, signed_in}`.
- a26: `meeting.agent.installed.core.run_agent_task(...)` → outcome `{ok, canceled, text, error}`.
- `config.py` (a01) constants listed above.

Called by (contracts):
- a05 `dictation_pipeline`: `recognition_phrases`, `apply_replacements`, `plan_expansion`,
  `prompt_block`, `prompt_guard`, `expand`, `base_prompt/resolve_level/custom_prompt/
  inline_lists_block`, `compose_profile_prompt`, `schedule_learning(job, pasted, before_cleanup)`;
  `finish_text` fallback rule (dictation_pipeline.py:408-425).
- a04 runtime transcription/command: `TranscriptCleanup`, `CleanupInfo`, `find_transform`,
  `find_cleanup_profile`, text_rewrite.
- a19 actions: `typesafe.judge_from_settings/route_from_settings` (voice routing),
  `find_transform`, `find_cleanup_profile`.
- a24-a29 meeting: `FollowingJudge`, `judge_from_settings`, `route_is_local`,
  `typesafe_signals` (`ChoiceAnswer`, `Judge`), `vocabulary.replace_terms`
  (meeting/corrections.py), `generate`, `model_spec`, `create_openai_client`,
  `profile_from_agent_config`, `snapshot_from_meeting`, `find_api_key`, `SIDECAR_API_KEY_ENV`.
- a14 transcriber backends: `recognition_context` budgets, `SpeechEndpoint`.
- UI (a11, a13, a15, a31, a08, a03): settings pages for Models, API keys, Dictionary, Snippets,
  Commands, Fast judgments, Cleanup rules, text endpoint dialog, speech server dialog, Scratchpad,
  Quick Record, cleanup agent offer dialog.
- a16/a01: `http_tls.verified_context` (updater, components, main).

## Tests

All under `D:\coding\whisper_local\tests\`. Pure-logic files marked **(golden)** can be replayed:
generate inputs/outputs from the Python functions and assert the C++ matches.

| File | Pins |
|---|---|
| test_text_llm.py | URL/name/env validation, presets and overrides, custom upsert/remove/protocol, dummy key, OpenAI filter, OpenRouter sort, snapshots and meeting fallback, consent copy **(golden for validators, filter, snapshot parse)** |
| test_text_generation.py | wire shapes per protocol, tool round-trip with signed thinking, truncation → raw, cancel, OpenCode routing, OpenAI reasoning routes, Luna Off mapping, capability table reachability **(golden: request bodies built by `generate` against a fake client)** |
| test_text_judge.py | noul/choice parsing, confidence formula, malformed replies, think/fence stripping, score refused, size bound, schema fallback, client sharing, route selection, gate matrix with meeting features **(golden: render/parse/confidence)** |
| test_typesafe_service.py | validation of each primitive, usage, failures→None, size bound, warning rate limit, settings gating, verify messages per status, OpenRouter route/model/header, FollowingJudge **(golden: validate_answers, _status_outcome)** |
| test_network_reuse.py | keep-alive reuse, no socket sharing, error keeps status only, stale/idle connection replacement, one resend, budget never exceeded, proxy path, dictation worst case two timeouts, batch retries |
| test_transcript_cleanup.py | fallback to raw, last_error states, per-request timeout, reasoning options, configure rebuild rules, self-hosted 120 s, rule polish, settings resolvers, legacy gpt-4o-mini → luna |
| test_cleanup_deadline.py | trickling body ends at budget, deadline stops second attempt, cancel immediate, cancel before call sends nothing |
| test_local_text_servers.py | per-server describers, identification, detection loopback only, learn once, judgment context check, snapshot records reported spec **(golden: describers fed canned JSON)** |
| test_speech_endpoints.py | presets, fallbacks to OpenAI, URL rules, timeouts, credential owners, privacy sentence, model listing (undecided area) |
| test_thinking_levels.py | agent/endpoint levels, nearest **(golden)** |
| test_personalize_s2_prompts.py | preset composition, level labels, built-in detection, custom prompt, inline lists block, order of blocks **(golden: prompt strings)** |
| test_cleanup_profiles.py | starters, round trip, validation, hotkey conflicts, prompt; also editor UI (a13) |
| test_personalize_s3_transforms_storage.py | starters, malformed rows, cap, conflicts **(golden)** |
| test_personalize_s3_text_rewrite.py | prompts, sentinel, refusals, timeouts, error shortening, readiness **(golden: compose_rewrite_prompt, _describe_error)** |
| test_personalize_s4_dictionary.py | parse tolerance, dedupe, ids, caps, ranking, replacements, prompt block, mutators, summary, parse_heard, spelling rules **(golden)** |
| test_personalize_s4_learning.py | correction detection, sightings, gating, scheduling non-blocking **(golden: learned_correction)** |
| test_dictionary_cleanup_revert.py | reverted_cleanup, protected words **(golden)** |
| test_personalize_fix2_g4_fold_speed.py | Unicode fold equivalence (İ, ß), random text, performance **(golden: replace_terms on random Unicode)** |
| test_personalize_s5_snippets.py | normalize, load, validation, trigger notes, planning, placeholders, expansion, Markdown **(golden: all pure)** |
| test_personalize_s4_recognition.py | RecognitionContext plumbing, language_code, hotwords/keywords/prompt budgets **(golden for budget functions)**; Nemotron/worker parts are other areas |
| test_cleanup_agent_offer.py | pending, offerable order, accept/decline, dialog |
| test_http_tls.py | shared context built once, thread-safe |
| test_openai_lazy_import.py | startup never imports SDK (behavior to keep: no network/SDK init at startup) |
| test_personalize_s1_styles.py | style blocks interplay with cleanup prompt (a01 app_styles) |

Many more tests import these modules for integration (personalize_foundation_*, s3/s5/s6
runtime/pipeline, meeting_*, automation_*); they belong to the calling areas.

## Porting notes

Suggested decomposition:
- **Qt-free core** (`src/core/text/`, no network): profiles + validation + snapshots,
  model catalog tables, thinking levels, cleanup prompt composition, profiles/transforms
  storage, rewrite prompt + error mapping, dictionary (parse/mutate/replace/prompt/spelling
  rules/correction alignment), vocabulary fold-replace, snippets (plan/expand/Markdown),
  recognition context budgets, TypeSafe answer validation, text-judge render/parse/confidence,
  local-server JSON describers (input = parsed JSON). All take/return plain structs + a JSON
  value type; settings access through an interface implemented by a10.
- **Network layer** (Qt Network, worker thread): OpenAI-compatible client with retry policy and
  wall-clock deadline + cancel, protocol adapters (chat/responses/anthropic/google), model
  listing, key verification, local server probes, TypeSafe transport, speech model listing.
- **Services** (Qt): `TranscriptCleanup`, judges, `FollowingJudge`, learning scheduler, caches.

Order: (1) vocabulary + dictionary + snippets + cleanup prompts/profiles/transforms (pure,
needed by the dictation pipeline, golden-vector friendly); (2) profiles/catalog/thinking levels;
(3) HTTP client + `generate` chat protocol + `TranscriptCleanup` with deadline/cancel; (4) rewrite;
(5) local server discovery; (6) Responses/Anthropic/Google adapters (needed for OpenAI reasoning
models and OpenCode); (7) TypeSafe + text judge (Meeting Mode); (8) agent cleanup (after a20/a26);
(9) speech endpoints if the API backend is kept.

Rough C++ size: core ~3,500 lines (catalog tables ~700 as data), network + adapters ~2,000,
services ~1,000, SequenceMatcher + casefold helpers ~400. Total about 6,500-7,500 lines plus
tests.

Risks:
- Unicode semantics: Python `str.casefold()` (full folding), `\w` (Unicode letters/digits/
  underscore), `str.split()` (Unicode whitespace), `str.isalpha` must match or replacements,
  triggers and learning diverge. `QString::toCaseFolded` is simple folding (ß ≠ ss); use ICU full
  folding plus the origin map. Python indexes code points; Qt uses UTF-16 - offsets must be
  handled in code points or carefully in UTF-16.
- `difflib.SequenceMatcher` exactness (opcodes and ratio) for learning and trigger notes.
- Reproducing the openai SDK's retry/backoff and the "trickling body" deadline; Qt's transfer
  timeout is also per-inactivity, so the wall-clock deadline + abandon-the-reply design is still
  needed (abort the reply instead of leaving a thread).
- The catalogs are dated and will drift; keep them as data (JSON resource) so updates do not need
  code changes.
- Branding strings ("OpenWhisper" in headers, snippet cap message, `OPENWHISPER_LLM_API_KEY`)
  must be decided; keyring entry names must stay compatible if FioriVoce should read existing keys.
- Many cross-area contracts (settings resolvers, focus snapshot, installed agents) must exist
  before the services compile; stub interfaces early.

Native gains: no 0.7 s SDK import or 154 ms TLS context build; QNetworkAccessManager pools
connections for every endpoint (not only TypeSafe), cutting cleanup latency; dictionary/snippet
processing becomes trivially fast; cancel can abort the HTTP reply rather than leaving a daemon
thread running; local Parakeet speech contexts may let dictionary terms steer recognition
in-process.
