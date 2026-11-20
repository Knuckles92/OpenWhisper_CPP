// meeting/custom_report.py:98-252: the transcript corpus the report pass reads.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <set>
#include <utility>

#include "meeting_intel/custom_report.h"
#include "meeting_intel/detail/report.h"
#include "meeting_intel/detail/text.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::meeting_intel {

namespace {

constexpr std::string_view kStopWords[] = {
    "a",    "an",   "and",   "are",   "as",    "at",    "be",   "but",  "by",    "for", "from",
    "has",  "have", "how",   "in",    "into",  "is",    "it",   "its",  "of",    "on",  "or",
    "that", "the",  "their", "them",  "there", "these", "they", "this", "to",    "was", "were",
    "what", "when", "where", "which", "who",   "why",   "will", "with", "would", "you", "your"};

bool stop_word(std::string_view word) {
    return std::ranges::find(kStopWords, word) != std::end(kStopWords);
}

std::set<std::string> term_set(std::string_view text) {
    const auto terms = detail::report_terms(text);
    return {terms.begin(), terms.end()};
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

// _speaker: the participant's name (or id), else the channel's label.
std::string speaker(const Json& segment, const Json& participants) {
    static const Json none;
    const Json& pid = detail::member(segment, "speaker_participant_id");
    const Json& person = detail::truthy(pid) ? detail::participant(participants, pid) : none;
    if (detail::truthy(person)) {
        if (!person.is_object()) {
            throw py::PyError("AttributeError", "participant has no attribute 'get'");
        }
        const Json& name = detail::member(person, "display_name");
        return detail::py_str(detail::truthy(name) ? name : pid);
    }
    return detail::member(segment, "channel") == "mic" ? "Me" : "Others";
}

// _line: "[3:12] Priya: we should hold the price".
std::string line(const Json& segment, const Json& participants) {
    const std::string text =
        py::strip(replace_all(detail::text_or_empty(detail::member(segment, "text")), "\n", " "));
    const std::string clock = format_clock(detail::member(segment, "start_s"));
    return "[" + clock + "] " + speaker(segment, participants) + ": " + text;
}

// int(limit or DEFAULT_SEARCH_LIMIT) clamped to 1..MAX_SEARCH_LIMIT.
std::int64_t search_limit(const Json& limit) {
    if (!detail::truthy(limit)) {
        return kReportDefaultSearchLimit;
    }
    const auto value = py::py_int_checked(limit);
    if (value) {
        return std::clamp<std::int64_t>(*value, 1, kReportMaxSearchLimit);
    }
    switch (value.error()) {
    case py::IntError::Type:
        throw py::TypeError(
            "int() argument must be a string, a bytes-like object or a real number");
    case py::IntError::Value:
        throw py::ValueError("invalid literal for int()");
    case py::IntError::Overflow:
        break;
    }
    if (limit.is_number_float() && std::isinf(limit.get<double>())) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    // Python's int is unbounded: a finite value beyond int64 still clamps by its sign.
    if (limit.is_string()) {
        return py::strip(limit.get<std::string>()).starts_with('-') ? 1 : kReportMaxSearchLimit;
    }
    if (limit.is_number_float()) {
        return limit.get<double>() < 0 ? 1 : kReportMaxSearchLimit;
    }
    return kReportMaxSearchLimit;
}

} // namespace

namespace detail {

const Json& member(const Json& d, std::string_view key) {
    static const Json missing;
    if (!d.is_object()) {
        return missing;
    }
    const auto found = d.find(std::string(key));
    return found == d.end() ? missing : *found;
}

const Json& participant(const Json& participants, const Json& key) {
    static const Json missing;
    if (key.is_array() || key.is_object()) {
        throw py::TypeError(std::string("unhashable type: '") + (key.is_array() ? "list" : "dict") +
                            "'");
    }
    // Participant ids are text: any other key matches no member of a JSON object.
    if (!key.is_string()) {
        return missing;
    }
    return member(participants, key.get_ref<const std::string&>());
}

double float_or_zero(const Json& value) {
    if (!truthy(value)) {
        return 0.0;
    }
    if (const auto number = py::py_float(value)) {
        return *number;
    }
    if (value.is_string()) {
        throw py::ValueError("could not convert string to float: " +
                             py::repr(value.get_ref<const std::string&>()));
    }
    throw py::TypeError(std::string("float() argument must be a string or a real number, not '") +
                        (value.is_array() ? "list" : "dict") + "'");
}

std::vector<std::string> report_terms(std::string_view text) {
    static const py::Regex word(R"([\w']+)");
    std::set<std::string> terms;
    for (auto& found : word.findall(py::lower(text))) {
        if (py::len(found) > 2 && !stop_word(found)) {
            terms.insert(std::move(found));
        }
    }
    return {terms.begin(), terms.end()};
}

std::string clip_lines(const std::vector<std::string>& lines, std::size_t budget) {
    std::vector<std::string> kept;
    std::size_t used = 0;
    for (const auto& item : lines) {
        const std::size_t length = py::len(item);
        if (used + length + 1 > budget) {
            kept.push_back(std::format("[… {} more lines omitted …]", lines.size() - kept.size()));
            break;
        }
        kept.push_back(item);
        used += length + 1;
    }
    std::string out;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        out += (i == 0 ? "" : "\n") + kept[i];
    }
    return out;
}

std::vector<std::string> splitlines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t i = 0;
    bool open = false; // text since the last break
    const auto push = [&] {
        lines.push_back(std::move(current));
        current.clear();
        open = false;
    };
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\r') {
            push();
            i += (i + 1 < text.size() && text[i + 1] == '\n') ? 2 : 1;
            continue;
        }
        if (c == '\n' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e) {
            push();
            ++i;
            continue;
        }
        // U+0085 (C2 85), U+2028 (E2 80 A8) and U+2029 (E2 80 A9) end a line too.
        if (c == 0xc2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x85) {
            push();
            i += 2;
            continue;
        }
        if (c == 0xe2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(text[i + 2]) == 0xa8 ||
             static_cast<unsigned char>(text[i + 2]) == 0xa9)) {
            push();
            i += 3;
            continue;
        }
        current += text[i];
        open = true;
        ++i;
    }
    if (open) {
        lines.push_back(std::move(current));
    }
    return lines;
}

} // namespace detail

std::string format_clock(double seconds) {
    if (std::isnan(seconds)) {
        return "0:00"; // int(nan) raises ValueError, which reads as 0
    }
    if (std::isinf(seconds)) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    const double whole = std::trunc(seconds);
    std::int64_t total = 0;
    if (whole >= 9.2e18) {
        total = std::numeric_limits<std::int64_t>::max();
    } else if (whole > 0) {
        total = static_cast<std::int64_t>(whole);
    }
    const std::int64_t secs = total % 60;
    const std::int64_t minutes = (total / 60) % 60;
    const std::int64_t hours = total / 3600;
    if (hours != 0) {
        return std::format("{}:{:02}:{:02}", hours, minutes, secs);
    }
    return std::format("{}:{:02}", minutes, secs);
}

std::string format_clock(const Json& seconds) {
    const auto value = py::py_float(seconds);
    return value ? format_clock(*value) : std::string("0:00");
}

TranscriptCorpus::TranscriptCorpus(const std::vector<Json>& segments, const Json& participants) {
    for (const auto& segment : segments) {
        if (!py::strip(detail::text_or_empty(detail::member(segment, "text"))).empty()) {
            segments_.push_back(segment);
        }
    }
    const Json people = detail::truthy(participants) ? participants : Json::object();
    for (const auto& segment : segments_) {
        lines_.push_back(line(segment, people));
        lengths_.push_back(py::len(lines_.back()));
    }
    for (const auto& segment : segments_) {
        starts_.push_back(detail::float_or_zero(detail::member(segment, "start_s")));
    }
    // max(..., default=0.0) keeps the first of equal values and never replaces with a NaN.
    bool first = true;
    for (const auto& segment : segments_) {
        const double end = detail::float_or_zero(detail::member(segment, "end_s"));
        if (first || end > duration_s_) {
            duration_s_ = end;
            first = false;
        }
    }
}

std::size_t TranscriptCorpus::total_chars() const {
    std::size_t total = 0;
    for (const auto length : lengths_) {
        total += length + 1;
    }
    return total;
}

std::string TranscriptCorpus::render_all() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        out += (i == 0 ? "" : "\n") + lines_[i];
    }
    return out;
}

std::string TranscriptCorpus::render_edges(std::size_t edge_chars) const {
    std::size_t head = 0;
    std::size_t head_chars = 0;
    for (; head < lines_.size(); ++head) {
        if (head_chars + lengths_[head] > edge_chars) {
            break;
        }
        head_chars += lengths_[head] + 1;
    }
    std::size_t tail = 0;
    std::size_t tail_chars = 0;
    while (tail < lines_.size() - head) {
        const std::size_t length = lengths_[lines_.size() - 1 - tail];
        if (tail_chars + length > edge_chars) {
            break;
        }
        tail_chars += length + 1;
        ++tail;
    }
    const std::size_t skipped = lines_.size() - head - tail;
    if (skipped == 0) {
        return render_all();
    }
    const std::string gap_from = starts_.empty() ? "0:00" : format_clock(starts_[head]);
    const std::string gap_to =
        tail != 0 ? format_clock(starts_[lines_.size() - tail - 1]) : format_clock(duration_s_);
    std::string out;
    for (std::size_t i = 0; i < head; ++i) {
        out += (i == 0 ? "" : "\n") + lines_[i];
    }
    out += std::format("\n\n[… {} lines from {} to {} are not shown here. Use read_transcript or "
                       "search_transcript to read them. …]\n\n",
                       skipped, gap_from, gap_to);
    for (std::size_t i = lines_.size() - tail; i < lines_.size(); ++i) {
        out += (i == lines_.size() - tail ? "" : "\n") + lines_[i];
    }
    return out;
}

std::string TranscriptCorpus::window(double start_s, double end_s) const {
    if (end_s < start_s) {
        std::swap(start_s, end_s);
    }
    std::vector<std::string> picked;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (start_s <= starts_[i] && starts_[i] <= end_s) {
            picked.push_back(lines_[i]);
        }
    }
    if (picked.empty()) {
        return "No transcript between " + format_clock(start_s) + " and " + format_clock(end_s) +
               ". The meeting runs to " + format_clock(duration_s_) + ".";
    }
    return detail::clip_lines(picked, kReportWindowChars);
}

std::string TranscriptCorpus::search(std::string_view q, int limit) const {
    return search(q, Json(limit));
}

std::string TranscriptCorpus::search(std::string_view q, const Json& limit) const {
    const std::set<std::string> terms = term_set(q);
    if (terms.empty()) {
        return "Provide search terms.";
    }
    const std::int64_t count = search_limit(limit);
    std::vector<std::pair<std::size_t, std::size_t>> scored; // (hits, index)
    for (std::size_t index = 0; index < segments_.size(); ++index) {
        const std::set<std::string> words =
            term_set(detail::text_or_empty(detail::member(segments_[index], "text")));
        std::size_t hits = 0;
        for (const auto& term : terms) {
            hits += words.contains(term) ? 1 : 0;
        }
        if (hits != 0) {
            scored.emplace_back(hits, index);
        }
    }
    if (scored.empty()) {
        return "No transcript lines match " + py::repr(q) + ".";
    }
    std::ranges::sort(scored, [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<std::size_t> chosen;
    for (std::size_t i = 0; i < scored.size() && std::cmp_less(i, count); ++i) {
        chosen.push_back(scored[i].second);
    }
    std::ranges::sort(chosen);
    std::vector<std::string> picked;
    for (const auto index : chosen) {
        picked.push_back(lines_[index]);
    }
    return std::format("{} matching lines; showing {} in time order.", scored.size(),
                       chosen.size()) +
           "\n" + detail::clip_lines(picked, kReportSearchResultChars);
}

} // namespace fv::meeting_intel
