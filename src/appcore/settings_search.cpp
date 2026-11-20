#include "appcore/settings_search.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <utility>

#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::appcore {

namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    // A code-point substring of UTF-8 text is a byte substring, and the reverse holds too.
    return haystack.find(needle) != std::string_view::npos;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

std::string join_words(std::string_view query) {
    std::string joined;
    for (const auto& word : py::split(query)) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += word;
    }
    return joined;
}

// str.find over code points: the index of the first `needle` at or after `from`, or nullopt.
std::optional<std::size_t> find_from(std::u32string_view haystack, std::u32string_view needle,
                                     std::size_t from = 0) {
    if (from > haystack.size()) {
        return std::nullopt;
    }
    const std::size_t at = haystack.find(needle, from);
    if (at == std::u32string_view::npos) {
        return std::nullopt;
    }
    return at;
}

std::string wrap(std::u32string_view run, bool marked, std::string_view color) {
    std::string escaped = py::html_escape(py::to_utf8(run));
    if (!marked) {
        return escaped;
    }
    return std::format("<span style=\"color:{}; font-weight:700\">{}</span>", color, escaped);
}

} // namespace

std::string SearchEntry::haystack() const {
    return py::casefold(std::format("{} {} {}", title, detail, keywords));
}

std::vector<std::string> search_tokens(std::string_view query) {
    return py::split(py::casefold(query));
}

int search_score(const SearchEntry& entry, std::string_view query,
                 std::span<const std::string> tokens) {
    const std::string title = py::casefold(entry.title);
    if (starts_with(title, query)) {
        return 0;
    }
    if (contains(title, query)) {
        return 1;
    }
    if (std::ranges::all_of(tokens,
                            [&](const std::string& token) { return contains(title, token); })) {
        return 2;
    }
    return 3;
}

std::vector<SearchEntry> match_entries(std::span<const SearchEntry> entries,
                                       std::string_view query) {
    const std::string folded = py::casefold(join_words(query));
    const std::vector<std::string> tokens = search_tokens(folded);
    if (tokens.empty()) {
        return {};
    }
    struct Hit {
        int score;
        std::size_t length;
        const SearchEntry* entry;
    };
    std::vector<Hit> hits;
    for (const auto& entry : entries) {
        const std::string haystack = entry.haystack();
        if (std::ranges::all_of(
                tokens, [&](const std::string& token) { return contains(haystack, token); })) {
            hits.push_back({search_score(entry, folded, tokens), py::len(entry.title), &entry});
        }
    }
    // list.sort is stable: equal keys keep the entries' order.
    std::ranges::stable_sort(hits, [](const Hit& a, const Hit& b) {
        return std::pair(a.score, a.length) < std::pair(b.score, b.length);
    });
    std::vector<SearchEntry> ordered;
    for (const SearchKind kind : {SearchKind::Setting, SearchKind::Model, SearchKind::Help}) {
        std::size_t taken = 0;
        const std::size_t limit = kSectionLimits[static_cast<std::size_t>(kind)];
        for (const auto& hit : hits) {
            if (hit.entry->kind != kind) {
                continue;
            }
            if (taken == limit) {
                break;
            }
            ordered.push_back(*hit.entry);
            ++taken;
        }
    }
    return ordered;
}

std::string snippet(std::string_view text, std::span<const std::string> tokens, std::size_t width) {
    const std::size_t length = py::len(text);
    if (length <= width) {
        return std::string(text);
    }
    const std::u32string folded = py::to_code_points(py::casefold(text));
    std::optional<std::size_t> first;
    for (const auto& token : tokens) {
        if (const auto at = find_from(folded, py::to_code_points(token))) {
            first = first ? std::min(*first, *at) : *at;
        }
    }
    const std::size_t found = first.value_or(0);
    const std::size_t start = found > width / 3 ? found - width / 3 : 0;
    const std::size_t end = std::min(length, start + width);
    const std::string cut = py::strip(
        py::slice(text, static_cast<std::int64_t>(start), static_cast<std::int64_t>(end)));
    return std::string(start != 0 ? "…" : "") + cut + (end < length ? "…" : "");
}

std::string highlight(std::string_view text, std::span<const std::string> tokens,
                      std::string_view color) {
    const std::u32string chars = py::to_code_points(text);
    // origin[i]: the index in `chars` of the character folded position i came from.
    std::vector<std::size_t> origin;
    std::u32string folded;
    for (std::size_t index = 0; index < chars.size(); ++index) {
        std::string one;
        py::append_utf8(one, chars[index]);
        const std::u32string piece = py::to_code_points(py::casefold(one));
        folded += piece;
        origin.insert(origin.end(), piece.size(), index);
    }
    std::vector<bool> marks(chars.size(), false);
    for (const auto& token_text : tokens) {
        const std::u32string token = py::to_code_points(token_text);
        if (token.empty()) {
            continue;
        }
        std::optional<std::size_t> start = find_from(folded, token);
        while (start) {
            for (std::size_t index = *start; index < *start + token.size(); ++index) {
                marks[origin[index]] = true;
            }
            start = find_from(folded, token, *start + token.size());
        }
    }
    std::string out;
    std::u32string run;
    bool marked = false;
    for (std::size_t index = 0; index < chars.size(); ++index) {
        if (marks[index] != marked && !run.empty()) {
            out += wrap(run, marked, color);
            run.clear();
        }
        run += chars[index];
        marked = marks[index];
    }
    if (!run.empty()) {
        out += wrap(run, marked, color);
    }
    return out;
}

} // namespace fv::appcore
