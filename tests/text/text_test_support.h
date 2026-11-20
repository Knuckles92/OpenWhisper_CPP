#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "pyport/text.h"
#include "text/dictionary.h"
#include "text/snippets.h"

// Shared helpers of the fv_text tests: the entries OpenWhisper's tests build, and the JSON forms
// the golden case files use (tools/parity/cases/services.dictionary.py, services.snippets.py).
namespace fv::text_test {

inline const std::string kDictionary = "dictation_dictionary";
inline const std::string kSnippets = "dictation_snippets";
inline const std::string kRules = "transcript_cleanup_rules";

// The tests' entry(term, **fields): {"id": term.lower() unless given, "term": term, **fields}.
inline Json entry(const std::string& term, const Json& fields = Json::object()) {
    Json out = Json::object();
    out["id"] = fields.contains("id") ? fields.at("id") : Json(py::lower(term));
    out["term"] = term;
    for (const auto& [key, value] : fields.items()) {
        if (key != "id") {
            out[key] = value;
        }
    }
    return out;
}

inline Json settings_with(std::string_view key, Json value) {
    Json settings = Json::object();
    settings[std::string(key)] = std::move(value);
    return settings;
}

inline Json dictionary_settings(Json entries) {
    return settings_with(kDictionary, std::move(entries));
}

inline std::vector<std::string> terms_of(const Json& settings) {
    std::vector<std::string> out;
    for (const text::DictionaryTerm& term : text::load_dictionary(settings)) {
        out.push_back(term.term);
    }
    return out;
}

inline text::DictionaryTerm term(std::string id, std::string spelling,
                                 Json fields = Json::object()) {
    text::DictionaryTerm out;
    out.id = std::move(id);
    out.term = std::move(spelling);
    out.starred = fields.value("starred", false);
    out.heard = fields.value("heard", std::vector<std::string>{});
    out.learned = fields.value("learned", false);
    out.is_new = fields.value("new", false);
    out.is_protected = fields.value("protected", false);
    return out;
}

// The case files' term dict: every field, "protected" included.
inline Json plain(const text::DictionaryTerm& term) {
    Json out = Json::object();
    out["id"] = term.id;
    out["term"] = term.term;
    out["starred"] = term.starred;
    out["heard"] = term.heard;
    out["learned"] = term.learned;
    out["new"] = term.is_new;
    out["protected"] = term.is_protected;
    return out;
}

inline Json plain(const std::optional<text::DictionaryTerm>& term) {
    return term ? plain(*term) : Json(nullptr);
}

inline Json plain(const std::vector<text::DictionaryTerm>& terms) {
    Json out = Json::array();
    for (const text::DictionaryTerm& item : terms) {
        out.push_back(plain(item));
    }
    return out;
}

inline text::DictionaryTerm term_from(const Json& fields) {
    return term(fields.at("id").get<std::string>(), fields.at("term").get<std::string>(), fields);
}

inline std::vector<text::DictionaryTerm> terms_from(const Json& list) {
    std::vector<text::DictionaryTerm> out;
    for (const Json& fields : list) {
        out.push_back(term_from(fields));
    }
    return out;
}

inline text::Snippet snippet(std::string id, std::string trigger, std::string text,
                             bool formatted = false) {
    return {std::move(id), std::move(trigger), std::move(text), formatted};
}

inline text::Snippet snippet_from(const Json& fields) {
    return {fields.at("id").get<std::string>(), fields.at("trigger").get<std::string>(),
            fields.at("text").get<std::string>(), fields.value("formatted", false)};
}

inline std::vector<text::Snippet> snippets_from(const Json& list) {
    std::vector<text::Snippet> out;
    for (const Json& fields : list) {
        out.push_back(snippet_from(fields));
    }
    return out;
}

inline Json plain(const text::Snippet& item) {
    return text::snippet_to_json(item);
}

inline Json plain(const std::optional<text::Snippet>& item) {
    return item ? plain(*item) : Json(nullptr);
}

inline Json stored(const std::vector<text::Snippet>& snippets) {
    Json list = Json::array();
    for (const text::Snippet& item : snippets) {
        list.push_back(text::snippet_to_json(item));
    }
    return settings_with(kSnippets, std::move(list));
}

} // namespace fv::text_test
