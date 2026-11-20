#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/cancel.h"
#include "base/json.h"
#include "http/client.h"
#include "llm/credentials.h"
#include "llm/transcript_cleanup.h"

// services/text_rewrite.py: rewriting text by an instruction through the user's AI cleanup
// model, for Command Mode, transforms and the Scratchpad. The selected text is data: the prompt
// says so and never asks the model to follow it. Nothing here logs text, selections or
// instructions.
namespace fv::llm {

// What the model answers, in place of text, when asked to change text it was never given.
inline constexpr std::string_view kNeedsSelection = "<<NEEDS_SELECTION>>";
inline constexpr std::string_view kNeedsSelectionMessage = "Select the text to change first";
inline constexpr std::string_view kNoProviderMessage = "Set up AI cleanup to rewrite text";
inline constexpr std::string_view kCanceledMessage = "Canceled";
// How long a rewrite waits for the model, retries included (seconds): longer than a dictation's
// cleanup budget, since a rewrite writes more and the user is watching for it. A model still
// working at the limit is not asked again, which would only double the wait.
inline constexpr double kRewriteTimeoutS = 30.0;
// Text this long (code points) gets kLongTextTimeoutS.
inline constexpr std::size_t kLongTextChars = 2000;
inline constexpr double kLongTextTimeoutS = 60.0;
// Sized to fit the pointer notice in full, after its "Error: " prefix.
inline constexpr std::string_view kTimedOutMessage =
    "The AI model took too long. Try a faster one in AI cleanup";
// A Select All in a long document is refused rather than sent: it would outgrow small local
// models' context and hold the job slot for minutes (code points).
inline constexpr std::size_t kMaxTextChars = 20000;
inline constexpr std::string_view kNoInstructionMessage = "Didn't catch an instruction";
inline constexpr std::string_view kTooLongMessage =
    "Select less text to rewrite (up to 20,000 characters)";
inline constexpr std::string_view kEmptyMessage = "The AI model sent back nothing";
// Messages the user acts on, as opposed to the model or provider failing.
inline constexpr std::array<std::string_view, 4> kRefusals{
    kNoInstructionMessage, kNeedsSelectionMessage, kTooLongMessage, kNoProviderMessage};
bool is_refusal(std::string_view message);

// The system prompt for a rewrite (`has_selection`: the user message is the selected text and
// the instruction is here) or for writing new text (the instruction is the user message).
// `dictionary_block` and `context_block` ("" to leave out) follow; a rewrite ends with the guard.
std::string compose_rewrite_prompt(std::string_view instruction, bool has_selection,
                                   std::string_view dictionary_block = {},
                                   std::string_view context_block = {});

// _describe_error: the short reason shown for a cleaner's last_error: "Canceled", the
// no-provider and timeout messages, "The AI model sent back nothing", else "The AI model failed:
// <first line, at most 120 code points with "…">".
std::string describe_error(std::string_view error);

// The wall-clock limit a rewrite gives `message` (seconds): 30, 60 over 2,000 code points, never
// below `attempt_timeout_s`.
double rewrite_budget_s(std::string_view message, double attempt_timeout_s);

// Rewrites `text` by `instruction`, or writes new text when `text` is blank. Returns (result,
// nullopt), or ("", reason) with a short user-facing reason; never the input. A blank
// instruction or a selection over 20,000 code points sends nothing.
std::pair<std::string, std::optional<std::string>>
rewrite_with(CleanupClient& cleaner, std::string_view text, std::string_view instruction,
             std::string_view dictionary_block = {}, std::string_view context_block = {},
             const CancelToken& cancel = {});

// Whether a cleanup model and its credential are set up, without connecting. An installed agent
// is available only for authorized dictation. Blocking (credential store); never on the GUI thread.
bool provider_ready(const Json& settings, KeySources& keys);
// provider_ready without waiting on an installed agent's lookup: nullopt while one runs. Never
// nullopt in this build, which has no agent cleanup.
std::optional<bool> provider_readiness(const Json& settings, KeySources& keys);

// Points `cleaner` at the user's cleanup provider, model and reasoning level.
void configure_cleaner(TranscriptCleanup& cleaner, const Json& settings);

// The personal dictionary's prompt block for a rewrite, or "" (also when reading it fails).
std::string dictionary_block(const Json& settings);

// rewrite_with on a client of its own, for callers outside the job slot; the dictation's client
// is never touched. "Set up AI cleanup to rewrite text" when no provider is ready. Blocking; call
// it from a worker thread.
std::pair<std::string, std::optional<std::string>>
rewrite_standalone(std::string_view text, std::string_view instruction, const Json& settings,
                   std::shared_ptr<http::Client> http, KeySources keys,
                   const CancelToken& cancel = {}, CleanupOptions options = {});

} // namespace fv::llm
