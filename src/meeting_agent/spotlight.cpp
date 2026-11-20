#include "meeting_agent/spotlight.h"

#include <algorithm>
#include <optional>
#include <set>
#include <tuple>

#include "meeting_agent/detail/py_value.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::meeting_agent {

namespace {

using detail::get;
using detail::is_text;
using detail::py_truthy;

constexpr double kJaccardMin = 0.60;
constexpr std::size_t kContainmentMinWords = 4;
constexpr double kContainmentMin = 0.75;

bool human_touched_status(const Json& status) {
    return is_text(status, "edited") || is_text(status, "confirmed");
}

bool is_unreviewed_repair(const Json& item) {
    const Json& synthesized = get(detail::get_object(item, "data"), "insight_synthesized");
    return is_text(get(item, "author_type"), "system") &&
           is_text(get(item, "author_id"), "state_repair") &&
           !(synthesized.is_boolean() && synthesized.get<bool>());
}

bool is_candidate(std::string_view key, const Json& item) {
    if (!item.is_object() || is_text(get(item, "status"), "removed")) {
        return false;
    }
    if (py_truthy(get(item, "pinned")) || human_touched_status(get(item, "status"))) {
        return true;
    }
    return key != "timeline" && !is_unreviewed_repair(item);
}

using SortKey = std::tuple<int, int, std::string>;

SortKey sort_key(const Json& item) {
    const int pinned = py_truthy(get(item, "pinned")) ? 1 : 0;
    const int touched = human_touched_status(get(item, "status")) ? 1 : 0;
    const Json& updated = get(item, "updated_at");
    std::string stamp = py_truthy(updated) ? detail::py_str(updated)
                                           : detail::str_or_empty(get(item, "created_at"));
    return {pinned, touched, std::move(stamp)};
}

std::string text_of(const Json& value) {
    return detail::str_or_empty(value);
}

bool duplicates_a_pick(const std::string& text, const std::vector<Json>& picks) {
    const std::string norm = normalized_text(text);
    if (norm.empty()) {
        return false;
    }
    for (const auto& pick : picks) {
        const std::string pick_norm = normalized_text(text_of(get(pick, "text")));
        if (!pick_norm.empty() && texts_overlap(norm, pick_norm)) {
            return true;
        }
    }
    return false;
}

void fill(const std::vector<Json>& ranked, std::vector<Json>& picks,
          std::set<std::string>& used_ids, std::size_t limit,
          std::optional<std::set<std::string>>& used_categories) {
    for (const auto& item : ranked) {
        if (picks.size() >= limit) {
            break;
        }
        const std::string id = detail::str_or_empty(get(item, "id"));
        const std::string category = detail::str_or_empty(get(item, "card"));
        if (used_categories && used_categories->contains(category)) {
            continue;
        }
        if (used_ids.contains(id)) {
            continue;
        }
        if (duplicates_a_pick(text_of(get(item, "text")), picks)) {
            continue;
        }
        picks.push_back(item);
        used_ids.insert(id);
        if (used_categories) {
            used_categories->insert(category);
        }
    }
}

std::set<std::string> words(std::string_view norm) {
    const auto parts = py::split(norm);
    return {parts.begin(), parts.end()};
}

} // namespace

std::string normalized_text(std::string_view text) {
    const std::string lowered = py::lower(text);
    std::string out;
    bool in_gap = false;
    std::size_t pos = 0;
    while (pos < lowered.size()) {
        const char32_t cp = py::next_code_point(lowered, pos);
        const bool keep = (cp >= U'a' && cp <= U'z') || (cp >= U'0' && cp <= U'9');
        if (keep) {
            out.push_back(static_cast<char>(cp));
            in_gap = false;
        } else if (!in_gap) {
            out.push_back(' ');
            in_gap = true;
        }
    }
    return py::strip(out);
}

bool texts_overlap(std::string_view norm, std::string_view other_norm) {
    if (norm == other_norm) {
        return true;
    }
    const auto mine = words(norm);
    const auto theirs = words(other_norm);
    if (mine.empty() || theirs.empty()) {
        return false;
    }
    std::size_t shared = 0;
    for (const auto& word : mine) {
        shared += theirs.contains(word) ? 1 : 0;
    }
    const std::size_t united = mine.size() + theirs.size() - shared;
    if (static_cast<double>(shared) / static_cast<double>(united) >= kJaccardMin) {
        return true;
    }
    const std::size_t smaller = std::min(mine.size(), theirs.size());
    return smaller >= kContainmentMinWords &&
           static_cast<double>(shared) / static_cast<double>(smaller) >= kContainmentMin;
}

std::vector<Json> select_spotlight_items(const Json& cards, int limit) {
    std::vector<Json> ranked;
    if (cards.is_object()) {
        for (const auto& [key, items] : cards.items()) {
            if (key == "live_notes" || key == "user_notes" || !items.is_array()) {
                continue;
            }
            for (const auto& item : items) {
                if (is_candidate(key, item)) {
                    Json copy = item;
                    copy["card"] = key;
                    ranked.push_back(std::move(copy));
                }
            }
        }
    }
    // sort(reverse=True) keeps equal keys in their original order.
    std::vector<SortKey> keys;
    keys.reserve(ranked.size());
    for (const auto& item : ranked) {
        keys.push_back(sort_key(item));
    }
    std::vector<std::size_t> order(ranked.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(order,
                             [&](std::size_t a, std::size_t b) { return keys[a] > keys[b]; });
    std::vector<Json> sorted;
    sorted.reserve(ranked.size());
    for (const std::size_t index : order) {
        sorted.push_back(std::move(ranked[index]));
    }

    const std::size_t cap = limit > 0 ? static_cast<std::size_t>(limit) : 0;
    std::vector<Json> picks;
    std::set<std::string> used_ids;
    std::optional<std::set<std::string>> categories = std::set<std::string>{};
    fill(sorted, picks, used_ids, cap, categories);
    std::optional<std::set<std::string>> any;
    fill(sorted, picks, used_ids, cap, any);
    return picks;
}

} // namespace fv::meeting_agent
