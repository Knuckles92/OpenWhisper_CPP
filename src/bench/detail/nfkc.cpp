#include "bench/detail/nfkc.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include "pyport/unicode.h"

namespace fv::bench::detail {

namespace {

struct Decomposition {
    std::uint32_t cp;
    std::uint32_t offset;
    std::uint32_t length;
};
struct CombiningClass {
    std::uint32_t cp;
    std::uint32_t ccc;
};
struct Composition {
    std::uint32_t first;
    std::uint32_t second;
    std::uint32_t composite;
};

#include "bench/detail/nfkc_tables.inc"

// The Hangul syllable algorithm of Unicode chapter 3.12.
constexpr char32_t kSBase = 0xAC00;
constexpr char32_t kLBase = 0x1100;
constexpr char32_t kVBase = 0x1161;
constexpr char32_t kTBase = 0x11A7;
constexpr char32_t kLCount = 19;
constexpr char32_t kVCount = 21;
constexpr char32_t kTCount = 28;
constexpr char32_t kNCount = kVCount * kTCount;
constexpr char32_t kSCount = kLCount * kNCount;

std::uint32_t combining_class(char32_t cp) {
    const auto* end = std::end(kCombiningClasses);
    const auto* found = std::lower_bound(
        std::begin(kCombiningClasses), end, cp,
        [](const CombiningClass& entry, char32_t value) { return entry.cp < value; });
    return found != end && found->cp == cp ? found->ccc : 0;
}

void decompose(char32_t cp, std::u32string& out) {
    if (cp >= kSBase && cp < kSBase + kSCount) {
        const char32_t index = cp - kSBase;
        out.push_back(kLBase + index / kNCount);
        out.push_back(kVBase + (index % kNCount) / kTCount);
        if (index % kTCount != 0) {
            out.push_back(kTBase + index % kTCount);
        }
        return;
    }
    const auto* end = std::end(kDecompositions);
    const auto* found = std::lower_bound(
        std::begin(kDecompositions), end, cp,
        [](const Decomposition& entry, char32_t value) { return entry.cp < value; });
    if (found == end || found->cp != cp) {
        out.push_back(cp);
        return;
    }
    for (std::uint32_t i = 0; i < found->length; ++i) {
        out.push_back(kDecompositionPool[found->offset + i]);
    }
}

std::optional<char32_t> compose_pair(char32_t first, char32_t second) {
    if (first >= kLBase && first < kLBase + kLCount && second >= kVBase &&
        second < kVBase + kVCount) {
        return kSBase + ((first - kLBase) * kVCount + (second - kVBase)) * kTCount;
    }
    if (first >= kSBase && first < kSBase + kSCount && (first - kSBase) % kTCount == 0 &&
        second > kTBase && second < kTBase + kTCount) {
        return first + (second - kTBase);
    }
    const auto* end = std::end(kCompositions);
    const auto* found = std::lower_bound(
        std::begin(kCompositions), end, std::pair<char32_t, char32_t>(first, second),
        [](const Composition& entry, const auto& key) {
            return entry.first < key.first ||
                   (entry.first == key.first && entry.second < key.second);
        });
    if (found != end && found->first == first && found->second == second) {
        return found->composite;
    }
    return std::nullopt;
}

} // namespace

std::string nfkc(std::string_view text) {
    std::u32string decomposed;
    for (const char32_t cp : py::to_code_points(text)) {
        decompose(cp, decomposed);
    }
    // Canonical ordering: a stable sort of each run of non-starters by combining class.
    for (std::size_t i = 0; i < decomposed.size();) {
        if (combining_class(decomposed[i]) == 0) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < decomposed.size() && combining_class(decomposed[end]) != 0) {
            ++end;
        }
        std::stable_sort(
            decomposed.begin() + static_cast<std::ptrdiff_t>(i),
            decomposed.begin() + static_cast<std::ptrdiff_t>(end),
            [](char32_t a, char32_t b) { return combining_class(a) < combining_class(b); });
        i = end;
    }
    // Canonical composition: each character joins the last starter unless a character between
    // them blocks it (same or zero class).
    std::u32string composed;
    std::optional<std::size_t> starter;
    std::uint32_t last_class = 0;
    for (const char32_t cp : decomposed) {
        const std::uint32_t ccc = combining_class(cp);
        if (starter) {
            const bool adjacent = composed.size() == *starter + 1;
            if (adjacent || (last_class != 0 && last_class < ccc)) {
                if (const auto joined = compose_pair(composed[*starter], cp)) {
                    composed[*starter] = *joined;
                    continue;
                }
            }
        }
        composed.push_back(cp);
        if (ccc == 0) {
            starter = composed.size() - 1;
            last_class = 0;
        } else {
            last_class = ccc;
        }
    }
    return py::to_utf8(composed);
}

} // namespace fv::bench::detail
