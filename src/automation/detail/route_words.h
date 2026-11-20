#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The word model of Route by voice (services/actions/routing.py:72-114, 188-252): words with
// Python's Unicode \w, casefolded and without apostrophes or hyphens, and roughly how a word
// sounds, so a name misheard by speech recognition ("cloud" for "Claude") still counts.
// Every function is pure and thread-safe.
namespace fv::automation {
struct GateMatch;
}

namespace fv::automation::detail {

// One word of a text. Offsets are code points (Python's), the byte_ ones the same span in UTF-8.
struct RouteWord {
    std::string norm;
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t byte_start = 0;
    std::size_t byte_end = 0;
    // Punctuation, or the end of the text, follows the word.
    bool pause = false;
    // Written as a name (written_as_name).
    bool name = false;
};

// The words of `text` as they are written, apostrophes and hyphens inside them kept ("don't",
// "to-do").
std::vector<std::string> word_tokens(std::string_view text);
// casefold, then without ' ’ - ‐.
std::string norm(std::string_view token);
// Written like a proper noun: a capital after the first letter ("ChatGPT"), or a capital first
// letter where no sentence starts ("Ask Claude"). Speech recognition writes names this way too.
bool written_as_name(std::string_view token, bool sentence_start);
// _words: the words of `text` with their offsets.
std::vector<RouteWord> route_words(std::string_view text);

// _sound: (consonants before the first vowel, that vowel's sound, the consonants after it,
// syllables) of a normalized word, or nullopt when it has no vowel. "claude", "cloud", "clod"
// and "clawed" all sound ("kl", "o", "d", 1).
struct WordSound {
    std::string onset;
    std::string vowel;
    std::string coda;
    int syllables = 0;
    friend bool operator==(const WordSound&, const WordSound&) = default;
};
std::optional<WordSound> word_sound(std::string_view word);
// _alike: `heard` is `name`, or plausibly a mishearing of it. Both of 4 or more letters, at most
// 2 apart in length, and the name has at least 3 consonants around its first vowel.
bool alike(std::string_view heard, std::string_view name);

// _Variant: words that name an action, and what may come before them.
struct NameVariant {
    std::vector<std::string> words;
    // The name's own leading words dropped to make this form ("send to my" from "Send to my
    // scripts"); said before it, they still name the action.
    std::vector<std::string> dropped;
    // Whether any lead-in words may come before it ("can you ask Claude").
    bool after_lead_ins = true;
    // Whether they may anyway when a word of it is heard written as a name ("tell Claude to …"
    // for an action typed "ask claude").
    bool after_lead_ins_as_name = false;

    // Whether the lead-in words `said` just before it, heard as `heard`, leave it naming the
    // action.
    bool follows(const std::vector<std::string>& said, std::span<const RouteWord> heard) const;
    friend bool operator==(const NameVariant&, const NameVariant&) = default;
};

// _name_variants: each of the name and spoken names as given, then without lead-ins and generic
// tails. `name` and `spoken_names` are the action's.
std::vector<NameVariant> name_variants(std::string_view name,
                                       const std::vector<std::string>& spoken_names);

// At most this many lead-in words before a name, and link words skipped after one.
inline constexpr std::size_t MAX_LEAD_INS = 4;
inline constexpr std::size_t MAX_LINK_WORDS = 4;

// _skip_fillers: the index of the first word after the fillers that start an utterance.
std::size_t skip_fillers(const std::vector<RouteWord>& words);
// _match_len: how many words from `at` say `variant`; 0 when they don't. Spacing is ignored
// ("chat gpt" says "ChatGPT"), unless a name heard as more words than it has goes on as an
// ordinary phrase without a pause ("to do that"); each word may be a near-homophone.
std::size_t match_len(const std::vector<RouteWord>& words, std::size_t at,
                      const std::vector<std::string>& variant);
// _polite_tail: 1 for a final "please" or "thanks", 2 for "thank you", else 0.
std::size_t polite_tail(const std::vector<RouteWord>& words);
// Whether `norm` is one of _LEAD_INS, the words that may come before a name.
bool is_lead_in(std::string_view norm);
// Whether `norm` is one of _LINK_WORDS, which only link a name to the request.
bool is_link_word(std::string_view norm);

// _spans (routing.cpp): the payloads code proposes for `text`, whose words and gate matches
// these are: the words after (or before) each named action, with and without the link words
// and politeness around them, then all of it.
std::vector<std::string> payload_spans(std::string_view text, const std::vector<RouteWord>& words,
                                       const std::vector<GateMatch>& matches);

// _without_polite_end: `span` without a final "please", "thanks" or "thank you" and the
// punctuation before it, unless nothing would be left.
std::string without_polite_end(std::string_view span);
// _clip: `text` when it has at most head + tail + 3 code points, else its head and tail joined
// by " … ".
std::string clip(std::string_view text, std::size_t head, std::size_t tail);
// _sentence_start: `span` with a lowercase first letter raised, as the start of a new request.
std::string sentence_start(std::string_view span);

} // namespace fv::automation::detail
