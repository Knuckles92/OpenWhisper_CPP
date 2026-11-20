#include "meeting_agent/prompts.h"

#include <algorithm>
#include <format>
#include <set>
#include <string_view>

#include "base/identity.h"
#include "meeting_agent/detail/prompt_text.h"
#include "meeting_agent/detail/py_value.h"
#include "meeting_state/patches.h"
#include "meeting_state/schema.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_agent {

namespace {

using detail::get;
using detail::get_object;
using detail::get_or;
using detail::is_text;
using detail::py_str;
using detail::py_truthy;
using detail::str_or_empty;

constexpr std::size_t kMaxItemTextChars = 240;
constexpr std::size_t kMaxNoteTextChars = 600;

// f"{x:g}" for the confidence thresholds.
std::string format_g(double value) {
    return std::format("{:g}", value);
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
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

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            if (start < text.size()) {
                lines.emplace_back(text.substr(start));
            }
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

// (text or "").replace("\n", " ").strip()
std::string one_line(const Json& text) {
    return py::strip(replace_all(str_or_empty(text), "\n", " "));
}

std::string truncate(std::string text, std::size_t max_len) {
    if (py::len(text) > max_len) {
        text = py::slice(text, 0, static_cast<std::int64_t>(max_len) - 3) + "...";
    }
    return text;
}

// float(value or 0.0), raising as Python does for text that is not a number.
double float_or_zero(const Json& value) {
    if (!py_truthy(value)) {
        return 0.0;
    }
    if (const auto number = py::py_float(value)) {
        return *number;
    }
    if (value.is_string()) {
        throw py::ValueError("could not convert string to float: " + detail::py_repr(value));
    }
    throw py::TypeError("float() argument must be a string or a real number");
}

bool is_number(const Json& value) {
    return value.is_number() || value.is_boolean();
}

std::vector<std::string> item_flags(const Json& item) {
    std::vector<std::string> flags{"rev=" + py_str(get_or(item, "revision", Json(1))),
                                   py_str(get_or(item, "status", Json("proposed")))};
    if (py_truthy(get(item, "pinned"))) {
        flags.emplace_back("pinned");
    }
    if (is_text(get(item, "author_type"), "system") &&
        is_text(get(item, "author_id"), "voice_command")) {
        flags.emplace_back("author_type=system author_id=voice_command protected");
    }
    return flags;
}

std::string render_line(const Json& item, const std::vector<std::string>& extras,
                        std::string text) {
    const std::string extra_txt = extras.empty() ? "" : " (" + join(extras, ", ") + ")";
    return "- [" + py_str(get(item, "id")) + " " + join(item_flags(item), " ") + "]" + extra_txt +
           " " + text;
}

std::string render_item(const Json& item, std::string_view card) {
    const Json& data = get_object(item, "data");
    std::vector<std::string> extras;
    if (py_truthy(get(data, "heading"))) {
        extras.push_back("heading=" + py_str(get(data, "heading")));
    }
    if (py_truthy(get(data, "owner_participant_id"))) {
        extras.push_back("owner=" + py_str(get(data, "owner_participant_id")));
    }
    if (!get(data, "start_s").is_null()) {
        extras.push_back("start_s=" + py_str(get(data, "start_s")));
    }
    if (py_truthy(get(data, "severity"))) {
        extras.push_back("severity=" + py_str(get(data, "severity")));
    }
    const bool long_text =
        card == "live_notes" || card == "user_notes" || py_truthy(get(data, "heading"));
    return render_line(
        item, extras,
        truncate(one_line(get(item, "text")), long_text ? kMaxNoteTextChars : kMaxItemTextChars));
}

std::string render_note_item(const Json& item) {
    const Json& data = get_object(item, "data");
    std::vector<std::string> extras;
    if (py_truthy(get(data, "heading"))) {
        extras.push_back("heading=" + py_str(get(data, "heading")));
    }
    if (!get(data, "start_s").is_null()) {
        extras.push_back("start_s=" + py_str(get(data, "start_s")));
    }
    return render_line(item, extras, truncate(one_line(get(item, "text")), kMaxNoteTextChars));
}

// The card's items that are not removed.
std::vector<Json> live_items(const Json& cards, std::string_view card) {
    std::vector<Json> items;
    const Json& listed = get(cards, card);
    if (listed.is_array()) {
        for (const auto& item : listed) {
            if (item.is_object() && !is_text(get(item, "status"), "removed")) {
                items.push_back(item);
            }
        }
    }
    return items;
}

std::string question_guidance(const Json& state) {
    using meeting_state::MAX_OPEN_QUESTIONS;
    using meeting_state::RESOLVE_CONFIDENCE;
    using meeting_state::SUGGEST_CONFIDENCE;
    std::vector<Json> questions;
    const Json& listed = get(state, "questions");
    if (listed.is_array()) {
        for (const auto& question : listed) {
            if (question.is_object() && is_text(get(question, "status"), "open")) {
                questions.push_back(question);
            }
        }
    }
    std::ranges::stable_sort(questions, [](const Json& a, const Json& b) {
        return str_or_empty(get(a, "asked_at")) < str_or_empty(get(b, "asked_at"));
    });
    std::vector<std::string> lines;
    if (questions.empty()) {
        lines.emplace_back("Open questions: none.");
    } else {
        lines.push_back(
            std::format("Open questions ({}/{}):", questions.size(), MAX_OPEN_QUESTIONS));
        for (const auto& question : questions) {
            std::string line = "- [" + py_str(get(question, "id")) + "] " +
                               py_str(get_or(question, "text", Json("")));
            const Json& suggested = get(question, "suggested_answer");
            if (py_truthy(suggested)) {
                const Json& confidence = get(question, "suggested_confidence");
                const std::string confidence_txt =
                    is_number(confidence)
                        ? py::format_fixed(confidence.is_boolean()
                                               ? (confidence.get<bool>() ? 1.0 : 0.0)
                                               : confidence.get<double>(),
                                           2)
                        : "?";
                line += " — unconfirmed suggested answer (confidence " + confidence_txt +
                        "): " + py_str(suggested);
            }
            lines.push_back(std::move(line));
        }
    }
    const std::size_t capacity =
        questions.size() < MAX_OPEN_QUESTIONS ? MAX_OPEN_QUESTIONS - questions.size() : 0;
    lines.push_back(std::format(
        "You may open {} more question(s). resolve_question with confidence >= {} marks a "
        "question answered from audio; {}-{} records a greyed suggestion; lower is "
        "rejected. Prefer firming up questions that already have suggested "
        "answers over asking new ones.",
        capacity, format_g(RESOLVE_CONFIDENCE), format_g(SUGGEST_CONFIDENCE),
        format_g(RESOLVE_CONFIDENCE)));
    return join(lines, "\n");
}

std::string topic_line(const Json& state) {
    const Json& topic = get(get_object(state, "topic"), "current");
    return "Current topic: " + (py_truthy(topic) ? py_str(topic) : std::string("(not set)"));
}

std::string summary_line(const Json& state) {
    const Json& summary = get(state, "rolling_summary");
    return "Rolling summary: " + (py_truthy(summary) ? py_str(summary) : std::string("(not set)"));
}

void append_segments(std::vector<std::string>& parts, const std::vector<Json>& segments,
                     const Json& participants) {
    if (segments.empty()) {
        parts.emplace_back("(no new segments)");
        return;
    }
    for (const auto& segment : segments) {
        parts.push_back(format_segment_line(segment, participants));
    }
}

void lead_with_brief_and_guidance(std::vector<std::string>& parts, const Json& state) {
    // Skipped when unset, so an absent brief adds no leading blank line; the guidance block is
    // always appended, so an empty one still adds its line.
    std::string brief = intent_prompt(state);
    if (!brief.empty()) {
        parts.push_back(std::move(brief));
    }
    parts.push_back(guidance_prompt(state));
}

std::string fill_policy(std::string_view text) {
    std::string out = replace_all(std::string(text), "{product}", identity().product);
    out = replace_all(std::move(out), "{max_evidence}",
                      std::to_string(meeting_state::MAX_EVIDENCE_REFS));
    out = replace_all(std::move(out), "{max_open_questions}",
                      std::to_string(meeting_state::MAX_OPEN_QUESTIONS));
    out = replace_all(std::move(out), "{resolve}", format_g(meeting_state::RESOLVE_CONFIDENCE));
    return replace_all(std::move(out), "{suggest}", format_g(meeting_state::SUGGEST_CONFIDENCE));
}

} // namespace

std::string build_system_prompt() {
    return fill_policy(detail::kSystemPromptTemplate);
}

std::string build_note_taker_system_prompt() {
    return fill_policy(detail::kNoteTakerSystemPromptTemplate);
}

std::string build_consolidation_instructions(const Json& report_views) {
    std::set<std::string> views;
    for (const auto& view : detail::py_list_or_empty(report_views)) {
        views.insert(py_str(view));
    }
    if (views.empty()) {
        views = {"ribbon", "brief", "signal"};
    }
    std::vector<std::string_view> bodies;
    for (const auto& step : detail::kConsolidationSteps) {
        if (!step.view || views.contains(std::string(*step.view))) {
            bodies.push_back(step.body);
        }
    }
    std::vector<std::string> numbered;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const std::size_t index = i + 1;
        const std::string indent(index < 10 ? 3 : 4, ' ');
        const auto lines = split_lines(py::strip(bodies[i]));
        std::vector<std::string> out;
        out.push_back(std::format("{}. {}", index, lines.empty() ? "" : lines.front()));
        for (std::size_t j = 1; j < lines.size(); ++j) {
            out.push_back(py::strip(lines[j]).empty() ? lines[j] : indent + py::lstrip(lines[j]));
        }
        numbered.push_back(join(out, "\n"));
    }
    return std::string(detail::kConsolidationHeader) + join(numbered, "\n");
}

std::string intent_prompt(const Json& state) {
    const std::string text = py::strip(str_or_empty(get(get_object(state, "intent"), "text")));
    if (text.empty()) {
        return "";
    }
    return "## MEETING BRIEF (what the host wants from this meeting)\n" +
           std::string(detail::kIntentInstructions) + "\n\nHOST'S BRIEF:\n" + text;
}

std::string render_state_compact(const Json& state) {
    std::vector<std::string> lines;
    const Json& title = get(state, "title");
    lines.push_back("Title: " + (py_truthy(title) ? py_str(title) : std::string("(untitled)")));
    lines.push_back(topic_line(state));
    lines.push_back(summary_line(state));

    const Json& cards = get_object(state, "cards");
    const auto spotlight = select_spotlight_items(cards, 3);
    lines.emplace_back("");
    lines.push_back(
        std::format("Top Insights (Dashboard Spotlight - {} active):", spotlight.size()));
    if (spotlight.empty()) {
        lines.emplace_back("- (none yet)");
    }
    for (const auto& item : spotlight) {
        lines.push_back("- [" + py_str(get(item, "id")) + " " +
                        py_str(get_or(item, "status", Json("proposed"))) + "] [" +
                        py_str(get_or(item, "card", Json(""))) + "] " +
                        py_str(get_or(item, "text", Json(""))));
    }

    lines.emplace_back("");
    lines.emplace_back("Participants:");
    const Json& participants = get_object(state, "participants");
    if (participants.empty()) {
        lines.emplace_back("- (none yet)");
    }
    for (const auto& [pid, participant] : participants.items()) {
        std::vector<std::string> flags{py_str(get_or(participant, "kind", Json("")))};
        if (is_text(get(participant, "name_source"), "human")) {
            flags.emplace_back("named-by-human");
        } else if (py_truthy(get(participant, "is_provisional"))) {
            flags.emplace_back("provisional");
        }
        std::erase_if(flags, [](const std::string& flag) { return flag.empty(); });
        lines.push_back("- [" + pid + "] " + py_str(get_or(participant, "display_name", Json(""))) +
                        " (" + join(flags, ", ") + ")");
    }

    for (const auto card : meeting_state::CARD_KEYS) {
        const auto items = live_items(cards, card);
        const std::string label =
            card == "user_notes" ? std::string(card) + " [HUMAN-ONLY]" : std::string(card);
        lines.emplace_back("");
        lines.push_back(std::format("Card {} ({} items):", label, items.size()));
        if (items.empty()) {
            lines.emplace_back("- (empty)");
        }
        for (const auto& item : items) {
            lines.push_back(render_item(item, card));
        }
    }

    lines.emplace_back("");
    lines.push_back(question_guidance(state));
    return join(lines, "\n");
}

std::string format_segment_line(const Json& segment, const Json& participants) {
    const Json& pid = get(segment, "speaker_participant_id");
    std::string speaker;
    // Participant keys are text, so an id of another type finds nobody, as in Python.
    const Json* participant = nullptr;
    if (py_truthy(pid) && pid.is_string()) {
        participant = &get(participants, pid.get_ref<const std::string&>());
    }
    if (participant != nullptr && py_truthy(*participant)) {
        const Json& name = get(*participant, "display_name");
        speaker = py_truthy(name) ? py_str(name) : py_str(pid);
    } else {
        speaker = is_text(get(segment, "channel"), "mic") ? "Me" : "Others";
    }
    const double start_s = float_or_zero(get(segment, "start_s"));
    return "[" + py_str(get(segment, "id")) + "] [t=" + py::format_fixed(start_s, 1) + "s] [" +
           speaker + "]: " + one_line(get(segment, "text"));
}

std::string render_notes_page(const Json& state) {
    std::vector<std::string> lines{topic_line(state), summary_line(state)};
    const Json& cards = get_object(state, "cards");
    const auto blocks = live_items(cards, "live_notes");
    lines.emplace_back("");
    lines.push_back(std::format("Notes blocks ({}):", blocks.size()));
    if (blocks.empty()) {
        lines.emplace_back("- (page is empty — the first note starts it)");
    }
    for (const auto& block : blocks) {
        lines.push_back(render_note_item(block));
    }
    lines.emplace_back("\nUser notes (context, human-only):");
    for (const auto& note : live_items(cards, "user_notes")) {
        lines.push_back("- " + py_str(get_or(note, "text", Json(""))));
    }
    return join(lines, "\n");
}

std::string build_notes_user_prompt(const Json& state, const std::vector<Json>& new_segments) {
    const Json& participants = get_object(state, "participants");
    std::vector<std::string> parts;
    lead_with_brief_and_guidance(parts, state);
    parts.emplace_back("## CURRENT NOTES PAGE");
    parts.push_back(render_notes_page(state));
    parts.emplace_back("");
    parts.emplace_back("## NEW TRANSCRIPT SEGMENTS");
    append_segments(parts, new_segments, participants);
    parts.emplace_back("");
    const Json& request = get(state, "note_adjustment_request");
    if (py_truthy(request)) {
        parts.emplace_back("## NOTE ADJUSTMENT REQUEST");
        parts.push_back(py_str(request));
        parts.emplace_back(detail::kNoteAdjustmentInstruction);
    } else if (py_truthy(get(state, "notes_review_requested"))) {
        parts.emplace_back(detail::kReviewExistingNotes);
    } else {
        parts.emplace_back(detail::kNotesInstructions);
    }
    return join(parts, "\n");
}

std::string build_checkpoint_user_prompt(const Json& state, const std::vector<Json>& new_segments,
                                         bool is_consolidation, bool is_polish) {
    const Json& participants = get_object(state, "participants");
    std::vector<std::string> parts;
    lead_with_brief_and_guidance(parts, state);
    parts.emplace_back("## CURRENT DASHBOARD STATE");
    parts.push_back(render_state_compact(state));
    parts.emplace_back("");
    const Json& context = get(state, "recent_transcript_context");
    if (py_truthy(context) && context.is_array() && !(is_consolidation || is_polish)) {
        parts.emplace_back("## RECENT TRANSCRIPT CONTEXT (already seen; may correct)");
        for (const auto& segment : context) {
            parts.push_back(format_segment_line(segment, participants));
        }
        parts.emplace_back("");
    }
    if (is_polish) {
        parts.emplace_back("## TRANSCRIPT BLOCK FOR CLEANUP");
    } else if (is_consolidation) {
        parts.emplace_back("## FULL MEETING TRANSCRIPT");
    } else {
        parts.emplace_back("## NEW TRANSCRIPT SEGMENTS");
    }
    append_segments(parts, new_segments, participants);
    parts.emplace_back("");
    if (is_polish) {
        parts.emplace_back(detail::kPolishInstructions);
    } else if (is_consolidation) {
        parts.push_back(build_consolidation_instructions(get(state, "report_views")));
    } else {
        parts.emplace_back(detail::kCheckpointInstructions);
    }
    return join(parts, "\n");
}

} // namespace fv::meeting_agent
