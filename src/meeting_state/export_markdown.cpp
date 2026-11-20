#include "meeting_state/export_markdown.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <utility>

#include "meeting_state/detail/extras.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/export_txt.h"
#include "meeting_state/highlights.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

using detail::get;
using detail::truthy;
using Lines = std::vector<std::string>;

constexpr std::string_view kEmDash = "\xE2\x80\x94";
constexpr std::string_view kMiddleDot = "\xC2\xB7";

// Card keys in render order with their headings. The note taker's running minutes come first:
// they read as the narrative record the structured cards then back up.
constexpr std::pair<std::string_view, std::string_view> kCardSections[] = {
    {"live_notes", "Meeting Notes"},  {"key_points", "Key Points"}, {"decisions", "Decisions"},
    {"action_items", "Action Items"}, {"risks", "Risks"},           {"timeline", "Timeline"},
    {"user_notes", "Notes"},
};

constexpr std::pair<std::string_view, std::string_view> kCitationLabels[] = {
    {"supported", "Citation supports claim"}, {"contradicted", "Citation conflicts with claim"},
    {"unsupported", "Check citation"},        {"missing", "Missing citation"},
    {"uncertain", "Citation uncertain"},      {"unavailable", "Citation check unavailable"},
    {"stale", "Citation needs recheck"},
};

bool is(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

void require_hashable(const Json& key) {
    if (key.is_array() || key.is_object()) {
        throw py::TypeError("unhashable type");
    }
}

// (value or {}) read as a mapping.
const Json& mapping_or_empty(const Json& value) {
    static const Json empty = Json::object();
    return truthy(value) ? detail::mapping(value) : empty;
}

// (value or []) iterated.
Json list_or_empty(const Json& value) {
    return truthy(value) ? detail::py_list(value) : Json::array();
}

// float(value or 0.0); `strict` lets float()'s errors escape, else they are nullopt.
std::optional<double> float_of(const Json& value, bool strict) {
    if (!truthy(value)) {
        return 0.0;
    }
    if (const auto parsed = py::py_float(value)) {
        return parsed;
    }
    if (!strict) {
        return std::nullopt;
    }
    if (value.is_string()) {
        throw py::ValueError("could not convert string to float");
    }
    throw py::TypeError("float() argument must be a string or a real number");
}

double number(const Json& value) {
    return value.get<double>();
}

// Segments by id (later rows win), for evidence lookups.
std::map<std::string, const Json*, std::less<>> segment_index(const std::vector<Json>& segments) {
    std::map<std::string, const Json*, std::less<>> index;
    for (const Json& segment : segments) {
        if (segment.is_object() && truthy(get(segment, "id")) && get(segment, "id").is_string()) {
            index[get(segment, "id").get<std::string>()] = &segment;
        }
    }
    return index;
}

std::string text_of(const Json& item) {
    const Json& text = detail::at(item, "text");
    if (!text.is_string()) {
        throw py::PyError("AttributeError", "object has no attribute 'strip'");
    }
    return text.get<std::string>();
}

// Header

std::optional<double> duration_s(const Json& meeting, const std::vector<Json>& segments,
                                 const Clock& clock) {
    // Segment times are meeting-clock seconds (pauses excluded); the fallback subtracts pause
    // credit too.
    std::optional<double> longest;
    for (const Json& segment : segments) {
        const double end = *float_of(get(detail::mapping(segment), "end_s"), true);
        longest = longest ? std::max(*longest, end) : end;
    }
    if (longest && *longest > 0) {
        return longest;
    }
    return meeting_duration_s(meeting, false, clock);
}

std::string metadata_line(const Json& meeting, const Json& state, const std::vector<Json>& segments,
                          const Clock& clock) {
    Lines parts;
    if (auto date = format_meeting_date(meeting, clock)) {
        parts.push_back(std::move(*date));
    }
    if (const auto duration = duration_s(meeting, segments, clock)) {
        parts.push_back("Duration " + format_clock(*duration));
    }
    const auto names = participant_names(state);
    if (!names.empty()) {
        parts.push_back("Participants: " + detail::join(names, ", "));
    }
    return detail::join(parts, " " + std::string(kMiddleDot) + " ");
}

// The host's brief, human-authored, so it survives an intelligence-free export: it is what a
// reader needs to judge whether the record met its purpose.
void append_intent(Lines& out, const Json& state) {
    const Json& raw = get(mapping_or_empty(get(state, "intent")), "text");
    const std::string text = py::strip(truthy(raw) ? detail::py_str(raw) : "");
    if (text.empty()) {
        return;
    }
    out.insert(out.end(), {"", "## Meeting Brief", ""});
    // Every line is quoted, so a multi-line brief reads as one block.
    for (const auto& line : detail::splitlines(text)) {
        out.push_back(py::rstrip("> " + line));
    }
}

// The user's standing corrections from the insight review.
void append_clarifications(Lines& out, const Json& state) {
    const Json& review = mapping_or_empty(get(state, "insight_review"));
    Lines corrections;
    for (const Json& q : detail::list_member(review, "questions")) {
        const Json& correction = get(detail::mapping(q), "correction");
        if (truthy(correction) && !truthy(get(q, "superseded"))) {
            corrections.push_back("- " + detail::py_str(correction));
        }
    }
    if (corrections.empty()) {
        return;
    }
    out.insert(out.end(), {"", "## User Clarifications", "",
                           "These corrections supersede earlier wording on the same points.", ""});
    out.insert(out.end(), corrections.begin(), corrections.end());
}

void append_topic(Lines& out, const Json& state, const Clock& clock) {
    const Json& topic = mapping_or_empty(get(state, "topic"));
    const std::string current = py::strip(detail::text_or_empty(get(topic, "current")));
    if (current.empty()) {
        return;
    }
    out.insert(out.end(), {"", "## Topic", "", current});
    // The last history entry is the current topic; the ones before it are the revision trail.
    const Json history = list_or_empty(get(topic, "history"));
    Lines rendered;
    for (std::size_t i = 0; i + 1 < history.size(); ++i) {
        const Json& entry = detail::mapping(history[i]);
        const std::string text = py::strip(detail::text_or_empty(get(entry, "text")));
        if (text.empty()) {
            continue;
        }
        const auto ts = as_local_time(get(entry, "ts"), clock);
        const std::string stamp = ts ? std::format(" ({:02}:{:02})", ts->hour, ts->minute) : "";
        rendered.push_back("- " + text + stamp);
    }
    if (!rendered.empty()) {
        out.insert(out.end(), {"", "Previously:", ""});
        out.insert(out.end(), rendered.begin(), rendered.end());
    }
}

void append_summary(Lines& out, const Json& state) {
    const std::string summary = py::strip(detail::text_or_empty(get(state, "rolling_summary")));
    if (!summary.empty()) {
        out.insert(out.end(), {"", "## Summary", "", summary});
    }
}

// Saved meeting pulses stay in the shareable record.
void append_highlights(Lines& out, const Json& state) {
    std::vector<const Json*> pulses;
    const Json highlights = list_or_empty(get(state, "live_highlights"));
    for (const Json& pulse : highlights) {
        const Json& kind = get(detail::mapping(pulse), "kind");
        if (py::strip(detail::text_or_empty(get(pulse, "text"))).empty()) {
            continue;
        }
        require_hashable(kind);
        if (!kind.is_string() || !pulse_label(kind.get_ref<const std::string&>())) {
            continue;
        }
        const Json& start = get(pulse, "start_s");
        if (!start.is_number() || !std::isfinite(number(start))) {
            continue;
        }
        pulses.push_back(&pulse);
    }
    if (pulses.empty()) {
        return;
    }
    out.insert(out.end(), {"", "## Meeting Pulses", "",
                           "Automatically detected moments; labels are advisory.", ""});
    std::stable_sort(pulses.begin(), pulses.end(), [](const Json* a, const Json* b) {
        return number(get(*a, "start_s")) < number(get(*b, "start_s"));
    });
    for (const Json* pulse : pulses) {
        const auto label = *pulse_label(get(*pulse, "kind").get_ref<const std::string&>());
        out.push_back("- [" + format_mmss(number(get(*pulse, "start_s"))) + "] **" +
                      std::string(label) + ":** " + py::strip(text_of(*pulse)));
    }
}

// Cards

// _item_text: review, spoken provenance and the current citation advisory.
std::string item_text(const Json& item) {
    Lines labels;
    const Json& review = get(mapping_or_empty(get(item, "review")), "state");
    if (is(review, "unsupported")) {
        labels.push_back("Needs verification");
    } else if (is(review, "provisional")) {
        labels.push_back("Provisional");
    } else if (is(review, "human")) {
        labels.push_back("User clarified");
    }
    if (is(get(item, "author_type"), "system") && is(get(item, "author_id"), "voice_command")) {
        labels.push_back(is(get(item, "card"), "live_notes") ? "Spoken note" : "Spoken request");
    }
    std::string text = (labels.empty() ? "" : detail::join(labels, ": ") + ": ") + text_of(item);
    const Json& check = mapping_or_empty(get(item, "citation_check"));
    const Json& status = get(check, "status");
    require_hashable(status);
    std::optional<std::string_view> label;
    for (const auto& [name, words] : kCitationLabels) {
        if (is(status, name)) {
            label = words;
        }
    }
    const Json& revision = get(item, "revision");
    if (label && !revision.is_null() && py::py_equal(get(check, "revision"), revision)) {
        text += " _(Advisory: " + std::string(*label) + ")_";
    }
    return text;
}

// _live_items: items that are not removed and have text, with their rendered text.
std::vector<Json> live_items(const Json& state, std::string_view card) {
    std::vector<Json> out;
    const Json& cards = mapping_or_empty(get(state, "cards"));
    for (const Json& item : list_or_empty(get(cards, card))) {
        if (is(get(detail::mapping(item), "status"), "removed") ||
            py::strip(detail::text_or_empty(get(item, "text"))).empty()) {
            continue;
        }
        Json shown = item;
        shown["text"] = item_text(item);
        out.push_back(std::move(shown));
    }
    return out;
}

// Grouped Mine / Others / Unassigned by data.owner_participant_id when owners are known (an
// owner of kind "me" is Mine; any other known owner is Others, named); flat without any.
Lines action_item_lines(const std::vector<Json>& items, const Json& participants) {
    Lines mine;
    Lines others;
    Lines unassigned;
    for (const Json& item : items) {
        std::string text = py::strip(text_of(item));
        const Json& data = mapping_or_empty(get(item, "data"));
        const Json& deadline =
            truthy(get(data, "deadline")) ? get(data, "deadline") : get(data, "due_date");
        if (deadline.is_string() && !py::strip(deadline.get_ref<const std::string&>()).empty()) {
            text += " " + std::string(kEmDash) +
                    " Due: " + py::strip(deadline.get_ref<const std::string&>());
        }
        // data comes straight from the model and is only checked to be a mapping, so an owner
        // id that is not text is unassigned rather than an error.
        const Json& owner_id = get(data, "owner_participant_id");
        const Json* owner = nullptr;
        if (owner_id.is_string() && participants.is_object() &&
            participants.contains(owner_id.get_ref<const std::string&>())) {
            owner = &participants.at(owner_id.get_ref<const std::string&>());
            if (owner->is_null()) {
                owner = nullptr;
            }
        }
        if (owner == nullptr) {
            unassigned.push_back("- " + text);
        } else if (is(get(detail::mapping(*owner), "kind"), "me")) {
            mine.push_back("- " + text);
        } else {
            std::string name = py::strip(detail::text_or_empty(get(*owner, "display_name")));
            if (name.empty()) {
                name = "Unknown";
            }
            others.push_back("- " + text + " " + std::string(kEmDash) + " " + name);
        }
    }
    if (mine.empty() && others.empty()) {
        return unassigned;
    }
    Lines lines;
    for (const auto& [label, group] : {std::pair<std::string_view, const Lines*>{"Mine", &mine},
                                       {"Others", &others},
                                       {"Unassigned", &unassigned}}) {
        if (group->empty()) {
            continue;
        }
        if (!lines.empty()) {
            lines.push_back("");
        }
        lines.push_back("**" + std::string(label) + "**");
        lines.push_back("");
        lines.insert(lines.end(), group->begin(), group->end());
    }
    return lines;
}

Lines risk_lines(const std::vector<Json>& items) {
    Lines lines;
    for (const Json& item : items) {
        const std::string text = py::strip(text_of(item));
        const Json& raw = get(mapping_or_empty(get(item, "data")), "severity");
        const std::string severity = py::strip(truthy(raw) ? detail::py_str(raw) : "");
        lines.push_back(severity.empty() ? "- " + text : "- **[" + severity + "]** " + text);
    }
    return lines;
}

Lines timeline_lines(const std::vector<Json>& items) {
    Lines lines;
    for (const Json& item : items) {
        const std::string text = py::strip(text_of(item));
        const Json& start = get(mapping_or_empty(get(item, "data")), "start_s");
        lines.push_back(start.is_number() ? "- [" + format_mmss(number(start)) + "] " + text
                                          : "- " + text);
    }
    return lines;
}

// Note-taker blocks in page order: "**[mm:ss] heading** — body"; either part may be absent.
Lines live_note_lines(const std::vector<Json>& items, const Clock& clock) {
    std::vector<std::pair<double, const Json*>> ordered;
    for (const Json& item : items) {
        const Json& start = get(mapping_or_empty(get(item, "data")), "start_s");
        double key = 0.0;
        if (start.is_number() && std::isfinite(number(start))) {
            key = number(start);
        } else if (const auto created =
                       detail::meeting_timestamp(get(item, "created_at"), *clock.zone())) {
            key = *created;
        }
        ordered.emplace_back(key, &item);
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    Lines lines;
    for (const auto& [key, item] : ordered) {
        const std::string text = detail::join(py::split(text_of(*item)), " ");
        const Json& data = mapping_or_empty(get(*item, "data"));
        const Json& raw_heading = get(data, "heading");
        const std::string heading =
            py::strip(truthy(raw_heading) ? detail::py_str(raw_heading) : "");
        const Json& start = get(data, "start_s");
        const std::string stamp = start.is_number() ? format_mmss(number(start)) : "";
        Lines parts;
        for (const auto& part : {stamp, heading}) {
            if (!part.empty()) {
                parts.push_back(part);
            }
        }
        const std::string prefix =
            parts.empty() ? ""
                          : "**" + detail::join(parts, " ") + "** " + std::string(kEmDash) + " ";
        lines.push_back("- " + prefix + text);
    }
    return lines;
}

// Key points stamped "[mm:ss]" from their earliest evidence, so the document can be read
// against the transcript without raw segment ids.
Lines key_point_lines(const std::vector<Json>& items, const std::vector<Json>& segments) {
    const auto index = segment_index(segments);
    Lines lines;
    for (const Json& item : items) {
        const std::string text = py::strip(text_of(item));
        std::optional<double> earliest;
        for (const Json& segment_id : list_or_empty(get(item, "evidence"))) {
            require_hashable(segment_id);
            if (!segment_id.is_string()) {
                continue;
            }
            const auto it = index.find(segment_id.get_ref<const std::string&>());
            if (it == index.end()) {
                continue;
            }
            const auto start = float_of(get(*it->second, "start_s"), false);
            if (!start) {
                continue;
            }
            earliest = earliest ? std::min(*earliest, *start) : *start;
        }
        lines.push_back(earliest ? "- [" + format_mmss(*earliest) + "] " + text : "- " + text);
    }
    return lines;
}

Lines card_lines(std::string_view card, const std::vector<Json>& items, const Json& participants,
                 const std::vector<Json>& segments, const Clock& clock) {
    if (card == "action_items") {
        return action_item_lines(items, participants);
    }
    if (card == "risks") {
        return risk_lines(items);
    }
    if (card == "timeline") {
        return timeline_lines(items);
    }
    if (card == "key_points") {
        return key_point_lines(items, segments);
    }
    if (card == "live_notes") {
        return live_note_lines(items, clock);
    }
    Lines lines;
    for (const Json& item : items) {
        lines.push_back("- " + py::strip(text_of(item)));
    }
    return lines;
}

void append_cards(Lines& out, const Json& state, const Json& participants,
                  const std::vector<Json>& segments, const Clock& clock) {
    for (const auto& [card, heading] : kCardSections) {
        const auto items = live_items(state, card);
        if (items.empty()) {
            continue;
        }
        out.insert(out.end(), {"", "## " + std::string(heading), ""});
        const Lines lines = card_lines(card, items, participants, segments, clock);
        out.insert(out.end(), lines.begin(), lines.end());
    }
}

// Questions

// "answered from audio" or "answered by <name>".
std::string answer_badge(const Json& question, const Json& participants) {
    if (is(get(question, "answer_source"), "audio")) {
        return "answered from audio";
    }
    const Json& resolved_by = get(question, "resolved_by");
    const Json key = truthy(resolved_by) ? resolved_by : Json("");
    require_hashable(key);
    const Json& resolver =
        key.is_string() ? get(participants, key.get_ref<const std::string&>()) : get(Json(), "");
    const std::string name =
        py::strip(detail::text_or_empty(get(mapping_or_empty(resolver), "display_name")));
    return name.empty() ? "answered by participant" : "answered by " + name;
}

// format(confidence, ".0%"): the value times 100, rounded as Python formats floats.
std::string percent(const Json& confidence) {
    return py::format_fixed(number(confidence) * 100.0, 0) + "%";
}

// Open questions with their suggestions and resolved ones with answers and attribution;
// dismissed questions are left out.
void append_questions(Lines& out, const Json& state, const Json& participants) {
    const Json questions = list_or_empty(get(state, "questions"));
    std::vector<const Json*> open;
    std::vector<const Json*> resolved;
    for (const Json& question : questions) {
        const Json& status = get(detail::mapping(question), "status");
        const bool has_text = !py::strip(detail::text_or_empty(get(question, "text"))).empty();
        if (is(status, "open") && has_text) {
            open.push_back(&question);
        }
    }
    for (const Json& question : questions) {
        const Json& status = get(question, "status");
        const bool has_text = !py::strip(detail::text_or_empty(get(question, "text"))).empty();
        if (is(status, "resolved") && has_text) {
            resolved.push_back(&question);
        }
    }
    if (open.empty() && resolved.empty()) {
        return;
    }
    out.insert(out.end(), {"", "## Questions", ""});
    for (const Json* question : open) {
        out.push_back("- **Open:** " + py::strip(text_of(*question)));
        const std::string suggestion =
            py::strip(detail::text_or_empty(get(*question, "suggested_answer")));
        if (!suggestion.empty()) {
            const Json& confidence = get(*question, "suggested_confidence");
            const std::string note =
                confidence.is_number() ? " (" + percent(confidence) + " confidence)" : "";
            out.push_back("  - Suggested answer" + note + ": " + suggestion);
        }
    }
    for (const Json* question : resolved) {
        out.push_back("- **Resolved:** " + py::strip(text_of(*question)));
        const std::string answer = py::strip(detail::text_or_empty(get(*question, "answer")));
        if (!answer.empty()) {
            out.push_back("  - Answer: " + answer + " " + std::string(kEmDash) + " _" +
                          answer_badge(*question, participants) + "_");
        }
    }
}

// Reports

// A report's own opening "# " line is dropped when it repeats the section heading.
std::string strip_leading_title(const std::string& markdown, const std::string& title) {
    const auto lines = detail::splitlines(markdown);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string line = py::strip(lines[index]);
        if (line.empty()) {
            continue;
        }
        if (line.starts_with("# ") && py::strip(line.substr(2)) == title) {
            const Lines rest(lines.begin() + static_cast<std::ptrdiff_t>(index) + 1, lines.end());
            return py::lstrip(detail::join(rest, "\n"), "\n");
        }
        return markdown;
    }
    return markdown;
}

// The reports participants asked for, each under its own heading. Only finished ones: a
// failed or running report is a dashboard state, not part of a document being circulated.
void append_custom_reports(Lines& out, const Json& state) {
    std::vector<const Json*> reports;
    const Json listed = list_or_empty(get(state, "custom_reports"));
    for (const Json& report : listed) {
        if (report.is_object() && is(get(report, "status"), "ready") &&
            !py::strip(detail::text_or_empty(get(report, "markdown"))).empty()) {
            reports.push_back(&report);
        }
    }
    if (reports.empty()) {
        return;
    }
    out.insert(out.end(), {"", "## Requested Reports"});
    for (const Json* report : reports) {
        const Json& raw_request = get(*report, "request");
        const std::string request =
            detail::join(py::split(truthy(raw_request) ? detail::py_str(raw_request) : ""), " ");
        const Json& raw_title = get(*report, "title");
        std::string title = py::strip(truthy(raw_title) ? detail::py_str(raw_title) : "");
        if (title.empty()) {
            title = request.empty() ? "Report" : request;
        }
        out.insert(out.end(), {"", "### " + title, ""});
        if (!request.empty()) {
            out.insert(out.end(), {"> Requested: " + request, ""});
        }
        out.push_back(demote_headings(strip_leading_title(
            py::strip(detail::py_str(detail::at(*report, "markdown"))), title)));
    }
}

} // namespace

std::string demote_headings(std::string_view markdown, int offset) {
    static const py::Regex heading(R"(^(#{1,6})(\s))");
    static const py::Regex fence_re(R"(^\s*(```|~~~))");
    Lines lines;
    std::string fence;
    for (const auto& line : detail::splitlines(markdown)) {
        if (const auto fence_match = fence_re.match(line)) {
            const std::string marker = fence_match->group(1);
            fence = fence == marker ? std::string() : (fence.empty() ? marker : fence);
            lines.push_back(line);
            continue;
        }
        const auto match = fence.empty() ? heading.match(line) : std::nullopt;
        if (match) {
            const int depth = std::min(6, static_cast<int>(match->group(1).size()) + offset);
            lines.push_back(std::string(static_cast<std::size_t>(std::max(depth, 0)), '#') +
                            line.substr(match->byte_end(1)));
        } else {
            lines.push_back(line);
        }
    }
    return detail::join(lines, "\n");
}

std::string export_markdown(const Json& meeting, const Json& state,
                            const std::vector<Json>& segments, bool include_transcript,
                            bool include_intelligence, const Clock& clock) {
    const Json& raw_participants = get(state, "participants");
    const Json participants = truthy(raw_participants) ? raw_participants : Json::object();
    Lines out{"# " + resolve_title(meeting, state, clock)};

    const std::string metadata = metadata_line(meeting, state, segments, clock);
    if (!metadata.empty()) {
        out.insert(out.end(), {"", metadata});
    }
    append_intent(out, state);

    if (include_intelligence) {
        append_clarifications(out, state);
        append_topic(out, state, clock);
        append_summary(out, state);
        append_highlights(out, state);
        append_cards(out, state, participants, segments, clock);
        append_questions(out, state, participants);
        append_custom_reports(out, state);
    }

    if (include_transcript) {
        const auto lines = transcript_lines(segments, participants);
        if (!lines.empty()) {
            out.insert(out.end(), {"", "### Transcript", ""});
            // A trailing double space is a Markdown hard line break per segment.
            for (const auto& line : lines) {
                out.push_back(line + "  ");
            }
        }
    }
    return detail::join(out, "\n") + "\n";
}

} // namespace fv::meeting_state
