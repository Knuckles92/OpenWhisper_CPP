#include "uimath/fuzzy_match.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv {
namespace {

using Text = std::u32string;

// Base score of each strategy, best first. Bonuses and penalties stay small enough that a
// strategy never outranks the one above it.
constexpr int kExactScore = 10000;
constexpr int kSubstringScore = 3000;
constexpr int kNormalizedSubstringScore = 2200;
constexpr int kMultiTokenScore = 1800;
constexpr int kStrippedSubstringScore = 1500;
constexpr int kSubsequenceScore = 700;
constexpr int kSubsequenceMinScore = 100;

constexpr char32_t kPathSeparator = U'/';
constexpr std::u32string_view kWordDelimiters = U" -_:.";
constexpr std::u32string_view kBoundaryChars = U" -_:./";

// Shorter query tokens only match whole tokens or token prefixes, so that "o" does not match the
// inside of every target token.
constexpr std::size_t kMinInfixTokenLen = 3;
constexpr std::size_t kMinSubsequenceLen = 2;

bool contains(std::u32string_view set, char32_t c) {
    return set.find(c) != std::u32string_view::npos;
}

// re.split(r"[^\w]+", text.casefold()) without the empty pieces: the runs of word characters.
std::vector<Text> split_tokens(const Text& text) {
    const Text folded = py::to_code_points(py::casefold(py::to_utf8(text)));
    std::vector<Text> tokens;
    Text current;
    for (const char32_t c : folded) {
        if (py::is_word(c)) {
            current.push_back(c);
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

Text join(const std::vector<Text>& tokens, std::u32string_view separator) {
    Text out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += tokens[i];
    }
    return out;
}

Text normalize_delimiters(const Text& text) {
    return join(split_tokens(text), U" ");
}

Text strip_delimiters(const Text& text) {
    return join(split_tokens(text), U"");
}

int position_penalty(std::size_t pos) {
    return static_cast<int>(std::min<std::size_t>(100, pos * 2));
}

std::optional<int> score_substring(const Text& q, const Text& t) {
    const std::size_t pos = t.find(q);
    if (pos == Text::npos) {
        return std::nullopt;
    }
    int prefix_bonus = 0;
    if (pos == 0 || t[pos - 1] == kPathSeparator) {
        prefix_bonus = 1000;
    } else if (contains(kWordDelimiters, t[pos - 1])) {
        prefix_bonus = 500;
    }
    return kSubstringScore + prefix_bonus - position_penalty(pos);
}

std::optional<int> score_normalized_substring(const Text& q, const Text& t) {
    const Text q_norm = normalize_delimiters(q);
    if (q_norm.empty()) {
        return std::nullopt;
    }
    const Text t_norm = normalize_delimiters(t);
    const std::size_t pos = t_norm.find(q_norm);
    if (pos == Text::npos) {
        return std::nullopt;
    }
    int prefix_bonus = 0;
    if (pos == 0) {
        prefix_bonus = 800;
    } else if (t_norm[pos - 1] == U' ') {
        prefix_bonus = 500;
    }
    return kNormalizedSubstringScore + prefix_bonus - position_penalty(pos);
}

// The first free target token matching q_tok: an exact token, then a prefix, then (for longer
// query tokens) a substring.
std::optional<std::size_t> match_token(const Text& q_tok, const std::vector<Text>& t_tokens,
                                       const std::vector<std::size_t>& taken) {
    const auto free = [&](std::size_t idx) {
        return std::find(taken.begin(), taken.end(), idx) == taken.end();
    };
    for (std::size_t idx = 0; idx < t_tokens.size(); ++idx) {
        if (free(idx) && t_tokens[idx] == q_tok) {
            return idx;
        }
    }
    for (std::size_t idx = 0; idx < t_tokens.size(); ++idx) {
        if (free(idx) && t_tokens[idx].starts_with(q_tok)) {
            return idx;
        }
    }
    if (q_tok.size() >= kMinInfixTokenLen) {
        for (std::size_t idx = 0; idx < t_tokens.size(); ++idx) {
            if (free(idx) && t_tokens[idx].find(q_tok) != Text::npos) {
                return idx;
            }
        }
    }
    return std::nullopt;
}

std::optional<int> score_multi_token(const Text& q, const Text& t) {
    const std::vector<Text> q_tokens = split_tokens(q);
    const std::vector<Text> t_tokens = split_tokens(t);
    if (q_tokens.size() < 2 || t_tokens.empty()) {
        return std::nullopt;
    }
    std::vector<std::size_t> matched;
    for (const Text& q_tok : q_tokens) {
        const std::optional<std::size_t> idx = match_token(q_tok, t_tokens, matched);
        if (!idx) {
            return std::nullopt;
        }
        matched.push_back(*idx);
    }
    int exact_count = 0;
    for (std::size_t i = 0; i < q_tokens.size(); ++i) {
        if (t_tokens[matched[i]] == q_tokens[i]) {
            ++exact_count;
        }
    }
    const int order_bonus = std::is_sorted(matched.begin(), matched.end()) ? 200 : 0;
    return kMultiTokenScore + order_bonus + exact_count * 50;
}

std::optional<int> score_stripped_substring(const Text& q, const Text& t) {
    const Text q_stripped = strip_delimiters(q);
    if (q_stripped.empty()) {
        return std::nullopt;
    }
    const std::size_t pos = strip_delimiters(t).find(q_stripped);
    if (pos == Text::npos) {
        return std::nullopt;
    }
    return kStrippedSubstringScore - position_penalty(pos);
}

std::optional<int> score_subsequence(const Text& q, const Text& t) {
    const Text q_stripped = strip_delimiters(q);
    if (q_stripped.size() < kMinSubsequenceLen) {
        return std::nullopt;
    }
    // The leftmost positions of the query's characters, in order.
    std::vector<std::size_t> positions;
    std::size_t start = 0;
    for (const char32_t c : q_stripped) {
        const std::size_t pos = t.find(c, start);
        if (pos == Text::npos) {
            return std::nullopt;
        }
        positions.push_back(pos);
        start = pos + 1;
    }
    int boundary_bonus = 0;
    for (const std::size_t pos : positions) {
        if (pos == 0 || contains(kBoundaryChars, t[pos - 1])) {
            boundary_bonus += 40;
        }
    }
    // Each match extending a run of adjacent matches earns 20 per earlier match in that run, so
    // longer runs are rewarded more than linearly.
    int consecutive_bonus = 0;
    int run = 1;
    for (std::size_t i = 1; i < positions.size(); ++i) {
        run = positions[i] == positions[i - 1] + 1 ? run + 1 : 1;
        consecutive_bonus += (run - 1) * 20;
    }
    const int span_penalty =
        static_cast<int>(std::min<std::size_t>(200, (positions.back() - positions.front()) * 3));
    const int score = kSubsequenceScore + consecutive_bonus + boundary_bonus - span_penalty;
    return std::max(kSubsequenceMinScore, score);
}

} // namespace

std::optional<int> fuzzy_score(std::string_view query, std::string_view target) {
    const std::string q_raw = py::strip(query);
    if (q_raw.empty()) {
        return 0;
    }
    const std::string t_raw = py::strip(target);
    if (t_raw.empty()) {
        return std::nullopt;
    }
    const Text q = py::to_code_points(py::casefold(q_raw));
    const Text t = py::to_code_points(py::casefold(t_raw));
    if (q == t) {
        return kExactScore;
    }
    using Strategy = std::optional<int> (*)(const Text&, const Text&);
    constexpr std::array<Strategy, 5> strategies{score_substring, score_normalized_substring,
                                                 score_multi_token, score_stripped_substring,
                                                 score_subsequence};
    for (const Strategy strategy : strategies) {
        if (const std::optional<int> score = strategy(q, t)) {
            return score;
        }
    }
    return std::nullopt;
}

bool fuzzy_match(std::string_view query, std::string_view target) {
    return fuzzy_score(query, target).has_value();
}

} // namespace fv
