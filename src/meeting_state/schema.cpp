#include "meeting_state/schema.h"

#include <algorithm>
#include <stdexcept>

#include "meeting_state/detail/py_value.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

using detail::get;
using detail::has;
using detail::string_or;
using detail::truthy;

constexpr std::array<std::string_view, 3> kTerminalMeetingStatuses = {"ended", "failed",
                                                                      "needs_recovery"};

template <std::size_t N>
bool one_of(const std::array<std::string_view, N>& names, std::string_view value) {
    return std::find(names.begin(), names.end(), value) != names.end();
}

bool one_of_json(const Json& value, std::initializer_list<std::string_view> names) {
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    return std::find(names.begin(), names.end(), text) != names.end();
}

// A required text key: d[key], with str() of another JSON type.
std::string required_str(const Json& d, std::string_view key) {
    const Json& value = detail::at(d, key);
    return value.is_string() ? value.get<std::string>() : detail::py_str(value);
}

std::optional<std::string> optional_str(const Json& d, std::string_view key) {
    const Json& value = get(d, key);
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::nullopt;
}

// str(d.get(key) or "").
std::string str_or_empty(const Json& d, std::string_view key) {
    const Json& value = get(d, key);
    return truthy(value) ? detail::py_str(value) : std::string();
}

// dict(value or {}).
Json dict_or_empty(const Json& value) {
    if (!truthy(value)) {
        return Json::object();
    }
    if (!value.is_object()) {
        throw py::TypeError("cannot convert dictionary update sequence element #0 to a sequence");
    }
    return value;
}

// list(value or []) keeping the text entries.
std::vector<std::string> strings_of(const Json& value) {
    std::vector<std::string> out;
    if (!truthy(value)) {
        return out;
    }
    for (const auto& entry : detail::py_list(value)) {
        if (entry.is_string()) {
            out.push_back(entry.get<std::string>());
        }
    }
    return out;
}

// d.get(key, now_iso()) for a timestamp.
std::string stamp_or_now(const Json& d, std::string_view key, const Clock& clock) {
    const Json& value = get(d, key);
    return value.is_string() ? value.get<std::string>() : utc_now_iso(clock);
}

// A JSON object's members iterated as Python iterates `value` in a for loop.
Json iterable(const Json& value) {
    return detail::py_list(value);
}

const Json& require_mapping(const Json& value) {
    if (!value.is_object()) {
        throw py::PyError("AttributeError", "object has no attribute 'get'");
    }
    return value;
}

Json optional_json(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json();
}

Json strings_json(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

} // namespace

std::optional<std::size_t> card_index(std::string_view card) {
    for (std::size_t i = 0; i < CARD_KEYS.size(); ++i) {
        if (CARD_KEYS[i] == card) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<Json> parse_state_json(const Json& raw) {
    if (raw.is_object()) {
        return std::optional<Json>(std::in_place, raw);
    }
    if (!truthy(raw) || !raw.is_string()) {
        return std::nullopt;
    }
    return parse_state_json(std::string_view(raw.get_ref<const std::string&>()));
}

std::optional<Json> parse_state_json(std::string_view raw) {
    if (raw.empty()) {
        return std::nullopt;
    }
    try {
        Json parsed = py::loads(raw);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const py::PyError&) {
    }
    return std::nullopt;
}

// CardItem

bool CardItem::is_protected() const {
    // Spoken requests stay proposed and reversible, but later summary passes must not
    // overwrite content a participant explicitly asked to save.
    const bool spoken = author_type == "system" && author_id == "voice_command";
    return spoken || pinned || status == "edited" || status == "confirmed";
}

Json CardItem::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["card"] = card;
    out["text"] = text;
    out["data"] = data;
    out["status"] = status;
    out["author_type"] = author_type;
    out["author_id"] = optional_json(author_id);
    out["pinned"] = pinned;
    out["revision"] = revision;
    out["evidence"] = strings_json(evidence);
    out["created_at"] = created_at;
    out["updated_at"] = updated_at;
    out["review"] = review;
    out["citation_check"] = citation_check;
    return out;
}

CardItem CardItem::from_json(const Json& d, const Clock& clock) {
    CardItem item;
    item.id = required_str(d, "id");
    item.card = required_str(d, "card");
    item.text = string_or(d, "text", "");
    item.review = dict_or_empty(get(d, "review"));
    item.citation_check = dict_or_empty(get(d, "citation_check"));
    item.data = dict_or_empty(get(d, "data"));
    item.status = string_or(d, "status", "proposed");
    item.author_type = string_or(d, "author_type", "agent");
    item.author_id = optional_str(d, "author_id");
    item.pinned = truthy(get(d, "pinned"));
    item.revision = has(d, "revision") ? detail::py_int_or_throw(get(d, "revision")) : 1;
    item.evidence = strings_of(get(d, "evidence"));
    item.created_at = stamp_or_now(d, "created_at", clock);
    item.updated_at = stamp_or_now(d, "updated_at", clock);
    return item;
}

// Question

Json Question::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["text"] = text;
    out["status"] = status;
    out["suggested_answer"] = optional_json(suggested_answer);
    out["suggested_confidence"] = suggested_confidence;
    out["answer"] = optional_json(answer);
    out["answer_source"] = optional_json(answer_source);
    out["confidence"] = confidence;
    out["thread"] = thread;
    out["evidence"] = strings_json(evidence);
    out["asked_at"] = asked_at;
    out["resolved_at"] = optional_json(resolved_at);
    out["resolved_by"] = optional_json(resolved_by);
    return out;
}

Question Question::from_json(const Json& d, const Clock& clock) {
    Question q;
    q.id = required_str(d, "id");
    q.text = string_or(d, "text", "");
    q.status = string_or(d, "status", "open");
    q.suggested_answer = optional_str(d, "suggested_answer");
    q.suggested_confidence = get(d, "suggested_confidence");
    q.answer = optional_str(d, "answer");
    q.answer_source = optional_str(d, "answer_source");
    q.confidence = get(d, "confidence");
    const Json& thread = get(d, "thread");
    q.thread = truthy(thread) ? detail::py_list(thread) : Json::array();
    q.evidence = strings_of(get(d, "evidence"));
    q.asked_at = stamp_or_now(d, "asked_at", clock);
    q.resolved_at = optional_str(d, "resolved_at");
    q.resolved_by = optional_str(d, "resolved_by");
    return q;
}

// CustomReport

Json CustomReport::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["request"] = request;
    out["status"] = status;
    out["title"] = title;
    out["markdown"] = markdown;
    out["message"] = message;
    out["run_id"] = run_id;
    out["sources"] = sources;
    out["requested_by"] = optional_json(requested_by);
    out["created_at"] = created_at;
    out["updated_at"] = updated_at;
    return out;
}

CustomReport CustomReport::from_json(const Json& d, const Clock& clock) {
    CustomReport report;
    report.id = required_str(d, "id");
    report.request = string_or(d, "request", "");
    const Json& status = has(d, "status") ? get(d, "status") : Json("running");
    report.status = status.is_string() && one_of(CUSTOM_REPORT_STATUSES, status.get<std::string>())
                        ? status.get<std::string>()
                        : "failed";
    report.title = string_or(d, "title", "");
    report.markdown = string_or(d, "markdown", "");
    report.message = string_or(d, "message", "");
    report.run_id = string_or(d, "run_id", "");
    report.sources = dict_or_empty(get(d, "sources"));
    report.requested_by = optional_str(d, "requested_by");
    report.created_at = stamp_or_now(d, "created_at", clock);
    report.updated_at = stamp_or_now(d, "updated_at", clock);
    return report;
}

// Participant

Json Participant::to_json() const {
    Json out = Json::object();
    out["id"] = id;
    out["display_name"] = display_name;
    out["kind"] = kind;
    out["name_source"] = name_source;
    out["is_provisional"] = is_provisional;
    out["created_at"] = created_at;
    out["updated_at"] = updated_at;
    return out;
}

Participant Participant::from_json(const Json& d, const Clock& clock) {
    Participant p;
    p.id = required_str(d, "id");
    p.display_name = string_or(d, "display_name", "");
    p.kind = string_or(d, "kind", "others_cluster");
    p.name_source = string_or(d, "name_source", "default");
    p.is_provisional = truthy(get(d, "is_provisional"));
    p.created_at = stamp_or_now(d, "created_at", clock);
    p.updated_at = stamp_or_now(d, "updated_at", clock);
    return p;
}

// TopicState and MeetingIntent

Json TopicState::to_json() const {
    Json out = Json::object();
    out["current"] = current;
    out["history"] = history;
    return out;
}

TopicState TopicState::from_json(const Json& d) {
    require_mapping(d);
    TopicState topic;
    topic.current = string_or(d, "current", "");
    const Json& history = get(d, "history");
    topic.history = truthy(history) ? detail::py_list(history) : Json::array();
    return topic;
}

Json MeetingIntent::to_json() const {
    Json out = Json::object();
    out["text"] = text;
    out["updated_at"] = updated_at;
    out["author_id"] = author_id;
    return out;
}

MeetingIntent MeetingIntent::from_json(const Json& d) {
    if (!d.is_object()) {
        return {};
    }
    return {str_or_empty(d, "text"), str_or_empty(d, "updated_at"), str_or_empty(d, "author_id")};
}

// FinalizationState

bool coerce_card_deferred(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return truthy(value);
    }
    if (value.is_string()) {
        const std::string folded = py::lower(py::strip(value.get_ref<const std::string&>()));
        return folded == "1" || folded == "true" || folded == "yes";
    }
    return false;
}

Json FinalizationState::to_json() const {
    Json out = Json::object();
    out["status"] = status;
    out["message"] = message;
    out["stage"] = stage;
    out["current_step"] = current_step;
    out["total_steps"] = total_steps;
    out["step_details"] = step_details;
    out["steps"] = steps;
    out["summary_stats"] = summary_stats;
    out["card_deferred"] = card_deferred;
    return out;
}

std::optional<std::pair<std::string, std::string>>
FinalizationState::history_pill(std::string_view meeting_status) const {
    if (meeting_status == "active" || meeting_status == "paused" || meeting_status == "ending") {
        return std::nullopt;
    }
    bool failed_steps = false;
    for (const auto& step : steps) {
        const Json& step_status = get(step, "status");
        if (truthy(step_status) && detail::py_str(step_status) == "failed") {
            failed_steps = true;
            break;
        }
    }
    const bool incomplete = failed_steps || status == "failed" || status == "unavailable";
    if (card_deferred && incomplete) {
        return std::pair<std::string, std::string>{"Saved for later", "warning"};
    }
    if (status == "failed" || (status == "completed" && failed_steps)) {
        return std::pair<std::string, std::string>{"Incomplete", "warning"};
    }
    if (status == "unavailable") {
        return std::pair<std::string, std::string>{"Unavailable", "warning"};
    }
    if (status == "completed") {
        return std::pair<std::string, std::string>{"Ready", "success"};
    }
    if (status == "disabled") {
        return std::pair<std::string, std::string>{"Off", "neutral"};
    }
    return std::nullopt;
}

FinalizationState FinalizationState::default_for_cloud(bool cloud_enabled) {
    FinalizationState fin;
    if (!cloud_enabled) {
        fin.status = "disabled";
        fin.message = kInsightsOffMessage;
    }
    return fin;
}

FinalizationState FinalizationState::infer_legacy(bool cloud_enabled,
                                                  std::string_view meeting_status) {
    FinalizationState fin;
    if (!cloud_enabled) {
        fin.status = "disabled";
        fin.message = kInsightsOffMessage;
    } else if (one_of(kTerminalMeetingStatuses, meeting_status)) {
        fin.status = "unavailable";
        fin.message = kInsightsNotRecordedMessage;
    }
    return fin;
}

FinalizationState FinalizationState::coerce(const FinalizationState& value, bool cloud_enabled,
                                            std::string_view meeting_status) {
    if (one_of(FINALIZATION_STATUSES, value.status)) {
        return value;
    }
    return infer_legacy(cloud_enabled, meeting_status);
}

FinalizationState FinalizationState::coerce(const Json& value, bool cloud_enabled,
                                            std::string_view meeting_status) {
    if (!value.is_object()) {
        return infer_legacy(cloud_enabled, meeting_status);
    }
    const Json& status = get(value, "status");
    if (!status.is_string() || !one_of(FINALIZATION_STATUSES, status.get<std::string>())) {
        return infer_legacy(cloud_enabled, meeting_status);
    }
    FinalizationState fin;
    fin.status = status.get<std::string>();
    const Json& message = get(value, "message");
    fin.message = message.is_null() ? std::string() : detail::py_str(message);
    fin.stage = str_or_empty(value, "stage");
    const Json& current = get(value, "current_step");
    fin.current_step = truthy(current) ? detail::py_int_or_throw(current) : 0;
    const Json& total = get(value, "total_steps");
    fin.total_steps = truthy(total) ? detail::py_int_or_throw(total) : 0;
    fin.step_details = str_or_empty(value, "step_details");
    const Json& steps = get(value, "steps");
    fin.steps = Json::array();
    if (steps.is_array()) {
        for (const auto& step : steps) {
            // dict(step) of anything but a mapping fails in Python too.
            if (!step.is_object()) {
                throw py::TypeError("cannot convert dictionary update sequence element #0 to a "
                                    "sequence");
            }
            fin.steps.push_back(step);
        }
    }
    const Json& stats = get(value, "summary_stats");
    fin.summary_stats = stats.is_object() ? stats : Json::object();
    fin.card_deferred = coerce_card_deferred(get(value, "card_deferred"));
    return fin;
}

FinalizationState FinalizationState::normalize_historical(const Json& value, bool cloud_enabled,
                                                          std::string_view meeting_status) {
    FinalizationState fin = coerce(value, cloud_enabled, meeting_status);
    if (!one_of(kTerminalMeetingStatuses, meeting_status)) {
        return fin;
    }
    if (fin.status != "pending" && fin.status != "running") {
        return fin;
    }
    if (!cloud_enabled) {
        fin.status = "disabled";
        fin.message = kInsightsOffMessage;
        return fin;
    }
    if (fin.status == "running") {
        Json interrupted = Json::array();
        for (const auto& step : fin.steps) {
            Json row = step;
            if (one_of_json(get(row, "status"), {"running", "pending"})) {
                row["status"] = "failed";
                if (!truthy(get(row, "detail"))) {
                    row["detail"] = kStepInterruptedDetail;
                }
            }
            interrupted.push_back(std::move(row));
        }
        fin.status = "failed";
        fin.message = kInsightsInterruptedMessage;
        fin.steps = std::move(interrupted);
        return fin;
    }
    fin.status = "unavailable";
    fin.message = kInsightsNotRecordedMessage;
    return fin;
}

// Cards

std::vector<CardItem>& Cards::operator[](std::string_view card) {
    const auto index = card_index(card);
    if (!index) {
        throw std::out_of_range("unknown card " + std::string(card));
    }
    return cards_[*index];
}

const std::vector<CardItem>& Cards::operator[](std::string_view card) const {
    return const_cast<Cards&>(*this)[card];
}

// MeetingState

Json MeetingState::default_capture() {
    Json capture = Json::object();
    capture["mic_available"] = false;
    capture["loopback_available"] = false;
    capture["message"] = "";
    capture["audio_incomplete"] = false;
    capture["integrity_error"] = "";
    return capture;
}

CustomReport* MeetingState::find_custom_report(std::string_view report_id) {
    for (auto& report : custom_reports) {
        if (report.id == report_id) {
            return &report;
        }
    }
    return nullptr;
}

CardItem* MeetingState::find_item(std::string_view item_id) {
    for (std::size_t c = 0; c < CARD_KEYS.size(); ++c) {
        for (auto& item : cards[c]) {
            if (item.id == item_id) {
                return &item;
            }
        }
    }
    return nullptr;
}

const CardItem* MeetingState::find_item(std::string_view item_id) const {
    return const_cast<MeetingState&>(*this).find_item(item_id);
}

std::size_t MeetingState::open_question_count() const {
    std::size_t count = 0;
    for (const auto& [id, question] : questions) {
        if (question.status == "open") {
            ++count;
        }
    }
    return count;
}

Json MeetingState::to_json() const {
    Json out = Json::object();
    out["meeting_id"] = meeting_id;
    out["seq"] = seq;
    out["status"] = status;
    out["cloud_enabled"] = cloud_enabled;
    out["intelligence_online"] = intelligence_online;
    out["diarization_available"] = diarization_available;
    out["diarization_reason"] = diarization_reason;
    out["speech"] = speech;
    out["title"] = title;
    out["intent"] = intent.to_json();
    out["topic"] = topic.to_json();
    out["rolling_summary"] = rolling_summary;
    out["rolling_summary_evidence"] = strings_json(rolling_summary_evidence);
    out["capture"] = capture;
    Json people = Json::object();
    for (const auto& [pid, participant] : participants) {
        people[pid] = participant.to_json();
    }
    out["participants"] = std::move(people);
    Json card_json = Json::object();
    for (std::size_t c = 0; c < CARD_KEYS.size(); ++c) {
        Json items = Json::array();
        for (const auto& item : cards[c]) {
            items.push_back(item.to_json());
        }
        card_json[std::string(CARD_KEYS[c])] = std::move(items);
    }
    out["cards"] = std::move(card_json);
    Json question_json = Json::array();
    for (const auto& [id, question] : questions) {
        question_json.push_back(question.to_json());
    }
    out["questions"] = std::move(question_json);
    out["finalization"] = finalization.to_json();
    out["report_views"] = strings_json(report_views);
    out["insight_review"] = insight_review;
    out["live_highlights"] = live_highlights;
    out["live_highlights_status"] = live_highlights_status;
    out["voice_feedback"] = voice_feedback;
    Json reports = Json::array();
    for (const auto& report : custom_reports) {
        reports.push_back(report.to_json());
    }
    out["custom_reports"] = std::move(reports);
    return out;
}

MeetingState MeetingState::from_json(const Json& d, const Clock& clock) {
    require_mapping(d);
    const bool cloud_enabled = truthy(get(d, "cloud_enabled"));
    const std::string meeting_status = string_or(d, "status", "active");
    // Legacy snapshots omit finalization; infer it rather than invent a success.
    FinalizationState finalization =
        has(d, "finalization")
            ? FinalizationState::coerce(get(d, "finalization"), cloud_enabled, meeting_status)
            : FinalizationState::infer_legacy(cloud_enabled, meeting_status);

    MeetingState state;
    state.meeting_id = required_str(d, "meeting_id");
    state.seq = has(d, "seq") ? detail::py_int_or_throw(get(d, "seq")) : 0;
    state.status = meeting_status;
    state.cloud_enabled = cloud_enabled;
    state.intelligence_online = truthy(get(d, "intelligence_online"));
    state.diarization_available = truthy(get(d, "diarization_available"));
    state.diarization_reason = str_or_empty(d, "diarization_reason");
    state.speech = dict_or_empty(get(d, "speech"));
    state.title = string_or(d, "title", "");
    const Json& intent = get(d, "intent");
    state.intent = MeetingIntent::from_json(truthy(intent) ? intent : Json::object());
    const Json& topic = get(d, "topic");
    state.topic = TopicState::from_json(truthy(topic) ? topic : Json::object());
    state.rolling_summary = string_or(d, "rolling_summary", "");
    state.rolling_summary_evidence = strings_of(get(d, "rolling_summary_evidence"));

    const Json& raw_capture = get(d, "capture");
    const Json capture = truthy(raw_capture) ? require_mapping(raw_capture) : Json::object();
    state.capture["mic_available"] = truthy(get(capture, "mic_available"));
    state.capture["loopback_available"] = truthy(get(capture, "loopback_available"));
    state.capture["message"] =
        has(capture, "message") ? detail::py_str(get(capture, "message")) : std::string();
    state.capture["audio_incomplete"] = truthy(get(capture, "audio_incomplete"));
    state.capture["integrity_error"] = has(capture, "integrity_error")
                                           ? detail::py_str(get(capture, "integrity_error"))
                                           : std::string();

    state.finalization = std::move(finalization);
    state.insight_review = dict_or_empty(get(d, "insight_review"));
    const Json& highlights = get(d, "live_highlights");
    state.live_highlights = truthy(highlights) ? highlights : Json::array();
    state.live_highlights_status = string_or(d, "live_highlights_status", "unknown");
    state.voice_feedback = dict_or_empty(get(d, "voice_feedback"));
    const Json& views = get(d, "report_views");
    if (truthy(views)) {
        state.report_views = strings_of(views);
    }

    const Json& people = get(d, "participants");
    if (truthy(people)) {
        for (const auto& [pid, pd] : require_mapping(people).items()) {
            state.participants.set(pid, Participant::from_json(pd, clock));
        }
    }
    const Json& raw_cards = get(d, "cards");
    const Json cards = truthy(raw_cards) ? require_mapping(raw_cards) : Json::object();
    for (std::size_t c = 0; c < CARD_KEYS.size(); ++c) {
        const std::string key(CARD_KEYS[c]);
        if (!cards.contains(key)) {
            continue;
        }
        for (const auto& item : iterable(cards.at(key))) {
            state.cards[c].push_back(CardItem::from_json(item, clock));
        }
    }
    const Json& questions = get(d, "questions");
    if (truthy(questions)) {
        for (const auto& qd : iterable(questions)) {
            Question q = Question::from_json(qd, clock);
            std::string id = q.id;
            state.questions.set(std::move(id), std::move(q));
        }
    }
    const Json& reports = get(d, "custom_reports");
    if (truthy(reports)) {
        for (const auto& rd : iterable(reports)) {
            if (rd.is_object() && truthy(get(rd, "id"))) {
                state.custom_reports.push_back(CustomReport::from_json(rd, clock));
            }
        }
    }
    return state;
}

} // namespace fv::meeting_state
