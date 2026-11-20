#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"

// Snippets (services/snippets.py): a spoken trigger phrase becomes exact text, without AI. A
// whole-utterance trigger skips cleanup. Triggers inside a sentence become protected
// placeholders before cleanup and are expanded after it; if cleanup loses a placeholder, the
// uncleaned text with expansions is used instead. Snippet text and triggers are the user's own
// content: never log them. Every function is pure over its arguments and thread-safe; the
// mutators edit the settings mapping they are given, for SettingsStore::mutate.
namespace fv::text {

// config.MAX_SNIPPETS.
inline constexpr std::size_t MAX_SNIPPETS = 200;
// Trigger length: the normalized words at least MIN, the raw trigger at most MAX, in code
// points; snippet text at most MAX_TEXT_CHARS code points.
inline constexpr std::size_t MIN_TRIGGER_CHARS = 2;
inline constexpr std::size_t MAX_TRIGGER_CHARS = 60;
inline constexpr std::size_t MAX_TEXT_CHARS = 10'000;

struct Snippet {
    std::string id;
    std::string trigger;
    std::string text;
    // Render light Markdown to HTML and paste it as rich text.
    bool formatted = false;

    friend bool operator==(const Snippet&, const Snippet&) = default;
};

struct SnippetPlan {
    // The text to clean: placeholders stand in for triggers.
    std::string text;
    // Set when the whole utterance was one trigger.
    std::optional<Snippet> whole;
    // (placeholder token, snippet), one per occurrence, in text order.
    std::vector<std::pair<std::string, Snippet>> placeholders;

    friend bool operator==(const SnippetPlan&, const SnippetPlan&) = default;
};

struct Expansion {
    std::string plain;
    // "" unless a formatted snippet was used.
    std::string html;
    // false when a placeholder is missing from the cleaned text, appears twice, or the text
    // holds one the plan never made.
    bool ok = true;

    friend bool operator==(const Expansion&, const Expansion&) = default;
};

// Snippet.to_dict: {id, trigger, text, formatted}.
Json snippet_to_json(const Snippet& snippet);

// `text` as triggers are compared: casefolded words joined by spaces, punctuation dropped. A
// word keeps apostrophes and dots between its letters ("don't", "acme.com", "’" read as "'");
// anything else, hyphens included, separates words, so "sign-off" and "sign off" say the same
// trigger.
std::string normalize(std::string_view text);

// The valid saved snippets, in their saved order: invalid rows, repeated ids and triggers said
// the same way as an earlier one are dropped, and the list is capped at MAX_SNIPPETS.
std::vector<Snippet> load_snippets(const Json& settings);

// uuid4().hex.
std::string new_snippet_id();

// The snippet whose trigger is said the same way as `trigger`, if any.
std::optional<Snippet> find_duplicate(std::string_view trigger,
                                      const std::vector<Snippet>& snippets,
                                      std::string_view exclude_id = {});

// A short heads-up about how `trigger` sits with the other triggers, or "". In order: a
// duplicate (which saving refuses), a trigger inside a longer one or containing a shorter one,
// a near-identical spelling speech could confuse, and a single everyday word.
std::string trigger_note(std::string_view trigger, const std::vector<Snippet>& snippets,
                         std::string_view exclude_id = {});

// Throws py::ValueError with a sentence for the user when `snippet` cannot be saved.
void validate_snippet(const Snippet& snippet, const Json& settings);
// Adds or replaces (by id) `snippet`, trigger stripped; throws as validate_snippet.
void save_snippet(Json& settings, const Snippet& snippet);
// Removes one snippet. The saved list is rewritten from load_snippets, so an unknown id still
// drops the rows that do not load.
void delete_snippet(Json& settings, std::string_view snippet_id);

// Finds triggers in `text` and protects them for cleanup. The whole utterance matching a trigger
// (ignoring case and punctuation) makes a `whole` plan. Otherwise every whole-word trigger
// occurrence, longest trigger first and never overlapping, becomes a numbered placeholder
// ("[[S1]]"), one per occurrence. Text that already reads as a placeholder gets no plan.
SnippetPlan plan_expansion(std::string_view text, const std::vector<Snippet>& snippets);

// The cleanup-prompt instruction to keep `plan`'s placeholders, or "".
std::string prompt_guard(const SnippetPlan& plan);

// `text` (the cleaned plan text) with `plan`'s placeholders expanded.
Expansion expand(std::string_view text, const SnippetPlan& plan);

// What `snippet` types as plain text: its text, Markdown stripped if formatted.
std::string plain_text(const Snippet& snippet);
// `snippet` as an HTML fragment when it keeps formatting, else "".
std::string snippet_html(const Snippet& snippet);

} // namespace fv::text
