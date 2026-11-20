#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "dictation/job.h"
#include "focus/capture_service.h"
#include "focus/join.h"
#include "focus/snapshot.h"

// The text stages of one dictation (services/dictation_pipeline.py 268-597). Every function here
// is thread-safe and never throws: a failing feature costs only its own step.
namespace fv::dictation {

// services/snippets.py Snippet: a trigger phrase and the text it types.
// Affinity: value type.
struct Snippet {
    std::string id;
    std::string trigger;
    std::string text;
    // Render light Markdown to HTML and paste it as rich text.
    bool formatted = false;

    bool operator==(const Snippet&) const = default;
};

// services/snippets.py SnippetPlan: the text to clean, with placeholders standing in for
// triggers. A plan with neither `whole` nor placeholders expands to its own text.
// Affinity: value type.
struct SnippetPlan {
    std::string text;
    // Set when the whole utterance was one trigger.
    std::optional<Snippet> whole;
    // (placeholder token, snippet), in the order the plan made them.
    std::vector<std::pair<std::string, Snippet>> placeholders;

    bool operator==(const SnippetPlan&) const = default;
};

// Affinity: value type.
struct PreparedText {
    // The speech engine's transcript.
    std::string original;
    // After dictionary replacements, before snippets.
    std::string text;
    // What AI cleanup is given: `text` with snippet placeholders.
    std::string cleanup_input;
    SnippetPlan plan;
    // The whole utterance was a snippet trigger, so no AI runs.
    bool skip_cleanup = false;
};

// Affinity: value type.
struct FinishedText {
    std::string text;
    // Rich-text alternative to paste with `text`, or "".
    std::string html;
    // False when cleanup lost a snippet placeholder and `text` is the uncleaned text instead.
    bool ok = true;
    // The text before AI cleanup, with snippets expanded.
    std::string raw_text;

    bool operator==(const FinishedText&) const = default;
};

// snippets.expand's (plain, html, ok).
struct Expansion {
    std::string plain;
    std::string html;
    bool ok = true;
};

// The dictionary and snippet features behind prepare_text, finish_text and after_paste. This
// base class is the pass-through every stage falls back to: no replacements, no snippets, no
// learning. The dictionary and snippets (M4) override it. A method may throw; the stage that
// calls it logs a warning and goes on as if the feature were off.
// Affinity: thread-safe (const methods, called from the UI thread and workers).
class TextPreparer {
public:
    virtual ~TextPreparer() = default;
    // dictionary.apply_replacements(text, load_dictionary(settings)).
    virtual std::string apply_replacements(std::string_view text, const Json& settings) const;
    // snippets.plan_expansion(text, load_snippets(settings)), for live jobs with snippets on.
    virtual SnippetPlan plan_expansion(std::string_view text, const Json& settings) const;
    // snippets.expand: a plan without snippets is its own text. Only an overriding preparer
    // makes plans with snippets; this one throws for them.
    virtual Expansion expand(std::string_view text, const SnippetPlan& plan) const;
    // dictionary.schedule_learning, once a dictation is pasted.
    virtual void schedule_learning(const DictationJob& job, std::string_view pasted_text,
                                   std::optional<std::string_view> before_cleanup) const;
};

// The pass-through preparer.
const TextPreparer& pass_through_preparer();

// Applies saved spelling replacements locally. A broken dictionary leaves the words as heard.
std::string apply_dictionary(std::string_view raw, const Json& settings,
                             const TextPreparer& preparer);

// Applies the dictionary when requested and plans live dictation's snippets (dictation,
// action and walkthrough jobs, with snippets on).
PreparedText prepare_text(std::string_view raw, const DictationJob* job, const Json& settings,
                          const TextPreparer& preparer = pass_through_preparer(),
                          bool replace_dictionary_terms = true);

// Expands snippets in the cleaned text, or falls back to the uncleaned text. `cleaned` is
// cleanup's output, or prepared.cleanup_input when no cleanup ran.
FinishedText finish_text(std::string_view cleaned, const PreparedText& prepared,
                         const TextPreparer& preparer = pass_through_preparer());

// The joins text_for_paste and html_for_paste use; a replacement (tests) may throw.
using TextJoin =
    std::function<std::string(std::string_view text, const std::optional<focus::TextContext>&)>;
using HtmlJoin = std::function<std::string(std::string_view html, std::string_view text,
                                           const std::optional<focus::TextContext>&)>;

// `text` joined onto what is already before the caret, for a live dictation whose capture knew
// the caret. UI thread: never waits for the capture.
std::string text_for_paste(std::string_view text, const DictationJob* job,
                           const TextJoin& join = focus::join_with_context);

// `html`, the rich-text twin of `text`, given text_for_paste's join. `text` is the transcript
// before its join. Rich-text apps paste the HTML, so both flavours must agree. UI thread.
std::string html_for_paste(std::string_view html, std::string_view text, const DictationJob* job,
                           const HtmlJoin& join = focus::join_html_with_context);

enum class PasteTarget {
    Same,
    Changed,
    // Either side could not be told: nothing proves the app is the same.
    Unknown,
};
// "same", "changed", "unknown".
std::string_view to_string(PasteTarget target);

// How long the paste check waits where the focused app is only known asynchronously (Linux:
// hyprctl or an X11 round trip); it runs on the UI thread.
inline constexpr std::chrono::milliseconds PASTE_CHECK_TIMEOUT{500};

// Whether the app that had focus at the job's start (its target capture, else its focus
// capture) still has it. UI thread: never waits for the start's capture, and waits at most
// PASTE_CHECK_TIMEOUT for the current app. Uses `service` (focus::get_service() when null).
PasteTarget paste_target(const DictationJob* job, focus::CaptureService* service = nullptr);

// Whether a command or transform may paste: its app provably still has focus. A rewrite is
// pasted even with auto-paste off, so an unknown app counts as changed rather than pasting it
// blind.
bool paste_target_ok(const DictationJob* job, focus::CaptureService* service = nullptr);

// cleanup_prompts.custom_prompt: the user's saved cleanup prompt, or "" when none is saved or
// it is a copy of a built-in one. The cleanup prompts (M4) provide it.
using CustomPrompt = std::function<std::string(const Json& settings)>;

// What CleanupInfo.level and History record for a cleanup that ran: "profile" for a cleanup
// profile, "custom" for a saved custom prompt, else the preset ("light", "medium", "high"), or
// "" when cleanup is off or the level is unknown.
std::string cleanup_level(const Json& settings, bool has_profile, const CustomPrompt& custom);

// Follow-up work once a dictation is pasted (dictionary learning); UI thread, never blocks.
// `before_cleanup` is the transcript before AI cleanup, when it changed it.
void after_paste(const DictationJob* job, std::string_view pasted_text,
                 std::optional<std::string_view> before_cleanup = std::nullopt,
                 const TextPreparer& preparer = pass_through_preparer());

// app_styles.category_for: the category of the app in `snapshot` ("email", "work", "personal",
// "other"), the user's overrides applied, styles on or off; "" when unknown or our own window.
std::string category_for(const std::optional<focus::FocusSnapshot>& snapshot, const Json& settings);

// The context columns of a History entry, with unknown ones left out, in this order:
// entry_kind, app_id, app_name, app_category, cleanup_level, language. `job` is the delivered
// job, or null (uploads); `info_level` the level of a cleanup that ran, nullopt when none did;
// `live` that the transcript was dictated rather than read from a file (entry_kind "dictation"
// or "file" when there is no command or transform job).
Json history_fields(const DictationJob* job, std::optional<std::string_view> info_level, bool live,
                    const Json& settings);

// services/dictation_stats.py count_words and words_edited, which History's stats (M1.13)
// provide.
struct WordCounter {
    std::function<std::size_t(std::string_view)> count_words;
    std::function<std::size_t(std::string_view before, std::string_view after)> words_edited;
};

// What Stats count from a prepared transcript besides its words, zero counts left out:
// "snippets" (snippets used), "snippet_words" (words they added beyond their triggers),
// "corrections" (words the dictionary changed). Never throws: {} on failure.
Json stats_counts(const PreparedText& prepared, const WordCounter& words);

namespace detail {
void log_record_stats_failure();
} // namespace detail

// Adds the saved entry to the local stats through `record` (dictation_stats.record, M1.13);
// history-save worker, never throws.
template <class Row, class Record>
void record_stats(const Json& entry_fields, const Row& row, Record&& record) noexcept {
    try {
        std::forward<Record>(record)(entry_fields, row);
    } catch (...) {
        detail::log_record_stats_failure();
    }
}

} // namespace fv::dictation
