#include "meeting_state/repair.h"

#include <algorithm>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "base/log.h"
#include "meeting_state/content.h"
#include "meeting_state/detail/extras.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/patches.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.state.repair");

using detail::get;
using detail::truthy;

constexpr std::size_t kMaxEvidence = 20;
constexpr std::size_t kMaxTopic = 500;
constexpr std::size_t kMaxSummary = 8000;
constexpr std::size_t kSummaryItems = 6;
constexpr std::size_t kMinCoverageTokens = 3;
constexpr std::size_t kMinTokenLength = 4;
constexpr double kCoveredJaccard = 0.45;

using SegmentIndex = std::map<std::string, const Json*, std::less<>>;

bool is(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

// (value or []) iterated.
Json list_or_empty(const Json& value) {
    if (!truthy(value)) {
        return Json::array();
    }
    return detail::py_list(value);
}

// _live_items: the card's items that are not removed.
std::vector<Json> live_items(const Json& state, std::string_view card) {
    const Json& cards = get(state, "cards");
    std::vector<Json> out;
    for (const Json& item :
         list_or_empty(truthy(cards) ? get(detail::mapping(cards), card) : Json())) {
        if (!is(get(detail::mapping(item), "status"), "removed")) {
            out.push_back(item);
        }
    }
    return out;
}

// _synthesized_items: unreviewed legacy transcript samples a repair once wrote are not insights.
std::vector<Json> synthesized_items(const Json& state, std::string_view card) {
    std::vector<Json> out;
    for (Json& item : live_items(state, card)) {
        const bool repaired = is(get(item, "author_type"), "system") &&
                              is(get(item, "author_id"), "state_repair") &&
                              !is(get(item, "status"), "edited") &&
                              !is(get(item, "status"), "confirmed") && !truthy(get(item, "pinned"));
        bool sample = false;
        if (repaired) {
            // (data or {}).get("insight_synthesized") is not True: only the bool True marks a
            // repair the agent later synthesized.
            const Json& data = get(item, "data");
            const Json& flag =
                truthy(data) ? get(detail::mapping(data), "insight_synthesized") : get(Json(), "");
            sample = !(flag.is_boolean() && flag.get<bool>());
        }
        if (!sample) {
            out.push_back(std::move(item));
        }
    }
    return out;
}

SegmentIndex segment_index(const std::vector<Json>& segments) {
    SegmentIndex index;
    for (const Json& segment : segments) {
        if (!segment.is_object()) {
            continue;
        }
        const Json& id = get(segment, "id");
        if (truthy(id) && id.is_string()) {
            index[id.get<std::string>()] = &segment;
        }
    }
    return index;
}

const Json* indexed(const SegmentIndex& index, const Json& segment_id) {
    if (segment_id.is_array() || segment_id.is_object()) {
        throw py::TypeError("unhashable type");
    }
    if (!segment_id.is_string()) {
        return nullptr;
    }
    const auto it = index.find(segment_id.get_ref<const std::string&>());
    return it == index.end() ? nullptr : it->second;
}

// _earliest_start: the earliest start_s of the evidence segments; unreadable ones are skipped.
std::optional<double> earliest_start(const Json& evidence, const SegmentIndex& index) {
    std::optional<double> earliest;
    for (const Json& segment_id : list_or_empty(evidence)) {
        const Json* segment = indexed(index, segment_id);
        if (segment == nullptr) {
            continue;
        }
        const Json& start = get(*segment, "start_s");
        const std::optional<double> value = truthy(start) ? py::py_float(start) : 0.0;
        if (!value) {
            continue;
        }
        earliest = earliest ? std::min(*earliest, *value) : *value;
    }
    return earliest;
}

// _clip_text: one line, at most MAX_BEAT_TEXT code points.
std::string clip_text(const std::string& text) {
    const std::string line = py::strip(detail::replace_all(text, "\n", " "));
    if (py::len(line) <= MAX_BEAT_TEXT) {
        return line;
    }
    return py::rstrip(py::slice(line, 0, static_cast<std::int64_t>(MAX_BEAT_TEXT) - 3)) + "...";
}

Json first_ids(const Json& evidence, std::size_t limit) {
    Json out = Json::array();
    for (const Json& id : evidence) {
        if (out.size() >= limit) {
            break;
        }
        out.push_back(id);
    }
    return out;
}

// list(dict.fromkeys(evidence))[:limit].
Json unique_ids(const Json& evidence, std::size_t limit) {
    Json unique = Json::array();
    for (const Json& id : evidence) {
        if (id.is_array() || id.is_object()) {
            throw py::TypeError("unhashable type");
        }
        if (std::none_of(unique.begin(), unique.end(),
                         [&](const Json& seen) { return py::py_equal(seen, id); })) {
            unique.push_back(id);
        }
    }
    return first_ids(unique, limit);
}

Json timeline_add_op(const std::string& text, double start_s, const Json& evidence) {
    Json op = Json::object();
    op["op"] = "add_item";
    op["card"] = "timeline";
    op["text"] = text;
    op["data"] = Json{{"start_s", start_s}};
    op["evidence"] = first_ids(evidence, kMaxEvidence);
    return op;
}

double start_of(const Json& op) {
    return op.at("data").at("start_s").get<double>();
}

void sort_by_start(std::vector<Json>& ops) {
    std::stable_sort(ops.begin(), ops.end(),
                     [](const Json& a, const Json& b) { return start_of(a) < start_of(b); });
}

Json to_array(std::vector<Json> ops, std::size_t limit) {
    Json out = Json::array();
    for (auto& op : ops) {
        if (out.size() >= limit) {
            break;
        }
        out.push_back(std::move(op));
    }
    return out;
}

// _key_point_beats: synthesized key points with timed evidence.
std::vector<Json> key_point_beats(const Json& state, const SegmentIndex& index) {
    std::vector<Json> ops;
    for (const Json& item : synthesized_items(state, "key_points")) {
        const Json evidence = list_or_empty(get(item, "evidence"));
        const auto start_s = earliest_start(evidence, index);
        if (!start_s) {
            continue;
        }
        const std::string text = clip_text(detail::text_or_empty(get(item, "text")));
        if (text.empty()) {
            continue;
        }
        ops.push_back(timeline_add_op(text, *start_s, evidence));
        if (ops.size() >= MAX_TIMELINE_BEATS) {
            break;
        }
    }
    return ops;
}

// _note_beat_text: "heading: text" unless the text already opens with its heading.
std::string note_beat_text(const Json& data, const Json& item) {
    const Json& raw_heading = get(data, "heading");
    const std::string heading = py::strip(truthy(raw_heading) ? detail::py_str(raw_heading) : "");
    const std::string item_text = py::strip(detail::text_or_empty(get(item, "text")));
    if (!heading.empty() && !item_text.starts_with(heading)) {
        return heading + ": " + item_text;
    }
    return heading.empty() ? item_text : heading;
}

// float(value) for a note's own start_s, which nothing caught in Python.
double float_or_throw(const Json& value) {
    if (const auto parsed = py::py_float(value)) {
        return *parsed;
    }
    if (value.is_string()) {
        throw py::ValueError("could not convert string to float");
    }
    throw py::TypeError("float() argument must be a string or a real number");
}

// _note_beats: note blocks whose evidence is in the transcript.
std::vector<Json> note_beats(const Json& state, const SegmentIndex& index) {
    std::vector<Json> ops;
    for (const Json& item : live_items(state, NOTES_CARD)) {
        const Json& raw_data = get(item, "data");
        const Json data = truthy(raw_data) ? detail::mapping(raw_data) : Json::object();
        Json evidence = Json::array();
        for (const Json& segment_id : list_or_empty(get(item, "evidence"))) {
            if (indexed(index, segment_id) != nullptr) {
                evidence.push_back(segment_id);
            }
        }
        if (evidence.empty()) {
            continue;
        }
        const Json& stated = get(data, "start_s");
        std::optional<double> start_s;
        if (stated.is_null() || stated.is_boolean()) {
            start_s = earliest_start(evidence, index);
        } else {
            start_s = float_or_throw(stated);
        }
        if (!start_s) {
            continue;
        }
        const std::string text = clip_text(note_beat_text(data, item));
        if (text.empty()) {
            continue;
        }
        ops.push_back(timeline_add_op(text, *start_s, evidence));
        if (ops.size() >= MAX_TIMELINE_BEATS) {
            break;
        }
    }
    return ops;
}

// _normalize_tokens: words of four or more code points, lowercased, split at anything that is
// not alphanumeric.
std::set<std::string> normalize_tokens(const std::string& text) {
    std::string spaced;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t start = pos;
        const char32_t cp = py::next_code_point(text, pos);
        if (py::is_alnum(cp)) {
            spaced += py::lower(std::string_view(text).substr(start, pos - start));
        } else {
            spaced += ' ';
        }
    }
    std::set<std::string> tokens;
    for (auto& token : py::split(spaced)) {
        if (py::len(token) >= kMinTokenLength) {
            tokens.insert(std::move(token));
        }
    }
    return tokens;
}

double jaccard(const std::set<std::string>& left, const std::set<std::string>& right) {
    std::size_t shared = 0;
    for (const auto& token : left) {
        shared += right.contains(token) ? 1 : 0;
    }
    const std::size_t total = left.size() + right.size() - shared;
    return static_cast<double>(shared) / static_cast<double>(total);
}

// _summary_from_items: the first six items' texts as sentences; their evidence is collected.
std::string summary_from_items(const std::vector<Json>& items, Json& evidence) {
    std::vector<std::string> sentences;
    for (std::size_t i = 0; i < items.size() && i < kSummaryItems; ++i) {
        const std::string text = py::strip(detail::text_or_empty(get(items[i], "text")));
        if (text.empty()) {
            continue;
        }
        sentences.push_back(py::rstrip(text, ".") + ".");
        for (const Json& id : list_or_empty(get(items[i], "evidence"))) {
            evidence.push_back(id);
        }
    }
    return py::strip(detail::join(sentences, " "));
}

std::optional<Json> snapshot_or_none(MeetingStateStore& store, std::string_view failure) {
    try {
        return store.snapshot();
    } catch (const std::exception& error) {
        logger.error_with(error, failure);
        return std::nullopt;
    }
}

// _apply_repair: one batch as system/state_repair; how many ops applied.
int apply_repair(MeetingStateStore& store, const Json& ops, std::string_view label) {
    if (ops.empty()) {
        return 0;
    }
    try {
        int applied = 0;
        for (const auto& result : store.apply("system", std::string("state_repair"), ops)) {
            applied += result.ok ? 1 : 0;
        }
        return applied;
    } catch (const std::exception& error) {
        logger.error_with(error, std::string(label) + " portion of state repair failed");
        return 0;
    }
}

Json chained(Json first, const Json& second) {
    for (const Json& op : second) {
        first.push_back(op);
    }
    return first;
}

} // namespace

Json build_timeline_backfill_ops(const Json& state, const std::vector<Json>& segments) {
    if (!live_items(state, "timeline").empty()) {
        return Json::array();
    }
    const SegmentIndex index = segment_index(segments);
    std::vector<Json> ops = key_point_beats(state, index);
    if (ops.empty()) {
        ops = note_beats(state, index);
    }
    sort_by_start(ops);
    return to_array(std::move(ops), MAX_TIMELINE_BEATS);
}

Json build_timeline_coverage_from_segments(const Json& state, const std::vector<Json>& segments) {
    const SegmentIndex index = segment_index(segments);
    const std::vector<Json> key_points = synthesized_items(state, "key_points");
    const std::vector<Json> timeline = live_items(state, "timeline");
    if (key_points.empty()) {
        return Json::array();
    }
    std::vector<std::set<std::string>> covered;
    for (const Json& item : timeline) {
        covered.push_back(normalize_tokens(detail::text_or_empty(get(item, "text"))));
    }
    std::vector<Json> ops;
    for (const Json& item : key_points) {
        const std::string text = detail::text_or_empty(get(item, "text"));
        std::set<std::string> tokens = normalize_tokens(text);
        if (tokens.size() < kMinCoverageTokens) {
            continue;
        }
        const bool already = std::any_of(covered.begin(), covered.end(), [&](const auto& other) {
            return !other.empty() && jaccard(tokens, other) >= kCoveredJaccard;
        });
        if (already) {
            continue;
        }
        const Json evidence = list_or_empty(get(item, "evidence"));
        const auto start_s = earliest_start(evidence, index);
        if (!start_s) {
            continue;
        }
        ops.push_back(timeline_add_op(clip_text(text), *start_s, evidence));
        covered.push_back(std::move(tokens));
        if (timeline.size() + ops.size() >= MAX_TIMELINE_BEATS) {
            break;
        }
    }
    sort_by_start(ops);
    const std::size_t count = ops.size();
    return to_array(std::move(ops), count);
}

Json build_topic_backfill_ops(const Json& state, const std::vector<Json>& segments) {
    (void)segments;
    const Json& topic = get(state, "topic");
    const std::string current = py::strip(
        detail::text_or_empty(truthy(topic) ? get(detail::mapping(topic), "current") : Json()));
    if (!current.empty()) {
        return Json::array();
    }
    const std::vector<Json> key_points = synthesized_items(state, "key_points");
    const std::vector<Json> live_notes = live_items(state, NOTES_CARD);
    std::string text;
    Json evidence;
    if (!key_points.empty()) {
        text = clip_text(detail::text_or_empty(get(key_points[0], "text")));
        evidence = list_or_empty(get(key_points[0], "evidence"));
    } else if (!live_notes.empty()) {
        const Json& first_note = live_notes[0];
        const Json& raw_data = get(first_note, "data");
        const Json data = truthy(raw_data) ? detail::mapping(raw_data) : Json::object();
        const Json& raw_heading = get(data, "heading");
        const std::string heading =
            py::strip(truthy(raw_heading) ? detail::py_str(raw_heading) : "");
        text =
            clip_text(heading.empty() ? detail::text_or_empty(get(first_note, "text")) : heading);
        evidence = list_or_empty(get(first_note, "evidence"));
    } else {
        return Json::array();
    }
    if (text.empty()) {
        return Json::array();
    }
    Json op = Json::object();
    op["op"] = "set_topic";
    op["text"] = py::slice(text, 0, static_cast<std::int64_t>(kMaxTopic));
    if (!evidence.empty()) {
        op["evidence"] = unique_ids(evidence, kMaxEvidence);
    }
    return Json::array({std::move(op)});
}

Json build_summary_backfill_ops(const Json& state, const std::vector<Json>& segments) {
    (void)segments;
    if (!py::strip(detail::text_or_empty(get(state, "rolling_summary"))).empty()) {
        return Json::array();
    }
    const std::vector<Json> key_points = synthesized_items(state, "key_points");
    const std::vector<Json> live_notes = live_items(state, NOTES_CARD);
    Json evidence = Json::array();
    std::string summary;
    if (!key_points.empty()) {
        summary = summary_from_items(key_points, evidence);
    } else if (!live_notes.empty()) {
        summary = summary_from_items(live_notes, evidence);
    } else {
        return Json::array();
    }
    if (summary.empty()) {
        return Json::array();
    }
    // Agent evidence discipline when anchors exist: host and system may omit evidence, but it
    // keeps exports and the dashboard's jumps useful.
    Json op = Json::object();
    op["op"] = "set_rolling_summary";
    op["text"] = py::slice(summary, 0, static_cast<std::int64_t>(kMaxSummary));
    if (!evidence.empty()) {
        op["evidence"] = unique_ids(evidence, kMaxEvidence);
    }
    return Json::array({std::move(op)});
}

int repair_meeting_state(MeetingStateStore& store, const std::vector<Json>& segments) {
    int applied = 0;
    auto snapshot = snapshot_or_none(store, "State repair could not snapshot state");
    if (!snapshot) {
        return 0;
    }
    const Json& stored_views = get(*snapshot, "report_views");
    const Json views =
        truthy(stored_views) ? stored_views : Json::array({"ribbon", "brief", "signal"});
    const bool ribbon = std::any_of(views.begin(), views.end(),
                                    [](const Json& view) { return is(view, "ribbon"); });
    if (ribbon) {
        applied +=
            apply_repair(store, build_timeline_backfill_ops(*snapshot, segments), "timeline");
        snapshot = snapshot_or_none(store, "State repair could not re-snapshot after timeline");
        if (!snapshot) {
            return applied;
        }
        applied += apply_repair(store, build_timeline_coverage_from_segments(*snapshot, segments),
                                "timeline_coverage");
    }
    snapshot =
        snapshot_or_none(store, "State repair could not re-snapshot after timeline coverage");
    if (!snapshot) {
        return applied;
    }
    applied += apply_repair(store,
                            chained(build_summary_backfill_ops(*snapshot, segments),
                                    build_topic_backfill_ops(*snapshot, segments)),
                            "summary/topic");
    // The topic becomes a blank title only once the meeting is terminal, so the repair at the
    // end of capture cannot lock in a live draft's topic.
    snapshot = snapshot_or_none(store, "State repair could not re-snapshot after topic");
    if (!snapshot) {
        return applied;
    }
    const Json& status = get(*snapshot, "status");
    const std::string terminal = truthy(status) ? detail::py_str(status) : "";
    if (terminal == "ended" || terminal == "needs_recovery" || terminal == "failed") {
        applied += apply_repair(store, build_untitled_title_ops(*snapshot), "title");
    }
    if (applied != 0) {
        logger.info("State repair applied {} op(s)", applied);
    }
    return applied;
}

} // namespace fv::meeting_state
