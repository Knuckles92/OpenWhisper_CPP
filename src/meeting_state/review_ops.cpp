#include "meeting_state/review_ops.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "meeting_state/detail/extras.h"
#include "meeting_state/detail/patches_internal.h"
#include "meeting_state/detail/py_value.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

using detail::at;
using detail::get;
using detail::has;
using detail::py_str;
using detail::truthy;

// Suffixes that append a review answer to a note block it clarifies.
constexpr std::string_view kClarificationPrefix =
    "\n\nUser clarification (supersedes earlier wording on this point): ";

constexpr std::string_view kAnsweredQuestionsKept = "answered";
constexpr std::size_t kRetainedAnswers = 50;
constexpr std::size_t kHistoryKept = 10;

constexpr std::string_view kOfferedPrefix = "Offered, not agreed: ";
constexpr std::string_view kResolvedPrefix = "Resolved: ";
constexpr std::string_view kEmDash = "\xE2\x80\x94";

// _REVIEW_LABELS; a missing answer is Python's KeyError.
std::string review_label(const std::string& action) {
    static const std::pair<std::string_view, std::string_view> labels[] = {
        {"accurate", "Confirmed as accurate"},
        {"accepted", "Agreed"},
        {"offered", "Only offered, not agreed"},
        {"incorrect", "Incorrect; removed"},
        {"resolved", "Risk resolved"},
        {"open", "Risk remains open"},
        {"edit", "Corrected"},
    };
    for (const auto& [answer, label] : labels) {
        if (answer == action) {
            return std::string(label);
        }
    }
    detail::key_error(action);
}

Json& review_of(MeetingState& state) {
    return detail::mapping(state.insight_review);
}

// Questions of the review, each an object Python indexes.
Json& questions_of(Json& review) {
    if (!review.contains("questions")) {
        // review.get("questions", []) reads a missing list as empty; nothing writes to it.
        static thread_local Json empty;
        empty = Json::array();
        return empty;
    }
    Json& questions = review["questions"];
    if (!questions.is_array()) {
        throw py::TypeError("object is not iterable");
    }
    return questions;
}

bool is(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

bool settled_meeting(const MeetingState& state) {
    return state.status == "ended" && state.finalization.status != "pending" &&
           state.finalization.status != "running";
}

bool consented(const MeetingState& state, const Json& review) {
    return truthy(get(review, "enabled")) && state.cloud_enabled &&
           is(get(review, "consent"), REVIEW_CONSENT);
}

OpResult result(const Json& op, const MeetingState& state, const std::vector<CardItem*>& items,
                std::optional<std::string> target) {
    Json effect = Json::object();
    effect["entity"] = "review";
    effect["review"] = state.insight_review;
    Json listed = Json::array();
    for (const CardItem* item : items) {
        listed.push_back(item->to_json());
    }
    effect["items"] = std::move(listed);
    OpResult out;
    out.ok = true;
    out.op = op;
    out.target_id = std::move(target);
    out.effect = std::move(effect);
    return out;
}

// _system: the gate of the worker's ops.
std::optional<std::string> system_rejection(MeetingState& state, const OpContext& ctx) {
    if (ctx.actor_type != "system") {
        return "system_only";
    }
    if (!consented(state, review_of(state))) {
        return "review_disabled";
    }
    return std::nullopt;
}

CardItem* find_item(MeetingState& state, const Json& id) {
    return id.is_string() ? state.find_item(id.get_ref<const std::string&>()) : nullptr;
}

std::string strip_prefix(const std::string& text, std::string_view prefix) {
    return text.starts_with(prefix) ? text.substr(prefix.size()) : text;
}

// _apply_assessments: each assessment lands on its still-current item.
std::pair<std::vector<CardItem*>, std::set<std::string>> apply_assessments(MeetingState& state,
                                                                           const Json& op) {
    std::vector<CardItem*> changed;
    std::set<std::string> eligible;
    for (const Json& assessment : detail::list_member(op, "assessments")) {
        CardItem* item = find_item(state, at(assessment, "item_id"));
        if (item == nullptr || item->is_protected() || item->status == "removed" ||
            !py::py_equal(Json(item->revision), at(assessment, "revision"))) {
            continue;
        }
        item->review = at(assessment, "review");
        eligible.insert(item->id);
        changed.push_back(item);
    }
    return {std::move(changed), std::move(eligible)};
}

bool same_key(const std::vector<Json>& left, const std::vector<Json>& right) {
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!py::py_equal(left[i], right[i])) {
            return false;
        }
    }
    return true;
}

std::vector<Json> question_key(const Json& q) {
    return {at(q, "item_id"), at(q, "revision"), at(q, "field")};
}

// The id as a member of a set of item ids; Python cannot hash a list or a dict.
std::optional<std::string> hashable_id(const Json& id) {
    if (id.is_array() || id.is_object()) {
        throw py::TypeError("unhashable type");
    }
    if (!id.is_string()) {
        return std::nullopt;
    }
    return id.get<std::string>();
}

// _new_review_questions: one question per eligible item, skipping repeats of the same insight
// text; a field skipped before stays skipped across retries of the same insight revision.
Json new_review_questions(MeetingState& state, const Json& op, Json& review,
                          const std::set<std::string>& eligible) {
    std::vector<std::vector<Json>> skipped;
    for (const Json& q : questions_of(review)) {
        if (is(get(detail::mapping(q), "status"), "skipped")) {
            skipped.push_back(question_key(q));
        }
    }
    Json questions = Json::array();
    std::set<std::string> seen;
    std::set<std::string> seen_text;
    for (const Json& submitted : detail::list_member(op, "questions")) {
        const auto item_id = hashable_id(at(submitted, "item_id"));
        if (!item_id || !eligible.contains(*item_id) || seen.contains(*item_id)) {
            continue;
        }
        const std::string text_key =
            detail::join(py::split(py::lower(state.find_item(*item_id)->text)), " ");
        if (seen_text.contains(text_key)) {
            continue;
        }
        seen_text.insert(text_key);
        Json q = submitted;
        const auto key = question_key(q);
        if (std::any_of(skipped.begin(), skipped.end(),
                        [&](const auto& other) { return same_key(key, other); })) {
            q["status"] = "skipped";
        }
        seen.insert(*item_id);
        questions.push_back(std::move(q));
    }
    return questions;
}

struct FoundQuestion {
    Json* question = nullptr;
    CardItem* item = nullptr;
    std::optional<std::string> reason;
};

// _question: the host's question, and whether it still describes the current insight.
FoundQuestion find_question(MeetingState& state, const Json& op, const OpContext& ctx) {
    if (ctx.actor_type != "host") {
        return {nullptr, nullptr, "host_only"};
    }
    if (!settled_meeting(state)) {
        return {nullptr, nullptr, "meeting_not_ready"};
    }
    Json* found = nullptr;
    for (Json& q : questions_of(review_of(state))) {
        if (py::py_equal(at(q, "id"), get(op, "question_id"))) {
            found = &q;
            break;
        }
    }
    if (found == nullptr) {
        return {nullptr, nullptr, "unknown_review_question"};
    }
    CardItem* item = find_item(state, at(*found, "item_id"));
    if (truthy(get(*found, "superseded")) || item == nullptr ||
        !py::py_equal(Json(item->revision), at(*found, "revision")) ||
        !py::py_equal(detail::evidence_json(item->evidence), at(*found, "evidence")) ||
        (item->status == "removed" && !truthy(get(*found, "correction")))) {
        return {found, item, "review_changed"};
    }
    return {found, item, std::nullopt};
}

// op.get("answer") in q["choices"].
bool offered_choice(const Json& answer, const Json& choices) {
    if (choices.is_object()) {
        if (answer.is_array() || answer.is_object()) {
            throw py::TypeError("unhashable type");
        }
        return answer.is_string() && choices.contains(answer.get_ref<const std::string&>());
    }
    if (choices.is_array()) {
        return std::any_of(choices.begin(), choices.end(),
                           [&](const Json& choice) { return py::py_equal(choice, answer); });
    }
    if (choices.is_string()) {
        if (!answer.is_string()) {
            throw py::TypeError("'in <string>' requires string as left operand");
        }
        return choices.get_ref<const std::string&>().find(answer.get_ref<const std::string&>()) !=
               std::string::npos;
    }
    throw py::TypeError("argument of type is not iterable");
}

// op.get("text", item.text).
const Json& answer_text(const Json& op, const CardItem& item, Json& fallback) {
    if (has(op, "text")) {
        return get(op, "text");
    }
    fallback = item.text;
    return fallback;
}

std::optional<std::string> answer_rejection(const Json& q, const CardItem& item, const Json& op) {
    if (!is(at(q, "status"), "open")) {
        return "already_closed";
    }
    if (!offered_choice(get(op, "answer"), at(q, "choices"))) {
        return "invalid_review_answer";
    }
    Json fallback;
    const Json& text = answer_text(op, item, fallback);
    if (!text.is_string() || py::strip(text.get_ref<const std::string&>()).empty() ||
        py::len(text.get_ref<const std::string&>()) > MAX_REVIEW_TEXT) {
        return "invalid_review_text";
    }
    return std::nullopt;
}

// _edit_owner: the corrected owner, set on `data`.
std::optional<std::string> edit_owner(const MeetingState& state, const Json& op,
                                      const CardItem& item, const std::string& text, Json& data) {
    const Json& owner = get(op, "owner_participant_id");
    if (!owner.is_null() &&
        (!owner.is_string() || !state.participants.contains(owner.get_ref<const std::string&>()))) {
        return "invalid_owner";
    }
    const Json owner_name = has(op, "owner_name") ? get(op, "owner_name") : Json("");
    if (!owner_name.is_string() ||
        py::len(owner_name.get_ref<const std::string&>()) > MAX_REVIEW_FIELD_CHARS) {
        return "invalid_owner";
    }
    if (!py::py_equal(owner, get(data, "owner_participant_id")) && text == item.text) {
        return "review_wording_required";
    }
    data["owner_participant_id"] = owner;
    data["owner_name"] =
        owner.is_null() ? py::strip(owner_name.get_ref<const std::string&>()) : std::string();
    return std::nullopt;
}

// _edit_deadline: the corrected deadline, set on `data`; an empty one clears it.
std::optional<std::string> edit_deadline(const Json& op, const CardItem& item,
                                         const std::string& text, Json& data) {
    const Json deadline = has(op, "deadline") ? get(op, "deadline") : Json("");
    if (!deadline.is_string() ||
        py::len(deadline.get_ref<const std::string&>()) > MAX_REVIEW_FIELD_CHARS) {
        return "invalid_deadline";
    }
    const Json& stored =
        truthy(get(data, "deadline")) ? get(data, "deadline") : get(data, "due_date");
    const std::string current = truthy(stored) ? py_str(stored) : std::string();
    const std::string wanted = py::strip(deadline.get_ref<const std::string&>());
    if (wanted != current && text == item.text) {
        return "review_wording_required";
    }
    data["deadline"] = wanted.empty() ? Json() : Json(wanted);
    data.erase("due_date");
    return std::nullopt;
}

// _answered_text: a commitment or resolution answer recorded on `data`, the text labelled.
std::string answered_text(const std::string& action, std::string text, const CardItem& item,
                          Json& data) {
    const Json& previous = get(item.review, "answer");
    if (action == "offered") {
        data["commitment"] = "offered";
        if (!text.starts_with(kOfferedPrefix)) {
            text = std::string(kOfferedPrefix) + text;
        }
    } else if (action == "accepted") {
        data["commitment"] = "accepted";
        if (is(previous, "offered")) {
            text = strip_prefix(text, kOfferedPrefix);
        }
    } else if (action == "resolved" || action == "open") {
        data["resolution"] = action;
        if (action == "resolved" && !text.starts_with(kResolvedPrefix)) {
            text = std::string(kResolvedPrefix) + text;
        } else if (action == "open" && is(previous, "resolved")) {
            text = strip_prefix(text, kResolvedPrefix);
        }
    }
    return text;
}

std::string review_note_text(const MeetingState& state, const std::string& action,
                             const Json& field, const std::string& text, const Json& data) {
    std::string note = review_label(action) + " " + std::string(kEmDash) + " " + text;
    if (action == "edit" && is(field, "owner")) {
        const Json& owner = get(data, "owner_participant_id");
        const Participant* person =
            owner.is_string() ? state.participants.find(owner.get_ref<const std::string&>())
                              : nullptr;
        std::string label;
        if (person != nullptr) {
            label = person->display_name;
        } else if (truthy(get(data, "owner_name"))) {
            label = py_str(get(data, "owner_name"));
        } else {
            label = "Unassigned";
        }
        note += " Owner: " + label + ".";
    } else if (action == "edit" && is(field, "deadline")) {
        const Json& deadline = get(data, "deadline");
        note += " Deadline: " + (truthy(deadline) ? py_str(deadline) : "None agreed") + ".";
    }
    return note;
}

// _linked_note: the user note an earlier answer to the question created, if it still exists.
CardItem* linked_note(MeetingState& state, const Json& q) {
    const Json& note_id = get(q, "note_id");
    if (!truthy(note_id)) {
        return nullptr;
    }
    return find_item(state, note_id);
}

// _clarify_note_blocks: the clarification is appended to (or refreshed on) every note block
// sharing the item's evidence. Clarifications are explicit additions: no prose is guessed at,
// and a block that would outgrow MAX_NOTE_BLOCK_CHARS keeps its text.
std::vector<CardItem*> clarify_note_blocks(MeetingState& state, const CardItem& item, const Json& q,
                                           const std::string& note_text, const std::string& now) {
    std::vector<CardItem*> changed;
    const Json& correction = get(q, "correction");
    const std::string suffix = std::string(kClarificationPrefix) + note_text;
    const std::string previous =
        std::string(kClarificationPrefix) + (has(q, "correction") ? py_str(correction) : "");
    for (CardItem& block : state.cards[NOTES_CARD]) {
        if (block.status == "removed") {
            continue;
        }
        const bool shared =
            std::any_of(block.evidence.begin(), block.evidence.end(), [&](const std::string& id) {
                return std::find(item.evidence.begin(), item.evidence.end(), id) !=
                       item.evidence.end();
            });
        if (!shared) {
            continue;
        }
        std::string updated;
        if (truthy(correction) && block.text.find(previous) != std::string::npos) {
            updated = detail::replace_all(block.text, previous, suffix);
        } else {
            updated = block.text + suffix;
        }
        if (py::len(updated) <= MAX_NOTE_BLOCK_CHARS) {
            block.text = std::move(updated);
            block.status = "edited";
            block.revision += 1;
            block.updated_at = now;
            changed.push_back(&block);
        }
    }
    return changed;
}

} // namespace

OpResult review_begin(MeetingState& state, Json& op, const OpContext& ctx) {
    if (auto reason = system_rejection(state, ctx)) {
        return reject(op, std::move(*reason));
    }
    if (!settled_meeting(state)) {
        return reject(op, "meeting_not_ready");
    }
    Json& review = review_of(state);
    if (is(get(review, "status"), "running")) {
        return reject(op, "review_running");
    }
    Json run_id = at(op, "run_id");
    review["status"] = "running";
    review["run_id"] = std::move(run_id);
    review["message"] = "Checking the final insights\xE2\x80\xA6";
    return result(op, state, {}, std::nullopt);
}

OpResult review_finish(MeetingState& state, Json& op, const OpContext& ctx) {
    if (ctx.actor_type != "system") {
        return reject(op, "system_only");
    }
    Json& review = review_of(state);
    if (!py::py_equal(get(review, "run_id"), get(op, "run_id")) ||
        !is(get(review, "status"), "running")) {
        return reject(op, "stale_review");
    }
    if (!consented(state, review)) {
        review["status"] = "unavailable";
        review["message"] = "AI insights are off. Review was stopped.";
        return result(op, state, {}, std::nullopt);
    }
    auto [changed, eligible] = apply_assessments(state, op);
    Json retained = Json::array();
    for (const Json& q : questions_of(review)) {
        if (is(get(detail::mapping(q), "status"), kAnsweredQuestionsKept)) {
            retained.push_back(q);
        }
    }
    Json questions = new_review_questions(state, op, review, eligible);
    const Json& status = get(op, "status");
    if (is(status, "ready") || is(status, "partial")) {
        Json kept = Json::array();
        const std::size_t first =
            retained.size() > kRetainedAnswers ? retained.size() - kRetainedAnswers : 0;
        for (std::size_t i = first; i < retained.size(); ++i) {
            kept.push_back(std::move(retained[i]));
        }
        for (Json& q : questions) {
            kept.push_back(std::move(q));
        }
        review["questions"] = std::move(kept);
    }
    Json final_status = at(op, "status");
    Json message = has(op, "message") ? get(op, "message") : Json("");
    review["status"] = std::move(final_status);
    review["message"] = std::move(message);
    review["checked_at"] = ctx.now_iso();
    return result(op, state, changed, std::nullopt);
}

OpResult review_skip(MeetingState& state, Json& op, const OpContext& ctx) {
    FoundQuestion found = find_question(state, op, ctx);
    if (found.reason) {
        return reject(op, std::move(*found.reason));
    }
    if (!is(at(*found.question, "status"), "open")) {
        return reject(op, "already_closed");
    }
    (*found.question)["status"] = "skipped";
    return result(op, state, {}, found.item->id);
}

OpResult review_reopen(MeetingState& state, Json& op, const OpContext& ctx) {
    FoundQuestion found = find_question(state, op, ctx);
    if (found.reason) {
        return reject(op, std::move(*found.reason));
    }
    (*found.question)["status"] = "open";
    return result(op, state, {}, found.item->id);
}

OpResult review_answer(MeetingState& state, Json& op, const OpContext& ctx) {
    FoundQuestion found = find_question(state, op, ctx);
    if (found.reason) {
        return reject(op, std::move(*found.reason));
    }
    Json& q = *found.question;
    if (auto reason = answer_rejection(q, *found.item, op)) {
        return reject(op, std::move(*reason));
    }
    // A choice offered by the question is text when choices is a mapping, as the worker writes
    // them; another choice reads as its str().
    const std::string action = py_str(get(op, "answer"));
    Json fallback;
    const std::string text =
        py::strip(answer_text(op, *found.item, fallback).get_ref<const std::string&>());
    const Json field = at(q, "field");
    Json data = detail::mapping(found.item->data);
    std::optional<std::string> reason;
    if (action == "edit" && is(field, "owner")) {
        reason = edit_owner(state, op, *found.item, text, data);
    } else if (action == "edit" && is(field, "deadline")) {
        reason = edit_deadline(op, *found.item, text, data);
    }
    if (reason) {
        return reject(op, std::move(*reason));
    }
    if (check_data_size(data)) {
        return reject(op, "data_too_large");
    }
    const std::string answered = answered_text(action, text, *found.item, data);
    const std::string note_text = review_note_text(state, action, field, answered, data);
    CardItem* note = linked_note(state, q);
    if (note == nullptr && state.cards[HUMAN_ONLY_CARD].size() >= MAX_REVIEW_USER_NOTES) {
        return reject(op, "item_limit");
    }

    const std::string now = ctx.now_iso();
    const std::string item_id = found.item->id;
    CardItem* item = found.item;
    const Json before = item->to_json();
    item->text = answered;
    item->data = std::move(data);
    item->status = action == "incorrect" ? "removed" : "edited";
    item->revision += 1;
    item->updated_at = now;
    item->review = Json::object();
    item->review["state"] = "human";
    item->review["field"] = field;
    item->review["answer"] = action;
    item->review["reviewed_by"] = ctx.actor_id ? Json(*ctx.actor_id) : Json();
    item->review["reviewed_at"] = now;

    // _write_review_note: the answer's user note, created or rewritten.
    std::string note_id;
    if (note == nullptr) {
        CardItem created;
        created.id = ctx.new_id("it");
        created.card = std::string(HUMAN_ONLY_CARD);
        created.text = note_text;
        created.status = "edited";
        created.author_type = "user";
        created.author_id = ctx.actor_id;
        created.evidence = item->evidence;
        created.data = Json::object();
        created.data["review_item_id"] = item_id;
        created.created_at = now;
        created.updated_at = now;
        note_id = created.id;
        state.cards[HUMAN_ONLY_CARD].push_back(std::move(created));
        // The append may move the user notes; the reviewed item is found again by its id.
        item = state.find_item(item_id);
        note = &state.cards[HUMAN_ONLY_CARD].back();
    } else {
        note->text = note_text;
        note->status = "edited";
        note->revision += 1;
        note->updated_at = now;
        note_id = note->id;
    }
    std::vector<CardItem*> changed{item, note};
    for (CardItem* block : clarify_note_blocks(state, *item, q, note_text, now)) {
        changed.push_back(block);
    }

    q["status"] = "answered";
    q["answer"] = review_label(action);
    q["answer_source"] = "user";
    q["correction"] = note_text;
    q["answered_at"] = now;
    q["answered_by"] = ctx.actor_id ? Json(*ctx.actor_id) : Json();
    q["note_id"] = note_id;
    q["revision"] = item->revision;
    q["insight"] = item->to_json();
    // The audit entry keeps the previous insight without touching transcript evidence.
    if (!q.contains("history")) {
        q["history"] = Json::array();
    }
    Json& history = q["history"];
    if (!history.is_array()) {
        throw py::PyError("AttributeError", "object has no attribute 'append'");
    }
    history.push_back({{"before", before},
                       {"answer", action},
                       {"at", now},
                       {"actor_id", ctx.actor_id ? Json(*ctx.actor_id) : Json()},
                       {"actor_type", ctx.actor_type}});
    if (history.size() > kHistoryKept) {
        Json kept = Json::array();
        for (std::size_t i = history.size() - kHistoryKept; i < history.size(); ++i) {
            kept.push_back(std::move(history[i]));
        }
        history = std::move(kept);
    }
    return result(op, state, changed, item->id);
}

OpResult review_unavailable(MeetingState& state, Json& op, const OpContext& ctx) {
    if (ctx.actor_type != "system") {
        return reject(op, "system_only");
    }
    Json& review = review_of(state);
    review["status"] = "unavailable";
    review["message"] = "AI insights and insight review must both be on for this meeting.";
    return result(op, state, {}, std::nullopt);
}

void invalidate_checks(MeetingState& state) {
    for (std::size_t card = 0; card < CARD_KEYS.size(); ++card) {
        for (CardItem& item : state.cards[card]) {
            item.citation_check = Json::object();
        }
    }
    Json& review = review_of(state);
    if (!truthy(get(review, "enabled"))) {
        return;
    }
    review["status"] = "pending";
    review["message"] = "Waiting for the updated final insights.";
    for (Json& q : questions_of(review)) {
        if (!is(get(detail::mapping(q), "status"), "answered")) {
            q["superseded"] = true;
        }
    }
    for (std::size_t card = 0; card < CARD_KEYS.size(); ++card) {
        for (CardItem& item : state.cards[card]) {
            if (!is(get(item.review, "state"), "human")) {
                item.review = Json::object();
            }
        }
    }
}

} // namespace fv::meeting_state
