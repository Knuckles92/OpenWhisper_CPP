#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "pyport/errors.h"

// The personal dictionary (services/dictionary.py): words the app should always get right. Terms
// replace their "sounds like" variants in every transcript, go into the cleanup prompt and, where
// an engine supports hints, reach the speech model. Terms are user content: never log them.
//
// The dictionary is a list of {id, term, starred, heard, learned, new, protected} objects under
// keys::dictation_dictionary, newest first. Loading is tolerant: malformed entries are skipped,
// duplicates (ignoring case) keep the first, and the list is capped at MAX_TERMS. The mutators
// edit the settings mapping they are given, for SettingsStore::mutate. Every function is pure
// over its arguments and thread-safe.
namespace fv::text {

// config.MAX_DICTIONARY_TERMS.
inline constexpr std::size_t MAX_TERMS = 500;
// config.MAX_RECOGNITION_PHRASES: speech-model hints, starred terms first.
inline constexpr std::size_t MAX_RECOGNITION_PHRASES = 50;
// Longest term or "sounds like" variant, in code points.
inline constexpr std::size_t MAX_TERM_CHARS = 64;
// "Sounds like" variants per term.
inline constexpr std::size_t MAX_HEARD = 5;
// The cleanup prompt lists at most this much of the dictionary, starred first, so a large
// dictionary still fits a 4k-context local model. Characters are code points.
inline constexpr std::size_t PROMPT_MAX_TERMS = 200;
inline constexpr std::size_t PROMPT_MAX_CHARS = 2400;
// Words the cleanup prompt tells AI cleanup never to change (PROMPT_MAX_PROTECTED).
inline constexpr std::size_t PROMPT_MAX_PROTECTED = 50;
// How long after a paste the field is read again for corrections, in seconds, and how many
// sightings of the same correction make a word learned. The scheduler that uses them is
// fv_dictation's.
inline constexpr double LEARN_DELAY_S = 4.0;
inline constexpr int LEARN_SIGHTINGS = 2;

// A change that cannot be saved; what() is the sentence shown to the user. Python's
// DictionaryError subclasses ValueError, and type_name() is "DictionaryError".
class DictionaryError : public py::ValueError {
public:
    explicit DictionaryError(const std::string& message)
        : py::ValueError("DictionaryError", message) {}
};

// The JSON keys are "new" and "protected"; those are C++ keywords, hence is_new and
// is_protected.
struct DictionaryTerm {
    std::string id;
    std::string term;
    bool starred = false;
    // User-entered "sounds like" variants replaced by term.
    std::vector<std::string> heard;
    bool learned = false;
    // Learned since the user last reviewed the Dictionary page.
    bool is_new = false;
    // AI cleanup changed this word and the user put it back, so the cleanup prompt tells it
    // never to change the word again.
    bool is_protected = false;

    friend bool operator==(const DictionaryTerm&, const DictionaryTerm&) = default;
};

// _as_dict: the saved form. "protected" is written only when true.
Json term_to_json(const DictionaryTerm& term);

// _parse: the valid terms of a raw saved value, in order.
std::vector<DictionaryTerm> parse_terms(const Json& raw);
// The valid saved terms, in their saved order (newest first).
std::vector<DictionaryTerm> load_dictionary(const Json& settings);

// Speech-model hints: starred terms first, cut to MAX_RECOGNITION_PHRASES. Empty while
// keys::dictionary_steer_recognition is off.
std::vector<std::string> recognition_phrases(const Json& settings);

// `text` with every "sounds like" variant replaced by its term: whole words, any case, longest
// variant first, in one pass; a starred term wins a variant two terms share. Only variants the
// user typed count; learned terms never replace.
std::string apply_replacements(std::string_view text, const std::vector<DictionaryTerm>& terms);

// The cleanup-prompt block listing the terms, or "".
std::string prompt_block(const std::vector<DictionaryTerm>& terms);

// "12 words · 2 new", for the Settings rail; "No words yet" when empty.
std::string summary(const Json& settings);

// "Sounds like" variants typed into one field, split at commas, semicolons and new lines, each
// with its whitespace collapsed; empty pieces are dropped.
std::vector<std::string> parse_heard(std::string_view text);

// Mutators. Each edits `settings` (a settings object) and throws DictionaryError with the
// user's sentence when the change cannot be saved, leaving `settings` unchanged.

// Adds `term` at the top, or merges into the entry it duplicates (ignoring case): that keeps its
// place, takes the new spelling and any new "sounds like" variants, ORs the star, and makes a
// learned word the user's own. Throws when the term is blank or too long, already there
// unchanged, or the dictionary is full.
DictionaryTerm add_term(Json& settings, std::string_view term,
                        const std::vector<std::string>& heard = {}, bool starred = false);
// Replaces one entry's spelling and "sounds like" variants; an edited learned word becomes the
// user's own and loses its New badge.
DictionaryTerm update_term(Json& settings, std::string_view term_id, std::string_view term,
                           const std::vector<std::string>& heard);
// Also drops the New badge.
DictionaryTerm set_starred(Json& settings, std::string_view term_id, bool starred);
// Removes one entry and returns it, or nullopt when it was already gone.
std::optional<DictionaryTerm> delete_term(Json& settings, std::string_view term_id);
// Removes a learned entry; a word the user added or edited stays (nullopt).
std::optional<DictionaryTerm> undo_learned(Json& settings, std::string_view term_id);
// Keeps every new learned word and drops its New badge; returns how many.
std::size_t clear_new(Json& settings);
// Adds a word learned from a correction, unless learning is off, the word is blank or too
// long, the dictionary is full or it is known. `is_protected` marks a word the user put back
// after AI cleanup changed it; a known word gains that mark (and is returned) once.
std::optional<DictionaryTerm> add_learned(Json& settings, std::string_view word,
                                          bool is_protected = false);
// The saved entry spelled `text` (ignoring case and extra whitespace), or nullopt.
std::optional<DictionaryTerm> find_term(const Json& settings, std::string_view text);

// The (term, heard) a plain spelling rule of the Learned rules page teaches, or nullopt.
// Matches short, unconditional rules such as `Always spell my name "Alex Rivera"`,
// `Spell "jon" as "John"` and `Spell it "Ksenia", not "Sonia"`.
std::optional<std::pair<std::string, std::vector<std::string>>>
spelling_rule(std::string_view rule);
// Learned cleanup rules that are plain spellings, which belong in the dictionary.
std::vector<std::string> spelling_rules(const Json& settings);
// Turns plain spelling rules into dictionary words and returns how many moved. A rule whose
// word cannot be added stays a rule, unless the word is in the dictionary already.
std::size_t move_spelling_rules(Json& settings);

} // namespace fv::text
