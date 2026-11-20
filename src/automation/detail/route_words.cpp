#include "automation/detail/route_words.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <unordered_set>
#include <utility>

#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::automation::detail {

namespace {

// A word, with apostrophes and hyphens inside it ("don't", "to-do").
const py::Regex& word_pattern() {
    static const py::Regex pattern(R"([^\W_]+(?:['’\-‐][^\W_]+)*)");
    return pattern;
}

// Punctuation between two words that marks a pause ("To do, email …").
const py::Regex& pause_pattern() {
    static const py::Regex pattern("[,.;:!?…—–-]");
    return pattern;
}

// Punctuation that ends a sentence, so the next word's capital says nothing.
const py::Regex& sentence_end_pattern() {
    static const py::Regex pattern("[.!?…]");
    return pattern;
}

bool contains(std::u32string_view set, char32_t cp) {
    return set.find(cp) != std::u32string_view::npos;
}

// str.replace: every non-overlapping occurrence, left to right.
void replace_all(std::u32string& text, std::u32string_view from, std::u32string_view to) {
    std::u32string out;
    std::size_t at = 0;
    while (true) {
        const std::size_t found = text.find(from, at);
        if (found == std::u32string::npos) {
            out.append(text, at);
            break;
        }
        out.append(text, at, found - at);
        out.append(to);
        at = found + from.size();
    }
    text = std::move(out);
}

// Vowel sounds by spelling, longest spellings first. Claude's "au", cloud's "ou", clod's "o"
// and clawed's "aw" are one open sound; clued's "ue" isn't.
struct VowelSound {
    std::array<std::u32string_view, 6> spellings;
    std::u32string_view sound;
};
constexpr std::array<VowelSound, 5> kVowelSounds = {{
    {{U"ai", U"ay", U"ei", U"ey"}, U"ay"},
    {{U"oi", U"oy"}, U"oy"},
    {{U"oo", U"ew", U"eu", U"ue", U"ui", U"u"}, U"u"},
    {{U"a", U"o"}, U"o"},
    {{U"e", U"i", U"y"}, U"e"},
}};

using WordSet = std::unordered_set<std::string_view>;

// Said before addressing anything; skipped at the start of an utterance.
const WordSet& fillers() {
    static const WordSet words = {
        "hey",    "hi",      "hello", "yo",  "ok",  "okay", "so",   "um",  "umm",
        "uh",     "uhh",     "uhm",   "er",  "erm", "ah",   "oh",   "hmm", "well",
        "please", "alright", "right", "now", "and", "like", "yeah", "yes",
    };
    return words;
}

// Words that may come between the fillers and an action's name ("can you ask Claude", "send
// this to my scripts"), and between a request and a name said last. Never "the": a name after
// it is an ordinary noun ("send the scripts to Bob").
const WordSet& lead_ins() {
    static const WordSet words = {
        "ask",  "tell", "send", "message", "ping", "use",  "open", "start",  "add",   "put",
        "save", "post", "note", "have",    "let",  "lets", "can",  "could",  "would", "will",
        "you",  "this", "that", "it",      "to",   "into", "for",  "my",     "a",     "an",
        "new",  "with", "via",  "using",   "in",   "on",   "hey",  "please", "go",    "and",
    };
    return words;
}

// Generic last words of an action name ("Tell my Claude Code session"), dropped so that saying
// "Claude Code" still names it.
const WordSet& name_tails() {
    static const WordSet words = {
        "session", "sessions", "chat", "app",    "action", "hotkey",
        "hook",    "webhook",  "list", "window", "tab",    "please",
    };
    return words;
}

// Words that carry an ordinary phrase on ("to do that"): pronouns, determiners, prepositions,
// conjunctions and a few adverbs. Before one of them, a name heard as several words ("to do"
// for Todo) is just those words. Not question words, "can", "please" or a verb: they start the
// request ("chat GPT what's …", "to do buy milk").
const WordSet& phrase_goes_on() {
    static const WordSet words = {
        "it",        "its",        "this",    "that",    "thats",    "these",    "those",
        "them",      "him",        "her",     "me",      "us",       "so",       "something",
        "anything",  "everything", "nothing", "one",     "all",      "both",     "each",
        "either",    "some",       "any",     "more",    "most",     "less",     "much",
        "many",      "enough",     "such",    "other",   "another",  "else",     "the",
        "a",         "an",         "my",      "your",    "our",      "their",    "his",
        "no",        "every",      "about",   "with",    "without",  "for",      "from",
        "in",        "on",         "at",      "by",      "to",       "of",       "into",
        "onto",      "after",      "before",  "over",    "under",    "around",   "through",
        "during",    "like",       "than",    "as",      "between",  "against",  "up",
        "down",      "out",        "off",     "away",    "back",     "and",      "or",
        "but",       "if",         "because", "then",    "while",    "unless",   "until",
        "though",    "although",   "since",   "whether", "well",     "better",   "right",
        "now",       "later",      "again",   "too",     "also",     "just",     "only",
        "even",      "here",       "there",   "today",   "tonight",  "tomorrow", "first",
        "next",      "already",    "anyway",  "instead", "together", "properly", "differently",
        "otherwise",
    };
    return words;
}

// Words that only link a name to the request ("Claude, um, can you …", "ask Claude to …").
const WordSet& link_words() {
    static const WordSet words = [] {
        WordSet all = fillers();
        for (std::string_view word :
             {"to", "that", "about", "can", "could", "would", "will", "you"}) {
            all.insert(word);
        }
        return all;
    }();
    return words;
}

bool all_tails(std::span<const std::string> words) {
    return std::ranges::all_of(words,
                               [](const std::string& word) { return name_tails().contains(word); });
}

std::string joined(std::span<const std::string> words) {
    std::string out;
    for (const std::string& word : words) {
        out += word;
    }
    return out;
}

const py::Regex& polite_end_pattern() {
    static const py::Regex pattern(R"([\s,;:]*\b(?:please|thanks|thank you)\b\s*([.!?]*)\s*$)",
                                   py::IGNORECASE);
    return pattern;
}
constexpr std::string_view kTrailingJunk = " \t\r\n,;:-–—";

std::u32string_view vowel_of(std::u32string_view nucleus) {
    for (const VowelSound& entry : kVowelSounds) {
        for (std::u32string_view spelling : entry.spellings) {
            if (!spelling.empty() && nucleus.starts_with(spelling)) {
                return entry.sound;
            }
        }
    }
    return nucleus;
}

} // namespace

std::vector<std::string> word_tokens(std::string_view text) {
    std::vector<std::string> tokens;
    for (const py::Match& match : word_pattern().finditer(text)) {
        tokens.push_back(match.group());
    }
    return tokens;
}

std::string norm(std::string_view token) {
    const std::string folded = py::casefold(token);
    std::string out;
    out.reserve(folded.size());
    std::size_t at = 0;
    while (at < folded.size()) {
        const std::size_t start = at;
        const char32_t cp = py::next_code_point(folded, at);
        if (cp == U'\'' || cp == U'\u2019' || cp == U'-' || cp == U'\u2010') {
            continue;
        }
        out.append(folded, start, at - start);
    }
    return out;
}

bool written_as_name(std::string_view token, bool sentence_start) {
    std::size_t at = 0;
    bool first = true;
    while (at < token.size()) {
        const char32_t cp = py::next_code_point(token, at);
        if (first) {
            first = false;
            if (!sentence_start && py::is_upper(cp)) {
                return true;
            }
            continue;
        }
        if (py::is_upper(cp)) {
            return true;
        }
    }
    return false;
}

std::vector<RouteWord> route_words(std::string_view text) {
    const std::vector<py::Match> found = word_pattern().finditer(text);
    std::vector<RouteWord> words;
    words.reserve(found.size());
    for (std::size_t i = 0; i < found.size(); ++i) {
        const py::Match& match = found[i];
        RouteWord word;
        const std::string token = match.group();
        word.norm = norm(token);
        word.start = match.start();
        word.end = match.end();
        word.byte_start = match.byte_start();
        word.byte_end = match.byte_end();
        if (i + 1 == found.size()) {
            word.pause = true;
        } else {
            const std::size_t gap_end = found[i + 1].byte_start();
            word.pause = pause_pattern()
                             .search(text.substr(match.byte_end(), gap_end - match.byte_end()))
                             .has_value();
        }
        bool sentence_start = i == 0;
        if (!sentence_start) {
            const std::size_t gap_start = found[i - 1].byte_end();
            sentence_start = sentence_end_pattern()
                                 .search(text.substr(gap_start, match.byte_start() - gap_start))
                                 .has_value();
        }
        word.name = written_as_name(token, sentence_start);
        words.push_back(std::move(word));
    }
    return words;
}

std::optional<WordSound> word_sound(std::string_view word) {
    std::u32string w = py::to_code_points(word);
    replace_all(w, U"ph", U"f");
    replace_all(w, U"ck", U"k");
    replace_all(w, U"x", U"ks");
    replace_all(w, U"q", U"k");
    replace_all(w, U"c", U"k");
    replace_all(w, U"z", U"s");
    if (w.size() > 3 && w.back() == U'e' && !contains(U"aeiouy", w[w.size() - 2])) {
        w.pop_back(); // a silent final e: "claude", "code"
    } else if (w.size() > 4 && w.ends_with(U"ed") && !contains(U"td", w[w.size() - 3])) {
        w.erase(w.size() - 2); // "clawed", "called": no syllable of its own
        w.push_back(U'd');
    }
    std::vector<std::pair<std::size_t, std::size_t>> groups;
    std::size_t i = 0;
    while (i < w.size()) {
        if (contains(U"aeiou", w[i]) || (w[i] == U'y' && i > 0)) {
            std::size_t j = i + 1;
            while (j < w.size() && contains(U"aeiouwy", w[j])) {
                ++j;
            }
            groups.emplace_back(i, j);
            i = j;
        } else {
            ++i;
        }
    }
    if (groups.empty()) {
        return std::nullopt;
    }
    const auto [first, after] = groups.front();
    const std::u32string_view nucleus = std::u32string_view(w).substr(first, after - first);
    std::u32string tail;
    for (std::size_t k = after; k < w.size(); ++k) {
        const char32_t cp = w[k];
        if (contains(U"aeiouyhw", cp) || (!tail.empty() && cp == tail.back())) {
            continue;
        }
        tail.push_back(cp);
    }
    WordSound sound;
    sound.onset = py::to_utf8(std::u32string_view(w).substr(0, first));
    sound.vowel = py::to_utf8(vowel_of(nucleus));
    sound.coda = py::to_utf8(tail);
    sound.syllables = static_cast<int>(groups.size());
    return sound;
}

bool alike(std::string_view heard, std::string_view name) {
    if (heard == name) {
        return true;
    }
    const auto heard_len = static_cast<long long>(py::len(heard));
    const auto name_len = static_cast<long long>(py::len(name));
    if (heard_len < 4 || name_len < 4 || std::llabs(heard_len - name_len) > 2) {
        return false;
    }
    const std::optional<WordSound> sound = word_sound(name);
    return sound && py::len(sound->onset) + py::len(sound->coda) >= 3 && word_sound(heard) == sound;
}

bool NameVariant::follows(const std::vector<std::string>& said,
                          std::span<const RouteWord> heard) const {
    if (said.empty() || after_lead_ins) {
        return true;
    }
    if (!dropped.empty()) {
        // all(word in rest for word in dropped) over one iterator: the dropped words in order,
        // anything between them allowed.
        std::size_t rest = 0;
        bool all = true;
        for (const std::string& word : dropped) {
            while (rest < said.size() && said[rest] != word) {
                ++rest;
            }
            if (rest == said.size()) {
                all = false;
                break;
            }
            ++rest;
        }
        if (all) {
            return true;
        }
    }
    return after_lead_ins_as_name &&
           std::ranges::any_of(heard, [](const RouteWord& word) { return word.name; });
}

std::vector<NameVariant> name_variants(std::string_view name,
                                       const std::vector<std::string>& spoken_names) {
    std::vector<NameVariant> variants;
    const auto add = [&](std::vector<std::string> form, std::vector<std::string> dropped,
                         bool after_lead_ins, bool as_name) {
        if (form.empty() || std::ranges::any_of(variants, [&](const NameVariant& seen) {
                return seen.words == form;
            })) {
            return;
        }
        variants.push_back(
            NameVariant{std::move(form), std::move(dropped), after_lead_ins, as_name});
    };

    std::vector<std::string_view> phrases{name};
    phrases.insert(phrases.end(), spoken_names.begin(), spoken_names.end());
    for (std::string_view phrase : phrases) {
        const std::vector<std::string> tokens = word_tokens(phrase);
        if (tokens.empty()) {
            continue;
        }
        std::vector<std::string> words;
        std::vector<bool> typed_as_name;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            words.push_back(norm(tokens[i]));
            typed_as_name.push_back(written_as_name(tokens[i], i == 0));
        }
        add(words, {}, !all_tails(words), false);
        std::size_t core = 0;
        while (core < words.size() - 1 && lead_ins().contains(words[core])) {
            ++core;
        }
        for (const std::size_t start : {std::size_t{0}, core}) {
            std::size_t end = words.size();
            while (end - start > 1 && name_tails().contains(words[end - 1])) {
                --end;
            }
            for (const std::size_t stop : {words.size(), end}) {
                const std::span<const std::string> form(words.data() + start, stop - start);
                // A shortened form must still be distinctive.
                if (py::len(joined(form)) < 3) {
                    continue;
                }
                const bool typed =
                    std::any_of(typed_as_name.begin() + static_cast<std::ptrdiff_t>(start),
                                typed_as_name.begin() + static_cast<std::ptrdiff_t>(stop),
                                [](bool value) { return value; });
                add(std::vector<std::string>(form.begin(), form.end()),
                    std::vector<std::string>(words.begin(),
                                             words.begin() + static_cast<std::ptrdiff_t>(start)),
                    typed, !all_tails(form));
            }
        }
    }
    return variants;
}

std::size_t skip_fillers(const std::vector<RouteWord>& words) {
    std::size_t i = 0;
    while (i < words.size()) {
        if (fillers().contains(words[i].norm)) {
            ++i;
            continue;
        }
        if (i + 1 < words.size() && words[i].norm == "all" && words[i + 1].norm == "right") {
            i += 2;
            continue;
        }
        break;
    }
    return i;
}

std::size_t match_len(const std::vector<RouteWord>& words, std::size_t at,
                      const std::vector<std::string>& variant) {
    const std::string target = joined(variant);
    std::string heard;
    for (std::size_t k = at; k < words.size(); ++k) {
        heard += words[k].norm;
        if (heard == target) {
            // The last word of a text always pauses, so words[k + 1] exists here.
            if (k - at + 1 > variant.size() && !words[k].pause &&
                phrase_goes_on().contains(words[k + 1].norm)) {
                break;
            }
            return k - at + 1;
        }
        if (!target.starts_with(heard)) {
            break;
        }
    }
    const std::size_t n = variant.size();
    if (at + n <= words.size()) {
        bool all = true;
        for (std::size_t i = 0; i < n && all; ++i) {
            all = alike(words[at + i].norm, variant[i]);
        }
        if (all) {
            return n;
        }
    }
    return 0;
}

std::size_t polite_tail(const std::vector<RouteWord>& words) {
    if (words.empty()) {
        return 0;
    }
    const std::string& last = words.back().norm;
    if (last == "please" || last == "thanks") {
        return 1;
    }
    return words.size() >= 2 && words[words.size() - 2].norm == "thank" && last == "you" ? 2 : 0;
}

bool is_lead_in(std::string_view norm) {
    return lead_ins().contains(norm);
}

bool is_link_word(std::string_view norm) {
    return link_words().contains(norm);
}

std::string without_polite_end(std::string_view span) {
    const std::string trimmed = py::rstrip(
        polite_end_pattern().sub([](const py::Match& match) { return match.group(1); }, span),
        kTrailingJunk);
    return route_words(trimmed).empty() ? std::string(span) : trimmed;
}

std::string clip(std::string_view text, std::size_t head, std::size_t tail) {
    if (py::len(text) <= head + tail + 3) {
        return std::string(text);
    }
    return py::slice(text, 0, static_cast<std::int64_t>(head)) + " … " +
           py::slice(text, -static_cast<std::int64_t>(tail));
}

std::string sentence_start(std::string_view span) {
    std::size_t at = 0;
    if (span.empty()) {
        return std::string(span);
    }
    const char32_t first = py::next_code_point(span, at);
    const std::size_t first_end = at;
    bool second_upper = false;
    if (at < span.size()) {
        second_upper = py::is_upper(py::next_code_point(span, at));
    }
    if (py::is_lower(first) && !second_upper) {
        return py::upper(span.substr(0, first_end)) + std::string(span.substr(first_end));
    }
    return std::string(span);
}

} // namespace fv::automation::detail
