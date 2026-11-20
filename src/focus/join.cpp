#include "focus/join.h"

#include <set>
#include <vector>

#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::focus {

namespace {

using Points = std::u32string;

constexpr std::u32string_view kClosers = U")]}”’»";
constexpr std::u32string_view kStraightQuotes = U"\"'";
constexpr std::u32string_view kOpeners = U"([“«";
constexpr std::u32string_view kSentenceEnds = U".!?:";
constexpr std::u32string_view kContinuationPunctuation = U".,;:!?)]}…";
constexpr std::u32string_view kWordEnds = U".,;:!?%…";
// Rich-text editors may collapse ordinary whitespace at the edges of a pasted HTML fragment
// (Qt's text widgets do), so a space the join adds there is a non-breaking one.
constexpr std::string_view kHtmlEdgeSpace = "&nbsp;";

// Mid-sentence, only these common words lose the capital a speech engine gives a dictation's
// first word. Anything else may be a name, and a stray capital costs less than a lowercased one.
const std::set<std::string, std::less<>>& lowercase_starts() {
    static const std::set<std::string, std::less<>> words = [] {
        std::set<std::string, std::less<>> result;
        for (const auto& word : py::split(R"(
some any all every each more most many much few other another both either
neither one two three a an the and but or nor so yet for because if then than that this these
those it its it's we we're we'll we've you you're you'll your he he's his
she she's her they they're they'll their them me my mine our us there
there's here what what's when where which who why how is are was were be
been being am do does did done don't doesn't didn't can can't could
couldn't would wouldn't should shouldn't will won't have has had haven't
not no also just maybe perhaps still really to of in on at by with without
from about into onto over under after before while as until since through
during per via let's let please thanks thank okay ok yes yeah well actually
anyway though although otherwise instead plus anything something nothing
everything need needs want wants like likes get gets got make makes made go
goes going went come comes came see sees saw say says said tell told ask
asked use used work works working call called talk talking send sent check
add remove fix update review meet start stop finish try keep put take give
find think know feel hope look looks sound sounds seems sure great good
nice fine right cool perfect
)")) {
            result.insert(word);
        }
        return result;
    }();
    return words;
}

const py::Regex& list_marker() {
    static const py::Regex pattern(R"((?:^|\n)[ \t]*[-*•–][ \t]*$)");
    return pattern;
}

const py::Regex& first_word() {
    static const py::Regex pattern(R"((\s*)([^\W\d_][\w'’-]*))");
    return pattern;
}

const py::Regex& html_tag() {
    static const py::Regex pattern(R"((<[^>]*>))");
    return pattern;
}

bool contains(std::u32string_view set, char32_t point) {
    return set.find(point) != std::u32string_view::npos;
}

Points rstrip_any(Points text, std::u32string_view chars) {
    while (!text.empty() && contains(chars, text.back())) {
        text.pop_back();
    }
    return text;
}

Points lstrip_any(const Points& text, std::u32string_view chars) {
    std::size_t start = 0;
    while (start < text.size() && contains(chars, text[start])) {
        ++start;
    }
    return text.substr(start);
}

Points rstrip_space(Points text) {
    while (!text.empty() && py::is_space(text.back())) {
        text.pop_back();
    }
    return text;
}

Points lstrip_space(const Points& text) {
    std::size_t start = 0;
    while (start < text.size() && py::is_space(text[start])) {
        ++start;
    }
    return text.substr(start);
}

std::string lower_one(char32_t point) {
    return py::lower(py::to_utf8(Points(1, point)));
}

bool opens_quote(const Points& line) {
    const char32_t last = line.back();
    return last == U'“' || last == U'«' ||
           (contains(kStraightQuotes, last) &&
            (line.size() == 1 || py::is_space(line[line.size() - 2])));
}

bool continues_sentence(const Points& before) {
    const Points line = rstrip_any(before, U" \t");
    if (line.empty() || line.back() == U'\r' || line.back() == U'\n' ||
        list_marker().search(py::to_utf8(line)) || opens_quote(line)) {
        return false;
    }
    Points closers(kClosers);
    closers += kStraightQuotes;
    const Points words = rstrip_any(line, closers);
    return !words.empty() && !contains(kSentenceEnds, words.back());
}

// Whether before writes word capitalized away from a sentence start.
bool named_in(const std::string& word, const std::string& before) {
    const py::Regex pattern("(?<!\\w)" + py::regex_escape(word) + "(?!\\w)");
    const Points points = py::to_code_points(before);
    Points skipped(U" \t");
    skipped += kStraightQuotes;
    skipped += kOpeners;
    for (const auto& found : pattern.finditer(before)) {
        const Points preceding = rstrip_any(points.substr(0, found.start()), skipped);
        if (!preceding.empty() && !contains(kSentenceEnds, preceding.back()) &&
            preceding.back() != U'\r' && preceding.back() != U'\n') {
            return true;
        }
    }
    return false;
}

std::string lowercase_first_word(const std::string& text, const Points& before) {
    const auto match = first_word().match(text);
    if (!match) {
        return text;
    }
    const std::string word = match->group(2);
    const Points letters = py::to_code_points(word);
    if (!py::is_upper(letters[0])) {
        return text;
    }
    for (std::size_t index = 1; index < letters.size(); ++index) {
        if (py::is_upper(letters[index])) {
            return text;
        }
    }
    // A hyphenated compound ("e-" + "Mail") always continues the word.
    const bool compound = !before.empty() && before.back() == U'-' && before.size() >= 2 &&
                          py::is_alnum(before[before.size() - 2]);
    std::string folded = py::casefold(word);
    for (std::size_t at = folded.find("’"); at != std::string::npos;
         at = folded.find("’", at + 1)) {
        folded.replace(at, std::string_view("’").size(), "'");
    }
    const bool common = lowercase_starts().contains(folded);
    if (!compound && (!common || named_in(word, py::to_utf8(before)))) {
        return text;
    }
    const Points points = py::to_code_points(text);
    const std::size_t start = match->start(2);
    return py::to_utf8(points.substr(0, start)) + lower_one(points[start]) +
           py::to_utf8(points.substr(start + 1));
}

bool continues_after(const Points& after) {
    const Points stripped = lstrip_any(after, U" \t");
    if (stripped.empty()) {
        return false;
    }
    const char32_t first = stripped.front();
    return contains(kContinuationPunctuation, first) || py::is_lower(first);
}

Points drop_final_period(const Points& text) {
    const Points stripped = rstrip_space(text);
    const bool one_period = !stripped.empty() && stripped.back() == U'.' &&
                            !(stripped.size() >= 2 && stripped[stripped.size() - 2] == U'.');
    if (one_period) {
        return stripped.substr(0, stripped.size() - 1) + text.substr(stripped.size());
    }
    return text;
}

bool ends_a_word(const Points& before) {
    const char32_t last = before.back();
    if (py::is_alnum(last) || contains(kWordEnds, last) || contains(kClosers, last)) {
        return true;
    }
    // A straight quote right after a word closes it; after a space it opens.
    return contains(kStraightQuotes, last) && before.size() > 1 &&
           !py::is_space(before[before.size() - 2]);
}

enum class Edge { None, Space, Strip };

struct JoinEdits {
    // The first word that lost its capital, or "".
    std::string lowered;
    bool dropped_period = false;
    Edge lead = Edge::None;
    Edge trail = Edge::None;

    bool any() const {
        return !lowered.empty() || dropped_period || lead != Edge::None || trail != Edge::None;
    }
};

std::pair<std::string, JoinEdits> join(std::string_view text,
                                       const std::optional<TextContext>& context) {
    if (text.empty() || !context || !context->caret_known) {
        return {std::string(text), {}};
    }
    const Points before = py::to_code_points(context->before);
    const Points after = py::to_code_points(context->after);
    JoinEdits edits;
    std::string result(text);
    // Replacing a capitalized selection (a name) keeps the capital, like an editor's
    // case-preserving replace.
    const Points selected =
        context->selection_known ? lstrip_space(py::to_code_points(context->selected)) : Points();
    if (continues_sentence(before) && !(!selected.empty() && py::is_upper(selected.front()))) {
        result = lowercase_first_word(result, before);
        if (result != text) {
            edits.lowered = first_word().match(text)->group(2);
        }
    }
    Points points = py::to_code_points(result);
    if (continues_after(after)) {
        Points shorter = drop_final_period(points);
        edits.dropped_period = shorter != points;
        points = std::move(shorter);
    }
    if (!before.empty() && before.back() == U' ') {
        edits.lead = Edge::Strip;
        points = lstrip_any(points, U" ");
    } else if (!before.empty() && ends_a_word(before) && !points.empty() &&
               (py::is_alnum(points.front()) || contains(kOpeners, points.front()))) {
        edits.lead = Edge::Space;
        points.insert(points.begin(), U' ');
    }
    if (!after.empty() && after.front() == U' ') {
        edits.trail = Edge::Strip;
        points = rstrip_any(points, U" ");
    } else if (!after.empty() && py::is_alnum(after.front()) && !points.empty() &&
               !py::is_space(points.back())) {
        edits.trail = Edge::Space;
        points.push_back(U' ');
    }
    return {py::to_utf8(points), edits};
}

} // namespace

std::string join_with_context(std::string_view text, const std::optional<TextContext>& context) {
    return join(text, context).first;
}

std::string join_html_with_context(std::string_view html, std::string_view text,
                                   const std::optional<TextContext>& context) {
    if (html.empty()) {
        return std::string(html);
    }
    const JoinEdits edits = join(text, context).second;
    if (!edits.any()) {
        return std::string(html);
    }
    std::vector<std::string> parts;
    for (auto& part : html_tag().split(html)) {
        parts.push_back(part.value_or(""));
    }
    std::vector<std::size_t> texts;
    for (std::size_t index = 0; index < parts.size(); index += 2) {
        if (!py::strip(parts[index]).empty()) {
            texts.push_back(index);
        }
    }
    if (!edits.lowered.empty() && !texts.empty()) {
        std::string& first = parts[texts.front()];
        const auto match = first_word().match(first);
        if (match && match->group(2) == edits.lowered) {
            const Points points = py::to_code_points(first);
            const std::size_t start = match->start(2);
            first = py::to_utf8(points.substr(0, start)) + lower_one(points[start]) +
                    py::to_utf8(points.substr(start + 1));
        }
    }
    if (edits.dropped_period && !texts.empty()) {
        std::string& last = parts[texts.back()];
        last = py::to_utf8(drop_final_period(py::to_code_points(last)));
    }
    std::string joined;
    for (const auto& part : parts) {
        joined += part;
    }
    if (edits.lead == Edge::Strip) {
        joined = py::lstrip(joined, " ");
    } else if (edits.lead == Edge::Space) {
        joined = std::string(kHtmlEdgeSpace) + joined;
    }
    if (edits.trail == Edge::Strip) {
        joined = py::rstrip(joined, " ");
    } else if (edits.trail == Edge::Space) {
        joined += kHtmlEdgeSpace;
    }
    return joined;
}

} // namespace fv::focus
