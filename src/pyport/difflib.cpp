#include "pyport/difflib.h"

#include <algorithm>
#include <tuple>
#include <utility>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/unicode.h"

namespace fv::py {
namespace {

double calculate_ratio(std::size_t matches, std::size_t length) {
    return length != 0 ? 2.0 * static_cast<double>(matches) / static_cast<double>(length) : 1.0;
}

} // namespace

std::string_view op_tag_name(OpTag tag) {
    switch (tag) {
    case OpTag::Replace:
        return "replace";
    case OpTag::Delete:
        return "delete";
    case OpTag::Insert:
        return "insert";
    case OpTag::Equal:
        return "equal";
    }
    return "";
}

template <class T>
BasicSequenceMatcher<T>::BasicSequenceMatcher(std::vector<T> a, std::vector<T> b, bool autojunk)
    : a_(std::move(a)), b_(std::move(b)), autojunk_(autojunk) {
    chain_b();
}

template <class T> void BasicSequenceMatcher<T>::set_seq1(std::vector<T> a) {
    a_ = std::move(a);
    matching_blocks_.reset();
    opcodes_.reset();
}

template <class T> void BasicSequenceMatcher<T>::set_seq2(std::vector<T> b) {
    b_ = std::move(b);
    matching_blocks_.reset();
    opcodes_.reset();
    fullbcount_.reset();
    chain_b();
}

template <class T> void BasicSequenceMatcher<T>::chain_b() {
    b2j_.clear();
    for (std::size_t i = 0; i < b_.size(); ++i) {
        b2j_[b_[i]].push_back(i);
    }
    // isjunk is None, so bjunk is empty; only the popular elements are dropped.
    const std::size_t n = b_.size();
    if (autojunk_ && n >= 200) {
        const std::size_t ntest = n / 100 + 1;
        std::erase_if(b2j_, [ntest](const auto& entry) { return entry.second.size() > ntest; });
    }
}

template <class T>
MatchBlock BasicSequenceMatcher<T>::find_longest_match(std::size_t alo, std::size_t ahi,
                                                       std::size_t blo, std::size_t bhi) const {
    std::size_t besti = alo;
    std::size_t bestj = blo;
    std::size_t bestsize = 0;
    // j2len[j] is the length of the longest match ending with a[i-1] and b[j].
    std::unordered_map<std::size_t, std::size_t> j2len;
    std::unordered_map<std::size_t, std::size_t> newj2len;
    for (std::size_t i = alo; i < ahi; ++i) {
        newj2len.clear();
        const auto found = b2j_.find(a_[i]);
        if (found != b2j_.end()) {
            for (const std::size_t j : found->second) {
                if (j < blo) {
                    continue;
                }
                if (j >= bhi) {
                    break;
                }
                std::size_t k = 1;
                if (j > 0) {
                    const auto prev = j2len.find(j - 1);
                    if (prev != j2len.end()) {
                        k = prev->second + 1;
                    }
                }
                newj2len[j] = k;
                if (k > bestsize) {
                    besti = i - k + 1;
                    bestj = j - k + 1;
                    bestsize = k;
                }
            }
        }
        std::swap(j2len, newj2len);
    }
    // With no junk elements Python's four extension loops reduce to these two.
    while (besti > alo && bestj > blo && a_[besti - 1] == b_[bestj - 1]) {
        --besti;
        --bestj;
        ++bestsize;
    }
    while (besti + bestsize < ahi && bestj + bestsize < bhi &&
           a_[besti + bestsize] == b_[bestj + bestsize]) {
        ++bestsize;
    }
    return {besti, bestj, bestsize};
}

template <class T> const std::vector<MatchBlock>& BasicSequenceMatcher<T>::get_matching_blocks() {
    if (matching_blocks_) {
        return *matching_blocks_;
    }
    const std::size_t la = a_.size();
    const std::size_t lb = b_.size();
    std::vector<std::tuple<std::size_t, std::size_t, std::size_t, std::size_t>> queue{
        {0, la, 0, lb}};
    std::vector<MatchBlock> blocks;
    while (!queue.empty()) {
        const auto [alo, ahi, blo, bhi] = queue.back();
        queue.pop_back();
        const MatchBlock x = find_longest_match(alo, ahi, blo, bhi);
        if (x.size != 0) {
            blocks.push_back(x);
            if (alo < x.a && blo < x.b) {
                queue.emplace_back(alo, x.a, blo, x.b);
            }
            if (x.a + x.size < ahi && x.b + x.size < bhi) {
                queue.emplace_back(x.a + x.size, ahi, x.b + x.size, bhi);
            }
        }
    }
    std::sort(blocks.begin(), blocks.end(), [](const MatchBlock& l, const MatchBlock& r) {
        return std::tie(l.a, l.b, l.size) < std::tie(r.a, r.b, r.size);
    });
    std::vector<MatchBlock> non_adjacent;
    std::size_t i1 = 0, j1 = 0, k1 = 0;
    for (const MatchBlock& block : blocks) {
        if (i1 + k1 == block.a && j1 + k1 == block.b) {
            k1 += block.size;
        } else {
            if (k1 != 0) {
                non_adjacent.push_back({i1, j1, k1});
            }
            i1 = block.a;
            j1 = block.b;
            k1 = block.size;
        }
    }
    if (k1 != 0) {
        non_adjacent.push_back({i1, j1, k1});
    }
    non_adjacent.push_back({la, lb, 0});
    matching_blocks_ = std::move(non_adjacent);
    return *matching_blocks_;
}

template <class T> const std::vector<Opcode>& BasicSequenceMatcher<T>::get_opcodes() {
    if (opcodes_) {
        return *opcodes_;
    }
    std::vector<Opcode> answer;
    std::size_t i = 0;
    std::size_t j = 0;
    for (const MatchBlock& block : get_matching_blocks()) {
        if (i < block.a && j < block.b) {
            answer.push_back({OpTag::Replace, i, block.a, j, block.b});
        } else if (i < block.a) {
            answer.push_back({OpTag::Delete, i, block.a, j, block.b});
        } else if (j < block.b) {
            answer.push_back({OpTag::Insert, i, block.a, j, block.b});
        }
        i = block.a + block.size;
        j = block.b + block.size;
        if (block.size != 0) {
            answer.push_back({OpTag::Equal, block.a, i, block.b, j});
        }
    }
    opcodes_ = std::move(answer);
    return *opcodes_;
}

template <class T> double BasicSequenceMatcher<T>::ratio() {
    std::size_t matches = 0;
    for (const MatchBlock& block : get_matching_blocks()) {
        matches += block.size;
    }
    return calculate_ratio(matches, a_.size() + b_.size());
}

template <class T> double BasicSequenceMatcher<T>::quick_ratio() {
    if (!fullbcount_) {
        fullbcount_.emplace();
        for (const T& elt : b_) {
            ++(*fullbcount_)[elt];
        }
    }
    std::unordered_map<T, long long> avail;
    std::size_t matches = 0;
    for (const T& elt : a_) {
        long long numb = 0;
        if (const auto it = avail.find(elt); it != avail.end()) {
            numb = it->second;
        } else if (const auto full = fullbcount_->find(elt); full != fullbcount_->end()) {
            numb = static_cast<long long>(full->second);
        }
        avail[elt] = numb - 1;
        if (numb > 0) {
            ++matches;
        }
    }
    return calculate_ratio(matches, a_.size() + b_.size());
}

template <class T> double BasicSequenceMatcher<T>::real_quick_ratio() const {
    return calculate_ratio(std::min(a_.size(), b_.size()), a_.size() + b_.size());
}

template class BasicSequenceMatcher<char32_t>;
template class BasicSequenceMatcher<std::string>;

namespace {
std::vector<char32_t> code_point_vector(std::string_view utf8) {
    const std::u32string cps = to_code_points(utf8);
    return {cps.begin(), cps.end()};
}
} // namespace

SequenceMatcher::SequenceMatcher(std::string_view a, std::string_view b, bool autojunk)
    : BasicSequenceMatcher<char32_t>(code_point_vector(a), code_point_vector(b), autojunk) {}

std::vector<std::string> get_close_matches(std::string_view word,
                                           const std::vector<std::string>& possibilities, int n,
                                           double cutoff) {
    if (n <= 0) {
        throw ValueError("n must be > 0: " + std::to_string(n));
    }
    if (!(cutoff >= 0.0 && cutoff <= 1.0)) {
        throw ValueError("cutoff must be in [0.0, 1.0]: " + float_repr(cutoff));
    }
    std::vector<std::pair<double, const std::string*>> result;
    BasicSequenceMatcher<char32_t> s({}, code_point_vector(word));
    for (const std::string& x : possibilities) {
        s.set_seq1(code_point_vector(x));
        if (s.real_quick_ratio() >= cutoff && s.quick_ratio() >= cutoff && s.ratio() >= cutoff) {
            result.emplace_back(s.ratio(), &x);
        }
    }
    // heapq.nlargest over (score, x): higher score first, then the larger string.
    std::sort(result.begin(), result.end(), [](const auto& l, const auto& r) {
        if (l.first != r.first) {
            return l.first > r.first;
        }
        return *l.second > *r.second;
    });
    std::vector<std::string> out;
    for (std::size_t i = 0; i < result.size() && i < static_cast<std::size_t>(n); ++i) {
        out.push_back(*result[i].second);
    }
    return out;
}

} // namespace fv::py
