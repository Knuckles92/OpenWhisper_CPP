#include "text/dictionary_learning.h"

#include <unordered_set>

#include "base/json.h"
#include "pyport/difflib.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "text/data/common_words.json.h"

namespace fv::text {

namespace {

// Punctuation a word may carry in running text; anything else (an underscore, a slash, an @)
// keeps the token whole so it is never learned.
constexpr std::string_view kEdge = ".,!?;:\"“”'‘’()[]{}<>«»…";

const std::unordered_set<std::string>& common_words() {
    static const std::unordered_set<std::string> words = [] {
        std::unordered_set<std::string> out;
        const Json document = Json::parse(embedded::text_common_words);
        for (const Json& word : document.at("words")) {
            out.insert(word.get<std::string>());
        }
        return out;
    }();
    return words;
}

// _bare: apostrophes and hyphens removed, casefolded.
std::string bare(std::string_view word) {
    static const py::Regex marks("['’-]");
    return py::casefold(marks.sub("", word));
}

bool learnable(std::string_view old_word, std::string_view new_word) {
    static const py::Regex word(R"([^\W_]+(?:['’-][^\W_]+)*)");
    if (py::len(new_word) < 3 || !word.fullmatch(new_word)) {
        return false;
    }
    bool has_letter = false;
    for (const char32_t cp : py::to_code_points(new_word)) {
        has_letter = has_letter || py::is_alpha(cp);
    }
    if (!has_letter) {
        return false;
    }
    if (bare(new_word) == bare(old_word)) {
        return false; // only case, an apostrophe or a hyphen changed
    }
    return !is_common_word(py::casefold(new_word));
}

std::vector<std::string> casefolded(const std::vector<std::string>& words) {
    std::vector<std::string> out;
    out.reserve(words.size());
    for (const std::string& word : words) {
        out.push_back(py::casefold(word));
    }
    return out;
}

} // namespace

std::vector<std::string> correction_tokens(std::string_view text) {
    std::vector<std::string> tokens;
    for (const std::string& raw : py::split(text)) {
        std::string token = py::strip(raw, kEdge);
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
    }
    return tokens;
}

bool is_common_word(std::string_view word) {
    return common_words().contains(std::string(word));
}

std::optional<std::pair<std::string, std::string>> learned_correction(std::string_view pasted,
                                                                      std::string_view reread) {
    const std::vector<std::string> said = correction_tokens(pasted);
    const std::vector<std::string> seen = correction_tokens(reread);
    if (said.size() < 3 || seen.empty()) {
        return std::nullopt;
    }
    py::WordSequenceMatcher matcher(casefolded(said), casefolded(seen), false);
    std::vector<py::Opcode> changes;
    std::size_t anchors = 0;
    for (const py::Opcode& op : matcher.get_opcodes()) {
        if (op.tag == py::OpTag::Equal) {
            anchors += op.i2 - op.i1;
        } else if (op.tag == py::OpTag::Insert && (op.i1 == 0 || op.i1 == said.size())) {
            continue; // the field's own text before or after the paste
        } else {
            changes.push_back(op);
        }
    }
    if (changes.size() != 1 || anchors < 2) {
        return std::nullopt;
    }
    const py::Opcode& change = changes.front();
    if (change.tag != py::OpTag::Replace || change.i2 - change.i1 != 1 ||
        change.j2 - change.j1 != 1) {
        return std::nullopt;
    }
    const std::string& old_word = said[change.i1];
    const std::string& new_word = seen[change.j1];
    if (!learnable(old_word, new_word)) {
        return std::nullopt;
    }
    return std::pair{old_word, new_word};
}

bool reverted_cleanup(std::optional<std::string_view> before_cleanup, std::string_view replaced,
                      std::string_view restored) {
    std::unordered_set<std::string> spoken;
    for (const std::string& word : correction_tokens(before_cleanup.value_or(""))) {
        spoken.insert(py::casefold(word));
    }
    return spoken.contains(py::casefold(restored)) && !spoken.contains(py::casefold(replaced));
}

} // namespace fv::text
