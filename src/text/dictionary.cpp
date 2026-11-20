#include "text/dictionary.h"

#include <format>
#include <set>
#include <unordered_set>
#include <utility>

#include "base/hash.h"
#include "base/ids.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "text/detail/cleanup_rules.h"
#include "text/vocabulary.h"

namespace fv::text {

namespace {

const Json& mapping(const Json& settings) {
    static const Json empty = Json::object();
    return settings.is_object() ? settings : empty;
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

// _clean: whitespace collapsed to single spaces; "" for anything but a string.
std::string clean(std::string_view value) {
    return join(py::split(value), " ");
}

std::string clean_value(const Json& value) {
    return value.is_string() ? clean(value.get_ref<const std::string&>()) : std::string();
}

// `item.get(key) is True`.
bool is_true(const Json& item, std::string_view key) {
    const auto found = item.find(std::string(key));
    return found != item.end() && found->is_boolean() && found->get<bool>();
}

std::string stable_id(std::string_view term) {
    // Entries saved without an id (hand edits) keep the same id on every load.
    return "t" + sha1_hex(py::casefold(term)).substr(0, 11);
}

std::string new_id() {
    return uuid4_hex().substr(0, 12);
}

// _variants over values already passed through _clean: distinct (ignoring case), not the term,
// at most MAX_TERM_CHARS each, the first MAX_HEARD.
std::vector<std::string> variants(const std::vector<std::string>& values, std::string_view term) {
    std::vector<std::string> found;
    std::unordered_set<std::string> seen{py::casefold(term)};
    for (const std::string& variant : values) {
        if (variant.empty() || py::len(variant) > MAX_TERM_CHARS) {
            continue;
        }
        if (!seen.insert(py::casefold(variant)).second) {
            continue;
        }
        found.push_back(variant);
        if (found.size() >= MAX_HEARD) {
            break;
        }
    }
    return found;
}

std::vector<std::string> variants(const Json& raw, std::string_view term) {
    std::vector<std::string> values;
    if (raw.is_string()) {
        values.push_back(clean_value(raw));
    } else if (raw.is_array()) {
        for (const Json& value : raw) {
            values.push_back(clean_value(value));
        }
    } else {
        return {};
    }
    return variants(values, term);
}

std::vector<DictionaryTerm> ranked(const std::vector<DictionaryTerm>& terms) {
    std::vector<DictionaryTerm> out;
    out.reserve(terms.size());
    for (const DictionaryTerm& term : terms) {
        if (term.starred) {
            out.push_back(term);
        }
    }
    for (const DictionaryTerm& term : terms) {
        if (!term.starred) {
            out.push_back(term);
        }
    }
    return out;
}

void store(Json& settings, const std::vector<DictionaryTerm>& terms) {
    Json saved = Json::array();
    for (const DictionaryTerm& term : terms) {
        saved.push_back(term_to_json(term));
    }
    settings[std::string(keys::dictation_dictionary.key())] = std::move(saved);
}

std::string checked_term(std::string_view term) {
    std::string text = clean(term);
    if (text.empty()) {
        throw DictionaryError("Type a word or name first.");
    }
    if (py::len(text) > MAX_TERM_CHARS) {
        throw DictionaryError(std::format("Keep words under {} characters.", MAX_TERM_CHARS));
    }
    return text;
}

std::vector<std::string> checked_heard(const std::vector<std::string>& heard,
                                       std::string_view term) {
    std::vector<std::string> cleaned;
    cleaned.reserve(heard.size());
    for (const std::string& variant : heard) {
        cleaned.push_back(clean(variant));
    }
    for (const std::string& variant : cleaned) {
        if (py::len(variant) > MAX_TERM_CHARS) {
            throw DictionaryError(
                std::format("Keep each “sounds like” under {} characters.", MAX_TERM_CHARS));
        }
    }
    std::set<std::string> distinct;
    for (const std::string& variant : cleaned) {
        if (!variant.empty()) {
            distinct.insert(py::casefold(variant));
        }
    }
    distinct.erase(py::casefold(term));
    if (distinct.size() > MAX_HEARD) {
        throw DictionaryError(std::format("Up to {} “sounds like” spellings per word.", MAX_HEARD));
    }
    return variants(cleaned, term);
}

std::size_t index_of(const std::vector<DictionaryTerm>& terms, std::string_view term_id) {
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (terms[i].id == term_id) {
            return i;
        }
    }
    throw DictionaryError("That word is no longer in your dictionary.");
}

// _index_of_text: the entry spelled `text` ignoring case, other than `skip`, or -1.
std::ptrdiff_t index_of_text(const std::vector<DictionaryTerm>& terms, std::string_view text,
                             std::string_view skip = {}) {
    const std::string key = py::casefold(text);
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (terms[i].id != skip && py::casefold(terms[i].term) == key) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

} // namespace

Json term_to_json(const DictionaryTerm& term) {
    Json saved = Json::object();
    saved["id"] = term.id;
    saved["term"] = term.term;
    saved["starred"] = term.starred;
    saved["heard"] = term.heard;
    saved["learned"] = term.learned;
    saved["new"] = term.is_new;
    if (term.is_protected) {
        saved["protected"] = true;
    }
    return saved;
}

std::vector<DictionaryTerm> parse_terms(const Json& raw) {
    if (!raw.is_array()) {
        return {};
    }
    static const Json missing;
    std::vector<DictionaryTerm> terms;
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> ids;
    for (const Json& item : raw) {
        if (terms.size() >= MAX_TERMS) {
            break;
        }
        if (!item.is_object()) {
            continue;
        }
        const auto field = [&item](std::string_view key) -> const Json& {
            const auto found = item.find(std::string(key));
            return found == item.end() ? missing : *found;
        };
        std::string term = clean_value(field("term"));
        if (term.empty() || py::len(term) > MAX_TERM_CHARS) {
            continue;
        }
        std::string folded = py::casefold(term);
        if (seen.contains(folded)) {
            continue;
        }
        const bool learned = is_true(item, "learned");
        const Json& saved_id = field("id");
        std::string term_id;
        if (saved_id.is_string()) {
            term_id = py::strip(saved_id.get_ref<const std::string&>());
        }
        if (term_id.empty()) {
            term_id = stable_id(term);
        }
        while (ids.contains(term_id)) {
            term_id = stable_id(term_id + term);
        }
        seen.insert(std::move(folded));
        ids.insert(term_id);
        DictionaryTerm parsed;
        parsed.id = std::move(term_id);
        parsed.starred = is_true(item, "starred");
        // Learned terms never replace text: a correction seen twice is not proof that every
        // occurrence of the old word was wrong.
        if (!learned) {
            parsed.heard = variants(field("heard"), term);
        }
        parsed.term = std::move(term);
        parsed.learned = learned;
        parsed.is_new = learned && is_true(item, "new");
        parsed.is_protected = is_true(item, "protected");
        terms.push_back(std::move(parsed));
    }
    return terms;
}

std::vector<DictionaryTerm> load_dictionary(const Json& settings) {
    const Json& snapshot = mapping(settings);
    const auto found = snapshot.find(std::string(keys::dictation_dictionary.key()));
    return found == snapshot.end() ? std::vector<DictionaryTerm>{} : parse_terms(*found);
}

std::vector<std::string> recognition_phrases(const Json& settings) {
    if (!keys::dictionary_steer_recognition.read(mapping(settings))) {
        return {};
    }
    std::vector<std::string> phrases;
    for (const DictionaryTerm& term : ranked(load_dictionary(settings))) {
        if (phrases.size() >= MAX_RECOGNITION_PHRASES) {
            break;
        }
        phrases.push_back(term.term);
    }
    return phrases;
}

std::string apply_replacements(std::string_view text, const std::vector<DictionaryTerm>& terms) {
    // replace_terms keeps the first rule of each folded variant, which is Python's setdefault
    // over the ranked terms.
    std::vector<std::pair<std::string, std::string>> rules;
    for (const DictionaryTerm& term : ranked(terms)) {
        if (term.learned) {
            continue;
        }
        for (const std::string& variant : term.heard) {
            rules.emplace_back(variant, term.term);
        }
    }
    return replace_terms(text, rules);
}

std::string prompt_block(const std::vector<DictionaryTerm>& terms) {
    const std::vector<DictionaryTerm> order = ranked(terms);
    std::vector<std::string> names;
    std::size_t used = 0;
    for (const DictionaryTerm& term : order) {
        const std::size_t cost = py::len(term.term) + 2;
        if (names.size() >= PROMPT_MAX_TERMS || used + cost > PROMPT_MAX_CHARS) {
            break;
        }
        names.push_back(term.term);
        used += cost;
    }
    if (names.empty()) {
        return {};
    }
    std::string block = "Vocabulary — when one of these is what was said, including "
                        "near-miss mishearings, spell it exactly: " +
                        join(names, ", ") + ".";
    // The protected list comes from the whole ranked list, so a protected word past the
    // vocabulary budget is still named.
    std::vector<std::string> kept;
    for (const DictionaryTerm& term : order) {
        if (kept.size() >= PROMPT_MAX_PROTECTED) {
            break;
        }
        if (term.is_protected) {
            kept.push_back(term.term);
        }
    }
    if (!kept.empty()) {
        block += " The speaker put these back after an earlier cleanup changed them; never "
                 "replace or respell them: " +
                 join(kept, ", ") + ".";
    }
    return block;
}

std::string summary(const Json& settings) {
    const std::vector<DictionaryTerm> terms = load_dictionary(settings);
    if (terms.empty()) {
        return "No words yet";
    }
    std::string text = std::format("{} word{}", terms.size(), terms.size() == 1 ? "" : "s");
    std::size_t fresh = 0;
    for (const DictionaryTerm& term : terms) {
        fresh += term.is_new ? 1 : 0;
    }
    return fresh > 0 ? std::format("{} · {} new", text, fresh) : text;
}

std::vector<std::string> parse_heard(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',' || text[i] == ';' || text[i] == '\n') {
            std::string part = clean(text.substr(start, i - start));
            if (!part.empty()) {
                parts.push_back(std::move(part));
            }
            start = i + 1;
        }
    }
    return parts;
}

DictionaryTerm add_term(Json& settings, std::string_view term,
                        const std::vector<std::string>& heard, bool starred) {
    const std::string text = checked_term(term);
    const std::vector<std::string> new_variants = checked_heard(heard, text);
    std::vector<DictionaryTerm> terms = load_dictionary(settings);
    const std::ptrdiff_t index = index_of_text(terms, text);
    DictionaryTerm updated;
    if (index >= 0) {
        const DictionaryTerm& existing = terms[static_cast<std::size_t>(index)];
        std::vector<std::string> merged = existing.heard;
        merged.insert(merged.end(), new_variants.begin(), new_variants.end());
        updated = existing;
        updated.term = text;
        updated.heard = checked_heard(merged, text);
        updated.learned = false;
        updated.is_new = false;
        updated.starred = existing.starred || starred;
        if (updated == existing) {
            throw DictionaryError("That's already in your dictionary.");
        }
        terms[static_cast<std::size_t>(index)] = updated;
    } else {
        if (terms.size() >= MAX_TERMS) {
            throw DictionaryError(std::format("Your dictionary is full ({} words).", MAX_TERMS));
        }
        updated.id = new_id();
        updated.term = text;
        updated.starred = starred;
        updated.heard = new_variants;
        terms.insert(terms.begin(), updated);
    }
    store(settings, terms);
    return updated;
}

DictionaryTerm update_term(Json& settings, std::string_view term_id, std::string_view term,
                           const std::vector<std::string>& heard) {
    const std::string text = checked_term(term);
    std::vector<std::string> new_variants = checked_heard(heard, text);
    std::vector<DictionaryTerm> terms = load_dictionary(settings);
    const std::size_t index = index_of(terms, term_id);
    if (index_of_text(terms, text, term_id) >= 0) {
        throw DictionaryError("That's already in your dictionary.");
    }
    DictionaryTerm& updated = terms[index];
    updated.term = text;
    updated.heard = std::move(new_variants);
    updated.learned = false;
    updated.is_new = false;
    DictionaryTerm result = updated;
    store(settings, terms);
    return result;
}

DictionaryTerm set_starred(Json& settings, std::string_view term_id, bool starred) {
    std::vector<DictionaryTerm> terms = load_dictionary(settings);
    const std::size_t index = index_of(terms, term_id);
    terms[index].starred = starred;
    terms[index].is_new = false;
    DictionaryTerm result = terms[index];
    store(settings, terms);
    return result;
}

std::optional<DictionaryTerm> delete_term(Json& settings, std::string_view term_id) {
    std::vector<DictionaryTerm> terms = load_dictionary(settings);
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (terms[i].id == term_id) {
            DictionaryTerm removed = std::move(terms[i]);
            terms.erase(terms.begin() + static_cast<std::ptrdiff_t>(i));
            store(settings, terms);
            return removed;
        }
    }
    return std::nullopt;
}

std::optional<DictionaryTerm> undo_learned(Json& settings, std::string_view term_id) {
    std::vector<DictionaryTerm> terms = load_dictionary(settings);
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (terms[i].id == term_id && terms[i].learned) {
            DictionaryTerm removed = std::move(terms[i]);
            terms.erase(terms.begin() + static_cast<std::ptrdiff_t>(i));
            store(settings, terms);
            return removed;
        }
    }
    return std::nullopt;
}

std::size_t clear_new(Json& settings) {
    std::vector<DictionaryTerm> terms = load_dictionary(settings);
    std::size_t fresh = 0;
    for (DictionaryTerm& term : terms) {
        fresh += term.is_new ? 1 : 0;
        term.is_new = false;
    }
    if (fresh > 0) {
        store(settings, terms);
    }
    return fresh;
}

std::optional<DictionaryTerm> add_learned(Json& settings, std::string_view word,
                                          bool is_protected) {
    if (!keys::dictionary_learn_enabled.read(mapping(settings))) {
        return std::nullopt;
    }
    const std::string text = clean(word);
    if (text.empty() || py::len(text) > MAX_TERM_CHARS) {
        return std::nullopt;
    }
    std::vector<DictionaryTerm> terms = load_dictionary(settings);
    const std::ptrdiff_t index = index_of_text(terms, text);
    if (index >= 0) {
        DictionaryTerm& known = terms[static_cast<std::size_t>(index)];
        if (!is_protected || known.is_protected) {
            return std::nullopt;
        }
        known.is_protected = true;
        DictionaryTerm result = known;
        store(settings, terms);
        return result;
    }
    if (terms.size() >= MAX_TERMS) {
        return std::nullopt;
    }
    DictionaryTerm learned;
    learned.id = new_id();
    learned.term = text;
    learned.learned = true;
    learned.is_new = true;
    learned.is_protected = is_protected;
    terms.insert(terms.begin(), learned);
    store(settings, terms);
    return learned;
}

std::optional<DictionaryTerm> find_term(const Json& settings, std::string_view text) {
    const std::vector<DictionaryTerm> terms = load_dictionary(settings);
    const std::ptrdiff_t index = index_of_text(terms, clean(text));
    if (index < 0) {
        return std::nullopt;
    }
    return terms[static_cast<std::size_t>(index)];
}

std::optional<std::pair<std::string, std::vector<std::string>>>
spelling_rule(std::string_view rule) {
    // _SPELLING_RULE_MAX_CHARS, in code points.
    constexpr std::size_t kMaxChars = 120;
    static const py::Regex quoted_pattern(
        std::format("[\"“”]([^\"“”]{{1,{}}})[\"“”]", MAX_TERM_CHARS));
    static const py::Regex spell(R"(\bspell(?:s|ed|ing)?\b)", py::IGNORECASE);
    // A condition makes the rule more than a spelling; it stays a rule.
    static const py::Regex conditional(R"(\b(?:except|unless|when|whenever|if|only|but)\b)",
                                       py::IGNORECASE);
    static const py::Regex as_word(R"(\bas\b)", py::IGNORECASE);
    static const py::Regex not_word(R"(\bnot\b)", py::IGNORECASE);

    if (py::len(rule) > kMaxChars) {
        return std::nullopt;
    }
    if (!spell.search(rule) || conditional.search(rule)) {
        return std::nullopt;
    }
    std::vector<std::string> quoted;
    for (const std::string& text : quoted_pattern.findall(rule)) {
        quoted.push_back(clean(text));
    }
    for (const std::string& text : quoted) {
        if (text.empty()) {
            return std::nullopt;
        }
    }
    if (quoted.size() == 1) {
        return std::pair{quoted[0], std::vector<std::string>{}};
    }
    if (quoted.size() == 2 && as_word.search(rule)) {
        return std::pair{quoted[1], std::vector<std::string>{quoted[0]}};
    }
    if (quoted.size() == 2 && not_word.search(rule)) {
        return std::pair{quoted[0], std::vector<std::string>{quoted[1]}};
    }
    return std::nullopt;
}

std::vector<std::string> spelling_rules(const Json& settings) {
    std::vector<std::string> rules;
    for (std::string& rule : detail::cleanup_rules(settings)) {
        if (spelling_rule(rule)) {
            rules.push_back(std::move(rule));
        }
    }
    return rules;
}

std::size_t move_spelling_rules(Json& settings) {
    const std::vector<std::string> rules = detail::cleanup_rules(settings);
    std::vector<std::string> kept;
    std::size_t moved = 0;
    for (const std::string& rule : rules) {
        auto parsed = spelling_rule(rule);
        if (!parsed) {
            kept.push_back(rule);
            continue;
        }
        auto& [term, heard] = *parsed;
        try {
            add_term(settings, term, heard);
        } catch (const DictionaryError&) {
            if (index_of_text(load_dictionary(settings), clean(term)) < 0) {
                kept.push_back(rule);
                continue;
            }
        }
        ++moved;
    }
    if (moved > 0) {
        settings[std::string(keys::transcript_cleanup_rules.key())] = kept;
    }
    return moved;
}

} // namespace fv::text
