#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The ranking and text rules of the Settings search palette (ui_qt/dialogs/settings_search.py):
// which entries a query finds and in what order, the help-text snippet and the highlighted
// HTML. The palette widget (M2.24) builds the entries from its pages and shows these results.
// Affinity: thread-safe (pure functions).
namespace fv::appcore {

// SETTING, MODEL and HELP, in the order the palette lists its sections.
enum class SearchKind { Setting, Model, Help };

// SECTION_TITLES.
constexpr std::string_view section_title(SearchKind kind) {
    switch (kind) {
    case SearchKind::Setting:
        return "Settings";
    case SearchKind::Model:
        return "Models & downloads";
    case SearchKind::Help:
        break;
    }
    return "Help text";
}

// SECTION_LIMITS: most results shown per section, so one section cannot bury the others.
// Indexed by SearchKind.
inline constexpr std::array<std::size_t, 3> kSectionLimits{6, 5, 3};

// One thing the palette can find and open.
struct SearchEntry {
    SearchKind kind = SearchKind::Setting;
    std::string title;
    std::string detail;
    std::string destination;
    // The widget to reveal, owned by the caller and never dereferenced here (Python's
    // `target`); null when the entry names its control by `target_name` instead.
    const void* target = nullptr;
    std::string target_name;
    std::string model_name;
    std::string component_id;
    std::vector<std::string> badges;
    std::string keywords;
    std::string icon = "box-blue.svg";

    // f"{title} {detail} {keywords}".casefold()
    std::string haystack() const;
};

// _tokens: the casefolded query split on Unicode whitespace.
std::vector<std::string> search_tokens(std::string_view query);

// _score: 0 when the folded title starts with the query, 1 when it contains it, 2 when it holds
// every token, else 3. `query` is the folded query with its whitespace runs joined by one space.
int search_score(const SearchEntry& entry, std::string_view query,
                 std::span<const std::string> tokens);

// match_entries: the entries whose haystack holds every query word, ordered by (score, title
// length in code points) with ties kept in input order, then grouped Settings, Models, Help and
// each group capped by kSectionLimits. An empty or blank query finds nothing.
std::vector<SearchEntry> match_entries(std::span<const SearchEntry> entries,
                                       std::string_view query);

// _snippet: a long help text cut to `width` code points around its first match, with "…" where
// it was cut. Like Python, the match position is taken in the casefolded text and applied to the
// original, which can shift the window when folding lengthens a character.
std::string snippet(std::string_view text, std::span<const std::string> tokens,
                    std::size_t width = 110);

// highlight: `text` as HTML (html.escape, quotes included) with every occurrence of each token
// in its casefolded form wrapped in <span style="color:{color}; font-weight:700">. Folding can
// lengthen a character ("ß" -> "ss"), so a match tints the original characters it came from.
std::string highlight(std::string_view text, std::span<const std::string> tokens,
                      std::string_view color);

} // namespace fv::appcore
