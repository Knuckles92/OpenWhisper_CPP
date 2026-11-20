#include "text/recognition.h"

#include <cstdint>

#include "pyport/text.h"

namespace fv::text {

namespace {

// _bounded: the first phrases that fit `count` and a budget of `chars` code points, counting
// `separator` between them. Blank and over-long phrases are skipped; the first that does not fit
// ends the list.
std::vector<std::string> bounded(const std::vector<std::string>& phrases, std::size_t count,
                                 std::int64_t chars, std::string_view separator) {
    std::vector<std::string> kept;
    std::int64_t used = 0;
    for (const std::string& raw : phrases) {
        std::string phrase;
        for (const std::string& word : py::split(raw)) {
            if (!phrase.empty()) {
                phrase += ' ';
            }
            phrase += word;
        }
        const auto length = static_cast<std::int64_t>(py::len(phrase));
        if (phrase.empty() || length > static_cast<std::int64_t>(MAX_PHRASE_CHARS)) {
            continue;
        }
        const std::int64_t cost =
            length + (kept.empty() ? 0 : static_cast<std::int64_t>(py::len(separator)));
        if (kept.size() >= count || used + cost > chars) {
            break;
        }
        kept.push_back(std::move(phrase));
        used += cost;
    }
    return kept;
}

} // namespace

std::string engine_support(std::string_view engine) {
    if (engine == "api") {
        return std::string(SUPPORT_MODEL);
    }
    if (engine == "remote") {
        return "";
    }
    return std::string(SUPPORT_AFTER);
}

std::string language_code(const std::optional<std::string_view>& language) {
    if (!language || language->empty() || py::lower(*language) == "auto") {
        return "";
    }
    std::string code(*language);
    for (char& c : code) {
        if (c == '_') {
            c = '-';
        }
    }
    return py::lower(code.substr(0, code.find('-')));
}

std::vector<std::string> keywords(const std::vector<std::string>& phrases) {
    return bounded(phrases, OPENAI_MAX_KEYWORDS,
                   static_cast<std::int64_t>(OPENAI_MAX_KEYWORDS * MAX_PHRASE_CHARS), "");
}

std::string vocabulary_prompt(const std::vector<std::string>& phrases, std::size_t max_chars) {
    const std::vector<std::string> kept =
        bounded(phrases, phrases.size(), static_cast<std::int64_t>(max_chars) - 1, ", ");
    if (kept.empty()) {
        return "";
    }
    std::string out;
    for (const std::string& phrase : kept) {
        if (!out.empty()) {
            out += ", ";
        }
        out += phrase;
    }
    return out + ".";
}

} // namespace fv::text
