#include "text/vocabulary.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::text {

namespace {

constexpr char32_t kCombiningDot = 0x0307;   // COMBINING DOT ABOVE
constexpr char32_t kDottedCapitalI = 0x0130; // LATIN CAPITAL LETTER I WITH DOT ABOVE

// Folded code points kept behind the matcher before the window is compacted.
constexpr std::size_t kCompactAfter = 4096;

// Casefolding has no context, so folding code point by code point is the fold of the whole
// text; each distinct code point is folded once.
class CodePointFolder {
public:
    const std::u32string& fold(char32_t cp) {
        auto found = cache_.find(cp);
        if (found != cache_.end()) {
            return found->second;
        }
        std::string one;
        py::append_utf8(one, cp);
        return cache_.emplace(cp, py::to_code_points(py::casefold(one))).first->second;
    }

private:
    std::unordered_map<char32_t, std::u32string> cache_;
};

// The fold of a text, produced as far as it is read and kept only from the oldest position
// still needed, so a long transcript is matched in a window of bounded size. Each folded code
// point has an origin: the byte offset of the original character it came from, so equal origins
// mean the same character and an origin slices the original UTF-8. Positions are folded code
// point indices from the start of the text.
class FoldStream {
public:
    explicit FoldStream(std::string_view text) : text_(text) {}

    // Whether folded position i exists, folding more of the text as needed.
    bool has(std::size_t i) {
        while (base_ + folded_.size() <= i && pos_ < text_.size()) {
            step();
        }
        return i < base_ + folded_.size();
    }
    char32_t at(std::size_t i) const { return folded_[i - base_]; }
    std::size_t origin(std::size_t i) const { return origin_[i - base_]; }

    // Positions before i will not be read again.
    void release_before(std::size_t i) {
        const std::size_t drop = i - base_;
        if (drop >= kCompactAfter) {
            folded_.erase(0, drop);
            origin_.erase(origin_.begin(), origin_.begin() + static_cast<std::ptrdiff_t>(drop));
            base_ = i;
        }
    }

private:
    void step() {
        const std::size_t start = pos_;
        char32_t cp = py::next_code_point(text_, pos_);
        if (cp == kDottedCapitalI) {
            // "İ" is the only character whose fold has the dot, and "I" folds to the plain "i"
            // it should match.
            cp = U'I';
        }
        if (cp < 0x80) {
            emit(cp >= U'A' && cp <= U'Z' ? cp + (U'a' - U'A') : cp, start);
            return;
        }
        for (const char32_t folded_cp : folder_.fold(cp)) {
            emit(folded_cp, start);
        }
    }

    // Dots are dropped in whole runs that follow an "i" of the folded text, as Python's
    // re.sub("(?<=i)" + _COMBINING_DOT + "+", "") does.
    void emit(char32_t folded_cp, std::size_t start) {
        if (folded_cp == kCombiningDot) {
            if (after_i_) {
                return;
            }
        } else {
            after_i_ = folded_cp == U'i';
        }
        folded_.push_back(folded_cp);
        origin_.push_back(start);
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    bool after_i_ = false;
    CodePointFolder folder_;
    std::u32string folded_;
    std::vector<std::size_t> origin_;
    std::size_t base_ = 0;
};

std::u32string fold_key(std::string_view term) {
    FoldStream stream(term);
    std::u32string out;
    for (std::size_t i = 0; stream.has(i); ++i) {
        out.push_back(stream.at(i));
    }
    return out;
}

struct Key {
    std::u32string folded;
    const std::string* replacement;
};

} // namespace

FoldedText fold_with_origin(std::string_view text) {
    FoldStream stream(text);
    std::u32string folded;
    FoldedText out;
    // Origins are ascending byte offsets; count the original's code points up to each.
    std::size_t index = 0;
    std::size_t pos = 0;
    for (std::size_t i = 0; stream.has(i); ++i) {
        folded.push_back(stream.at(i));
        while (pos < stream.origin(i)) {
            py::next_code_point(text, pos);
            ++index;
        }
        out.origin.push_back(index);
    }
    out.folded = py::to_utf8(folded);
    return out;
}

std::string replace_terms(std::string_view text,
                          const std::vector<std::pair<std::string, std::string>>& rules) {
    if (rules.empty() || text.empty()) {
        return std::string(text);
    }
    std::vector<Key> keys;
    {
        std::unordered_map<std::u32string, bool> seen;
        for (const auto& [term, replacement] : rules) {
            std::u32string key = fold_key(term);
            if (!key.empty() && seen.emplace(key, true).second) {
                keys.push_back({std::move(key), &replacement});
            }
        }
    }
    if (keys.empty()) {
        return std::string(text);
    }
    // Python's pattern (?<!\w)(?:k1|k2|...)(?!\w) with the keys sorted longest first (a stable
    // sort, so equal lengths keep their order), matched here without a regex: an alternation of
    // a full dictionary's 2,500 variants is larger than PCRE2 compiles. At each position the
    // first key in that order that matches, its lookahead included, wins, as in sre.
    std::stable_sort(keys.begin(), keys.end(),
                     [](const Key& a, const Key& b) { return a.folded.size() > b.folded.size(); });
    std::unordered_map<char32_t, std::vector<const Key*>> by_first;
    for (const Key& key : keys) {
        by_first[key.folded.front()].push_back(&key);
    }

    FoldStream f(text);
    const auto matches_at = [&f](const Key& key, std::size_t i) {
        for (std::size_t k = 0; k < key.folded.size(); ++k) {
            if (!f.has(i + k) || f.at(i + k) != key.folded[k]) {
                return false;
            }
        }
        const std::size_t end = i + key.folded.size();
        return !f.has(end) || !py::is_word(f.at(end));
    };
    std::string out;
    std::size_t cursor = 0; // byte offset into text
    std::size_t i = 0;
    while (f.has(i)) {
        f.release_before(i > 0 ? i - 1 : 0);
        if (i > 0 && py::is_word(f.at(i - 1))) {
            ++i;
            continue;
        }
        const auto candidates = by_first.find(f.at(i));
        if (candidates == by_first.end()) {
            ++i;
            continue;
        }
        const Key* hit = nullptr;
        for (const Key* key : candidates->second) {
            if (matches_at(*key, i)) {
                hit = key;
                break;
            }
        }
        if (hit == nullptr) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        const std::size_t end = i + hit->folded.size();
        i = end;
        // Only whole original characters are replaced, never part of one that folded to
        // several.
        if (start > 0 && f.origin(start - 1) == f.origin(start)) {
            continue;
        }
        const bool at_end = !f.has(end);
        if (!at_end && f.origin(end - 1) == f.origin(end)) {
            continue;
        }
        const std::size_t from = f.origin(start);
        if (from > cursor) {
            out.append(text.substr(cursor, from - cursor));
        }
        out += *hit->replacement;
        cursor = at_end ? text.size() : f.origin(end);
    }
    if (cursor < text.size()) {
        out.append(text.substr(cursor));
    }
    return out;
}

} // namespace fv::text
