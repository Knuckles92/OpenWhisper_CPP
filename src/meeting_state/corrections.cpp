#include "meeting_state/corrections.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "meeting_state/detail/py_value.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "text/vocabulary.h"

namespace fv::meeting_state {

namespace {

using detail::get;
using detail::truthy;

constexpr std::int64_t kMaxOccurrenceIndex = 1000;

bool equals(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

// isinstance(value, str) and 0 < len(value.strip()) <= MAX_TERM_CHARS.
bool bounded_term(const Json& value) {
    if (!value.is_string()) {
        return false;
    }
    const std::size_t length = py::len(py::strip(value.get_ref<const std::string&>()));
    return length > 0 && length <= MAX_TERM_CHARS;
}

const Json& user_notes(const Json& state) {
    static const Json empty = Json::array();
    const Json& cards = get(state, "cards");
    const Json& notes = get(cards, "user_notes");
    return truthy(notes) ? notes : empty;
}

std::vector<Json> live_correction_notes(const Json& state) {
    std::vector<Json> notes;
    const Json& items = user_notes(state);
    if (!items.is_array()) {
        return notes;
    }
    for (const auto& item : items) {
        if (item.is_object() && !equals(get(item, "status"), "removed") &&
            is_correction_author(item)) {
            notes.push_back(item);
        }
    }
    return notes;
}

void set_rule(TermRules& rules, std::string key, std::string value) {
    for (auto& [existing, replacement] : rules) {
        if (existing == key) {
            replacement = std::move(value);
            return;
        }
    }
    rules.emplace_back(std::move(key), std::move(value));
}

} // namespace

std::string segment_fingerprint(std::string_view text) {
    std::uint64_t value = 0xCBF29CE484222325ULL;
    for (const char byte : text) {
        value = (value ^ static_cast<unsigned char>(byte)) * 0x100000001B3ULL;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "fnv1a64:%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

bool valid_segment_fingerprint(const Json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    constexpr std::string_view prefix = "fnv1a64:";
    if (text.size() != prefix.size() + 16 || !text.starts_with(prefix)) {
        return false;
    }
    return std::all_of(text.begin() + prefix.size(), text.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool is_correction_author(const Json& item) {
    const Json& author_type = get(item, "author_type");
    if (equals(author_type, "user")) {
        return true;
    }
    const Json& raw_data = get(item, "data");
    const Json& data = truthy(raw_data) ? raw_data : Json::object();
    return equals(author_type, "system") && equals(get(item, "author_id"), "voice_command") &&
           equals(get(data, "source"), "voice_command") &&
           equals(get(data, "command"), "fix_transcript");
}

TermRules term_rules_from_items(const Json& items) {
    TermRules rules;
    if (!items.is_array()) {
        return rules;
    }
    for (const auto& item : items) {
        if (!item.is_object()) {
            continue;
        }
        if (equals(get(item, "status"), "removed") || !is_correction_author(item)) {
            continue;
        }
        const Json& raw_data = get(item, "data");
        if (!raw_data.is_object() || !equals(get(raw_data, "kind"), "term_correction")) {
            continue;
        }
        const Json& source = get(raw_data, "selected_text");
        const Json& replacement = get(raw_data, "replacement");
        if (bounded_term(source) && bounded_term(replacement)) {
            set_rule(rules, py::lower(py::strip(source.get_ref<const std::string&>())),
                     py::strip(replacement.get_ref<const std::string&>()));
        }
    }
    return rules;
}

TermRules term_rules(const Json& state) {
    return term_rules_from_items(user_notes(state));
}

ScopedRules occurrence_rules(const Json& state) {
    ScopedRules grouped;
    for (const auto& item : live_correction_notes(state)) {
        const Json& data = get(item, "data");
        const Json& raw_evidence = get(item, "evidence");
        if (!data.is_object() || !equals(get(data, "kind"), "occurrence_correction")) {
            continue;
        }
        // `evidence or []` has one entry only when it is a truthy list of one.
        if (!raw_evidence.is_array() || raw_evidence.size() != 1 || !raw_evidence[0].is_string()) {
            continue;
        }
        const Json& source = get(data, "selected_text");
        const Json& replacement = get(data, "replacement");
        const Json& index = get(data, "occurrence_index");
        const Json& fingerprint = get(data, "base_fingerprint");
        // A JSON bool is never an integer here, as isinstance(index, bool) excludes it.
        if (!bounded_term(source) || !bounded_term(replacement) || !index.is_number_integer() ||
            !valid_segment_fingerprint(fingerprint)) {
            continue;
        }
        const std::int64_t position = index.get<std::int64_t>();
        if (position < 0 || position > kMaxOccurrenceIndex) {
            continue;
        }
        grouped[raw_evidence[0].get<std::string>()].push_back(
            {py::strip(source.get_ref<const std::string&>()),
             py::strip(replacement.get_ref<const std::string&>()), position,
             fingerprint.get<std::string>()});
    }
    return grouped;
}

std::string correct_text(std::string_view text, const TermRules& rules) {
    return text::replace_terms(text, rules);
}

std::string correct_segment_text(std::string_view text, std::string_view segment_id,
                                 const TermRules& rules, const ScopedRules& scoped) {
    std::string base = correct_text(text, rules);
    const auto found = scoped.find(segment_id);
    if (found == scoped.end() || found->second.empty()) {
        return base;
    }
    const std::string fingerprint = segment_fingerprint(base);
    // Byte spans of the base text in insertion order; the replacement of a repeated span is the
    // later note's.
    std::vector<std::pair<std::pair<std::size_t, std::size_t>, std::string>> edits;
    for (const auto& rule : found->second) {
        if (rule.base_fingerprint != fingerprint) {
            continue;
        }
        const py::Regex pattern("(?<!\\w)" + py::regex_escape(rule.selected_text) + "(?!\\w)",
                                py::IGNORECASE);
        const auto matches = pattern.finditer(base);
        if (rule.occurrence_index >= static_cast<std::int64_t>(matches.size())) {
            continue;
        }
        const auto& match = matches[static_cast<std::size_t>(rule.occurrence_index)];
        const std::pair<std::size_t, std::size_t> span{match.byte_start(), match.byte_end()};
        const bool overlaps = std::any_of(edits.begin(), edits.end(), [&](const auto& prior) {
            return span != prior.first && span.first < prior.first.second &&
                   prior.first.first < span.second;
        });
        if (overlaps) {
            continue;
        }
        auto same = std::find_if(edits.begin(), edits.end(),
                                 [&](const auto& prior) { return prior.first == span; });
        if (same != edits.end()) {
            same->second = rule.replacement;
        } else {
            edits.emplace_back(span, rule.replacement);
        }
    }
    std::sort(edits.begin(), edits.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string result = base;
    for (const auto& [span, replacement] : edits) {
        result = result.substr(0, span.first) + replacement + result.substr(span.second);
    }
    return result;
}

std::vector<std::string> vocabulary_terms(const TermRules& rules, std::size_t max_terms,
                                          std::size_t max_chars) {
    std::vector<std::string> terms;
    std::set<std::string, std::less<>> seen;
    std::size_t used = 0;
    for (const auto& [term, replacement] : rules) {
        std::string key = py::lower(replacement);
        if (seen.contains(key)) {
            continue;
        }
        const std::size_t cost = py::len(replacement) + (terms.empty() ? 0 : 2);
        if (terms.size() >= max_terms || used + cost > max_chars) {
            break;
        }
        seen.insert(std::move(key));
        terms.push_back(replacement);
        used += cost;
    }
    return terms;
}

std::string vocabulary_hint(const TermRules& rules) {
    const auto terms = vocabulary_terms(rules);
    if (terms.empty()) {
        return "";
    }
    std::string hint;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i > 0) {
            hint += ", ";
        }
        hint += terms[i];
    }
    return hint + ".";
}

} // namespace fv::meeting_state
