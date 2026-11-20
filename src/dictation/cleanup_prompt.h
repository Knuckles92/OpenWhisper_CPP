#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "focus/snapshot.h"
#include "text/app_styles.h"
#include "text/cleanup_profiles.h"

// The system prompt of one AI cleanup call (services/dictation_pipeline.py _block,
// _base_cleanup_prompt, _app_style and compose_cleanup_prompt). The prompt never names the app
// the words go to unless the user lets the text near the cursor be read.
namespace fv::dictation {

// How long cleanup waits for the job's focus capture (config.CONTEXT_CAPTURE_DEADLINE_S). The
// capture resolved long before cleanup starts; the wait only bounds a service that broke its
// promise.
inline constexpr std::chrono::milliseconds CONTEXT_CAPTURE_DEADLINE{400};

// The parts compose_cleanup_prompt builds its blocks from; an empty function is the real one.
// Any of them may throw: its block is then left out. Tests replace them as OpenWhisper's tests
// monkeypatch the modules.
struct CleanupPromptBlocks {
    // app_styles.style_for.
    std::function<std::optional<text::app_styles::AppStyle>(
        const std::optional<focus::FocusSnapshot>&, const Json& settings)>
        style_for;
    // app_styles.prompt_block.
    std::function<std::string(const std::optional<text::app_styles::AppStyle>&)> style_block;
    // cleanup_prompts.inline_lists_block.
    std::function<std::string(const Json& settings, const std::optional<focus::FocusSnapshot>&)>
        lists_block;
    // dictionary.prompt_block(dictionary.load_dictionary(settings)).
    std::function<std::string(const Json& settings)> dictionary_block;
    // snippets.prompt_guard.
    std::function<std::string(const SnippetPlan&)> snippet_guard;
    // focus_context.prompt_block.
    std::function<std::string(const std::optional<focus::FocusSnapshot>&)> context_block;
};

// The prompt in this order, each block after one blank line and left out when empty: the base
// prompt (`profile`'s, or the level preset or custom prompt with the learned `rules`), the app's
// style (no profile, styles on), the inline-lists note (a dictation without a profile, unless the
// style already keeps a terminal on one line), the dictionary, the snippet guard, the text near
// the cursor (reading text on), and last `batch_context` with its injection guard.
// Blocking up to CONTEXT_CAPTURE_DEADLINE for the job's capture: a worker, never the UI thread.
// Never throws for a failing block.
std::string compose_cleanup_prompt(const Json& settings, const DictationJob* job,
                                   const text::CleanupProfile* profile,
                                   const std::vector<std::string>& rules,
                                   const PreparedText& prepared,
                                   std::optional<std::string_view> batch_context = std::nullopt,
                                   const CleanupPromptBlocks& blocks = {});

} // namespace fv::dictation
