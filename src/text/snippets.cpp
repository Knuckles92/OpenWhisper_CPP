#include "text/snippets.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include "base/identity.h"
#include "base/ids.h"
#include "pyport/difflib.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "text/snippet_markdown.h"

namespace fv::text {

namespace {

// A word keeps apostrophes and dots between its letters; anything else separates words.
const py::Regex& word_pattern() {
    static const py::Regex pattern(R"(\w+(?:['’.]\w+)*)");
    return pattern;
}

// What may sit between the words of one trigger inside a sentence: speech engines add commas
// and hyphens, but a sentence end splits a trigger.
const py::Regex& word_gap() {
    static const py::Regex pattern(R"([\s,\-‐‑‒–—]*)");
    return pattern;
}

// Plain ASCII, so every cleanup model and tokenizer sees ordinary characters, and speech
// engines never write double brackets. Matched loosely ("[[ s1 ]]") because a model may space
// or lowercase them.
const py::Regex& placeholder_pattern() {
    static const py::Regex pattern(R"(\[\[\s*[Ss]\s*(\d{1,4})\s*\]\])");
    return pattern;
}

std::string placeholder(std::size_t number) {
    return std::format("[[S{}]]", number);
}

struct Word {
    std::string key;       // casefolded, "’" as "'"
    std::size_t start = 0; // byte offsets into the text
    std::size_t end = 0;
};

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

std::vector<Word> words(std::string_view text) {
    std::vector<Word> out;
    for (const py::Match& match : word_pattern().finditer(text)) {
        out.push_back({replace_all(py::casefold(match.group()), "’", "'"), match.byte_start(),
                       match.byte_end()});
    }
    return out;
}

std::vector<std::string> keys_of(const std::vector<Word>& found) {
    std::vector<std::string> out;
    out.reserve(found.size());
    for (const Word& word : found) {
        out.push_back(word.key);
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

const Json& mapping(const Json& settings) {
    static const Json empty = Json::object();
    return settings.is_object() ? settings : empty;
}

std::string trigger_problem(std::string_view raw_trigger) {
    const std::string trigger = py::strip(raw_trigger);
    if (trigger.empty()) {
        return "Add a trigger phrase to say.";
    }
    const std::string key = normalize(trigger);
    if (key.empty() || placeholder_pattern().search(trigger)) {
        return "Use words you can say for the trigger.";
    }
    if (py::len(key) < MIN_TRIGGER_CHARS || py::len(trigger) > MAX_TRIGGER_CHARS) {
        return std::format("Use {} to {} characters for the trigger.", MIN_TRIGGER_CHARS,
                           MAX_TRIGGER_CHARS);
    }
    return {};
}

std::string text_problem(std::string_view text) {
    if (py::strip(text).empty()) {
        return "Add the text to insert.";
    }
    if (py::len(text) > MAX_TEXT_CHARS) {
        // Python's {:,} grouping of MAX_TEXT_CHARS, spelled out.
        static_assert(MAX_TEXT_CHARS == 10'000, "the sentence below spells MAX_TEXT_CHARS");
        return "Keep the text to 10,000 characters or fewer.";
    }
    return {};
}

void store(Json& settings, const std::vector<Snippet>& snippets) {
    Json saved = Json::array();
    for (const Snippet& snippet : snippets) {
        saved.push_back(snippet_to_json(snippet));
    }
    settings[std::string(keys::dictation_snippets.key())] = std::move(saved);
}

struct Keyed {
    const Snippet* snippet;
    std::vector<std::string> words;
};

// _triggers_by_first_word: triggers grouped by their first word, most words first, then the
// longest (in code points), then in list order.
std::unordered_map<std::string, std::vector<const Keyed*>>
triggers_by_first_word(const std::vector<Keyed>& keyed) {
    std::vector<std::size_t> order(keyed.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::vector<std::size_t> chars(keyed.size());
    for (std::size_t i = 0; i < keyed.size(); ++i) {
        chars[i] = py::len(join(keyed[i].words, " "));
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (keyed[a].words.size() != keyed[b].words.size()) {
            return keyed[a].words.size() > keyed[b].words.size();
        }
        return chars[a] > chars[b];
    });
    std::unordered_map<std::string, std::vector<const Keyed*>> by_first_word;
    for (const std::size_t position : order) {
        by_first_word[keyed[position].words.front()].push_back(&keyed[position]);
    }
    return by_first_word;
}

bool contiguous(std::string_view text, const std::vector<Word>& found, std::size_t index,
                std::size_t count) {
    for (std::size_t i = index; i + 1 < index + count; ++i) {
        const std::string_view gap = text.substr(found[i].end, found[i + 1].start - found[i].end);
        if (!word_gap().fullmatch(gap)) {
            return false;
        }
    }
    return true;
}

// _trigger_at: the first candidate spoken at `index`, or nullptr.
const Keyed* trigger_at(std::string_view text, const std::vector<Word>& found,
                        const std::vector<std::string>& spoken, std::size_t index,
                        const std::vector<const Keyed*>& candidates) {
    for (const Keyed* candidate : candidates) {
        const std::size_t count = candidate->words.size();
        if (index + count > spoken.size()) {
            continue;
        }
        if (std::equal(candidate->words.begin(), candidate->words.end(),
                       spoken.begin() + static_cast<std::ptrdiff_t>(index)) &&
            contiguous(text, found, index, count)) {
            return candidate;
        }
    }
    return nullptr;
}

std::int64_t placeholder_number(const py::Match& match) {
    // \d is Unicode, as int() of it is.
    return py::py_int(match.group(1)).value_or(-1);
}

} // namespace

Json snippet_to_json(const Snippet& snippet) {
    Json saved = Json::object();
    saved["id"] = snippet.id;
    saved["trigger"] = snippet.trigger;
    saved["text"] = snippet.text;
    saved["formatted"] = snippet.formatted;
    return saved;
}

std::string normalize(std::string_view text) {
    return join(keys_of(words(text)), " ");
}

std::vector<Snippet> load_snippets(const Json& settings) {
    const Json raw = keys::dictation_snippets.read(mapping(settings));
    std::vector<Snippet> snippets;
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> triggers;
    for (const Json& item : raw) {
        if (snippets.size() >= MAX_SNIPPETS) {
            break;
        }
        if (!item.is_object()) {
            continue;
        }
        const auto id_field = item.find("id");
        const auto trigger_field = item.find("trigger");
        const auto text_field = item.find("text");
        if (id_field == item.end() || trigger_field == item.end() || text_field == item.end() ||
            !id_field->is_string() || !trigger_field->is_string() || !text_field->is_string()) {
            continue;
        }
        std::string snippet_id = py::strip(id_field->get_ref<const std::string&>());
        std::string trigger = py::strip(trigger_field->get_ref<const std::string&>());
        const std::string& text = text_field->get_ref<const std::string&>();
        std::string key = normalize(trigger);
        if (snippet_id.empty() || ids.contains(snippet_id) || triggers.contains(key) ||
            !trigger_problem(trigger).empty() || !text_problem(text).empty()) {
            continue;
        }
        ids.insert(snippet_id);
        triggers.insert(std::move(key));
        const auto formatted = item.find("formatted");
        const bool is_formatted =
            formatted != item.end() && formatted->is_boolean() && formatted->get<bool>();
        snippets.push_back({std::move(snippet_id), std::move(trigger), text, is_formatted});
    }
    return snippets;
}

std::string new_snippet_id() {
    return uuid4_hex();
}

std::optional<Snippet> find_duplicate(std::string_view trigger,
                                      const std::vector<Snippet>& snippets,
                                      std::string_view exclude_id) {
    const std::string key = normalize(trigger);
    if (key.empty()) {
        return std::nullopt;
    }
    for (const Snippet& snippet : snippets) {
        if (snippet.id != exclude_id && normalize(snippet.trigger) == key) {
            return snippet;
        }
    }
    return std::nullopt;
}

std::string trigger_note(std::string_view trigger, const std::vector<Snippet>& snippets,
                         std::string_view exclude_id) {
    const std::string key = normalize(trigger);
    if (key.empty()) {
        return {};
    }
    if (find_duplicate(trigger, snippets, exclude_id)) {
        return "Another snippet already uses this trigger.";
    }
    const std::string padded = " " + key + " ";
    std::vector<const Snippet*> others;
    for (const Snippet& snippet : snippets) {
        if (snippet.id != exclude_id) {
            others.push_back(&snippet);
        }
    }
    for (const Snippet* other : others) {
        const std::string other_padded = " " + normalize(other->trigger) + " ";
        if (padded.find(other_padded) != std::string::npos) {
            return std::format("Includes “{}”. Saying the full phrase inserts this "
                               "snippet.",
                               other->trigger);
        }
        if (other_padded.find(padded) != std::string::npos) {
            return std::format("Also part of “{}”, which wins when you say it in full.",
                               other->trigger);
        }
    }
    for (const Snippet* other : others) {
        if (py::SequenceMatcher(key, normalize(other->trigger)).ratio() >= 0.85) {
            return std::format("Sounds close to “{}”; speech might mix them up.", other->trigger);
        }
    }
    if (key.find(' ') == std::string::npos) {
        return "A single word can come up in everyday speech; a short phrase is safer.";
    }
    return {};
}

void validate_snippet(const Snippet& snippet, const Json& settings) {
    std::string problem = trigger_problem(snippet.trigger);
    if (problem.empty()) {
        problem = text_problem(snippet.text);
    }
    if (!problem.empty()) {
        throw py::ValueError(problem);
    }
    const std::vector<Snippet> existing = load_snippets(settings);
    if (find_duplicate(snippet.trigger, existing, snippet.id)) {
        throw py::ValueError(
            "Another snippet already uses this trigger. Choose a different phrase.");
    }
    const bool known = std::any_of(existing.begin(), existing.end(),
                                   [&](const Snippet& s) { return s.id == snippet.id; });
    if (existing.size() >= MAX_SNIPPETS && !known) {
        throw py::ValueError(std::format("You have {} snippets, the most {} keeps. Delete one to "
                                         "add another.",
                                         MAX_SNIPPETS, identity().product));
    }
}

void save_snippet(Json& settings, const Snippet& snippet) {
    validate_snippet(snippet, settings);
    const Snippet saved{snippet.id, py::strip(snippet.trigger), snippet.text, snippet.formatted};
    std::vector<Snippet> snippets = load_snippets(settings);
    const auto found = std::find_if(snippets.begin(), snippets.end(),
                                    [&](const Snippet& s) { return s.id == saved.id; });
    if (found == snippets.end()) {
        snippets.push_back(saved);
    } else {
        *found = saved;
    }
    store(settings, snippets);
}

void delete_snippet(Json& settings, std::string_view snippet_id) {
    std::vector<Snippet> kept;
    for (Snippet& snippet : load_snippets(settings)) {
        if (snippet.id != snippet_id) {
            kept.push_back(std::move(snippet));
        }
    }
    store(settings, kept);
}

SnippetPlan plan_expansion(std::string_view text, const std::vector<Snippet>& snippets) {
    SnippetPlan plan;
    plan.text = std::string(text);
    std::vector<Keyed> keyed;
    for (const Snippet& snippet : snippets) {
        std::vector<std::string> trigger_words = keys_of(words(snippet.trigger));
        if (!trigger_words.empty()) {
            keyed.push_back({&snippet, std::move(trigger_words)});
        }
    }
    const std::vector<Word> found = words(text);
    if (keyed.empty() || found.empty()) {
        return plan;
    }
    const std::vector<std::string> spoken = keys_of(found);
    for (const Keyed& candidate : keyed) {
        if (spoken == candidate.words) {
            plan.whole = *candidate.snippet;
            return plan;
        }
    }
    if (placeholder_pattern().search(text)) {
        // Already holds something that reads as a placeholder; expanding would replace the
        // wrong text.
        return plan;
    }

    const auto by_first_word = triggers_by_first_word(keyed);
    static const std::vector<const Keyed*> none;
    std::string out;
    std::size_t cursor = 0;
    std::size_t index = 0;
    while (index < found.size()) {
        const auto group = by_first_word.find(spoken[index]);
        const Keyed* match = trigger_at(text, found, spoken, index,
                                        group == by_first_word.end() ? none : group->second);
        if (match == nullptr) {
            ++index;
            continue;
        }
        const std::size_t count = match->words.size();
        std::string token = placeholder(plan.placeholders.size() + 1);
        out.append(text.substr(cursor, found[index].start - cursor));
        out += token;
        plan.placeholders.emplace_back(std::move(token), *match->snippet);
        cursor = found[index + count - 1].end;
        index += count;
    }
    if (plan.placeholders.empty()) {
        return plan;
    }
    out.append(text.substr(cursor));
    plan.text = std::move(out);
    return plan;
}

std::string prompt_guard(const SnippetPlan& plan) {
    std::vector<std::string> tokens;
    for (const auto& [token, snippet] : plan.placeholders) {
        tokens.push_back(token);
    }
    if (tokens.empty()) {
        return {};
    }
    if (tokens.size() == 1) {
        return std::format("The transcript contains the placeholder {}, which stands for text "
                           "inserted later; keep it exactly once, unchanged and in its place.",
                           tokens.front());
    }
    const std::string last = tokens.back();
    tokens.pop_back();
    return std::format("The transcript contains the placeholders {} and {}, each standing for "
                       "text inserted later; keep every one exactly once, unchanged and in its "
                       "place.",
                       join(tokens, ", "), last);
}

Expansion expand(std::string_view text, const SnippetPlan& plan) {
    if (plan.whole) {
        return {plain_text(*plan.whole), snippet_html(*plan.whole), true};
    }
    if (plan.placeholders.empty()) {
        return {std::string(text), {}, true};
    }
    std::map<std::int64_t, const Snippet*> by_number;
    for (const auto& [token, snippet] : plan.placeholders) {
        if (const auto match = placeholder_pattern().fullmatch(token)) {
            by_number[placeholder_number(*match)] = &snippet;
        }
    }
    const std::vector<py::Match> matches = placeholder_pattern().finditer(text);
    std::vector<std::int64_t> numbers;
    for (const py::Match& match : matches) {
        numbers.push_back(placeholder_number(match));
    }
    std::sort(numbers.begin(), numbers.end());
    std::vector<std::int64_t> expected;
    for (const auto& [number, snippet] : by_number) {
        expected.push_back(number);
    }
    if (numbers != expected || by_number.size() != plan.placeholders.size()) {
        return {std::string(text), {}, false};
    }

    const bool rich = std::any_of(plan.placeholders.begin(), plan.placeholders.end(),
                                  [](const auto& entry) { return entry.second.formatted; });
    Expansion out;
    std::size_t cursor = 0;
    for (const py::Match& match : matches) {
        const Snippet& snippet = *by_number.at(placeholder_number(match));
        const std::string_view before = text.substr(cursor, match.byte_start() - cursor);
        out.plain.append(before);
        out.plain += plain_text(snippet);
        if (rich) {
            out.html += escape_lines(before);
            const std::string html = snippet_html(snippet);
            out.html += html.empty() ? escape_lines(snippet.text) : html;
        }
        cursor = match.byte_end();
    }
    const std::string_view rest = text.substr(cursor);
    out.plain.append(rest);
    if (rich) {
        out.html += escape_lines(rest);
    }
    out.ok = true;
    return out;
}

std::string plain_text(const Snippet& snippet) {
    return snippet.formatted ? markdown_to_plain(snippet.text) : snippet.text;
}

std::string snippet_html(const Snippet& snippet) {
    return snippet.formatted ? markdown_to_html(snippet.text) : std::string();
}

} // namespace fv::text
