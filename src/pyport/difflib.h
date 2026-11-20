#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fv::py {

struct MatchBlock {
    std::size_t a = 0;
    std::size_t b = 0;
    std::size_t size = 0;
    friend bool operator==(const MatchBlock&, const MatchBlock&) = default;
};

enum class OpTag { Replace, Delete, Insert, Equal };
std::string_view op_tag_name(OpTag tag); // "replace", "delete", "insert", "equal"

struct Opcode {
    OpTag tag;
    std::size_t i1, i2, j1, j2;
    friend bool operator==(const Opcode&, const Opcode&) = default;
};

// difflib.SequenceMatcher(None, a, b, autojunk) over two sequences of T (isjunk is always
// None, the only form OpenWhisper uses). With autojunk, an element of b that occurs more than
// len(b) // 100 + 1 times is ignored as a match anchor once len(b) >= 200.
// Instantiated for char32_t (strings, by code point) and std::string (word lists).
template <class T> class BasicSequenceMatcher {
public:
    BasicSequenceMatcher(std::vector<T> a, std::vector<T> b, bool autojunk = true);

    void set_seq1(std::vector<T> a);
    void set_seq2(std::vector<T> b);

    MatchBlock find_longest_match(std::size_t alo, std::size_t ahi, std::size_t blo,
                                  std::size_t bhi) const;
    const std::vector<MatchBlock>& get_matching_blocks();
    const std::vector<Opcode>& get_opcodes();
    double ratio();
    double quick_ratio();
    double real_quick_ratio() const;

private:
    void chain_b();

    std::vector<T> a_;
    std::vector<T> b_;
    bool autojunk_;
    std::unordered_map<T, std::vector<std::size_t>> b2j_;
    std::optional<std::unordered_map<T, std::size_t>> fullbcount_;
    std::optional<std::vector<MatchBlock>> matching_blocks_;
    std::optional<std::vector<Opcode>> opcodes_;
};

extern template class BasicSequenceMatcher<char32_t>;
extern template class BasicSequenceMatcher<std::string>;

using WordSequenceMatcher = BasicSequenceMatcher<std::string>;

// SequenceMatcher over two UTF-8 strings, element by element in code points.
class SequenceMatcher : public BasicSequenceMatcher<char32_t> {
public:
    SequenceMatcher(std::string_view a, std::string_view b, bool autojunk = true);
};

// difflib.get_close_matches: the best n possibilities scoring at least cutoff, best first
// (ties by the larger string). Throws ValueError for n <= 0 or cutoff outside [0, 1].
std::vector<std::string> get_close_matches(std::string_view word,
                                           const std::vector<std::string>& possibilities, int n = 3,
                                           double cutoff = 0.6);

} // namespace fv::py
