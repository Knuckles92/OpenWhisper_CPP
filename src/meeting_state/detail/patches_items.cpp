#include <algorithm>
#include <set>

#include "meeting_state/corrections.h"
#include "meeting_state/detail/patches_internal.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/time_utils.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

// Card item ops (patches.py) and the review-aware item effect (review.py item_effect).
namespace fv::meeting_state {

namespace {

using detail::get;
using detail::has;
using detail::truthy;

// Duplicate thresholds (jaccard, containment): distinct cards legitimately restate one another,
// so only a near-verbatim match across cards counts; same-card duplicates are paraphrases.
constexpr double kCrossCardJaccard = 0.75;
constexpr double kCrossCardContainment = 0.90;
constexpr double kSameCardJaccard = 0.60;
constexpr double kSameCardContainment = 0.75;

// Highest occurrence index a one-off transcript correction may target.
constexpr std::int64_t kMaxOccurrenceIndex = 1000;

constexpr std::string_view kClarificationPrefix =
    "\n\nUser clarification (supersedes earlier wording on this point): ";
constexpr std::string_view kSupersededClarificationPrefix =
    "\n\nEarlier clarification, superseded by a later insight edit: ";
constexpr std::string_view kSupersededNotePrefix =
    "Earlier review, superseded by a later insight edit: ";

std::set<std::string> tokens(const std::string& normalized) {
    const auto words = py::split(normalized);
    return {words.begin(), words.end()};
}

bool valid_term(const Json& value) {
    if (!value.is_string()) {
        return false;
    }
    const std::size_t length = py::len(py::strip(value.get_ref<const std::string&>()));
    return length > 0 && length <= MAX_TERM_CHARS;
}

bool valid_occurrence_index(const Json& index) {
    // A JSON bool is its own type, so isinstance(index, bool) needs no test here.
    if (!index.is_number_integer()) {
        return false;
    }
    if (index.is_number_unsigned()) {
        return index.get<std::uint64_t>() <= static_cast<std::uint64_t>(kMaxOccurrenceIndex);
    }
    const std::int64_t value = index.get<std::int64_t>();
    return value >= 0 && value <= kMaxOccurrenceIndex;
}

// A one-off correction must be attached to exactly one real passage, so an arbitrary page
// selection cannot silently become a global text rule.
std::optional<std::string> check_occurrence_correction(const Json& op, const Json& data,
                                                       const OpContext& ctx) {
    const Json& raw = get(op, "evidence");
    if (!ctx.is_human()) {
        return "human_only_card";
    }
    if (!raw.is_array() || raw.size() != 1 || !raw[0].is_string() ||
        raw[0].get_ref<const std::string&>().empty()) {
        return "invalid_evidence";
    }
    if (!ctx.segment_exists || !ctx.segment_exists(raw[0].get_ref<const std::string&>())) {
        return "unknown_evidence";
    }
    if (!(valid_term(get(data, "selected_text")) && valid_term(get(data, "replacement")))) {
        return "invalid_data";
    }
    if (!valid_occurrence_index(get(data, "occurrence_index"))) {
        return "invalid_data";
    }
    if (!valid_segment_fingerprint(get(data, "base_fingerprint"))) {
        return "invalid_data";
    }
    return std::nullopt;
}

// Timeline beats need a numeric meeting-clock anchor, stored as a float.
std::optional<std::string> check_timeline_data(Json& data) {
    if (!data.contains("start_s")) {
        return "missing_start_s";
    }
    const auto start_s = py::py_float(data.at("start_s"));
    if (!start_s || *start_s < 0.0) {
        return "invalid_start_s";
    }
    data["start_s"] = *start_s;
    return std::nullopt;
}

std::optional<std::string> check_card_data(std::string_view card, Json& data, const Json& op,
                                           const OpContext& ctx) {
    if (card == "user_notes") {
        const Json& kind = get(data, "kind");
        if (kind.is_string() && kind.get_ref<const std::string&>() == "occurrence_correction") {
            if (auto reason = check_occurrence_correction(op, data, ctx)) {
                return reason;
            }
        }
    }
    if (card == "timeline" && (ctx.is_agent() || ctx.actor_type == "system")) {
        return check_timeline_data(data);
    }
    return std::nullopt;
}

bool duplicates_live_item(const MeetingState& state, std::string_view card,
                          const std::string& text) {
    for (std::size_t c = 0; c < CARD_KEYS.size(); ++c) {
        const std::string_view other = CARD_KEYS[c];
        if (other == HUMAN_ONLY_CARD) {
            continue;
        }
        // The note taker's minutes stand on their own even when a shorter card records the
        // same fact.
        if (other != card && (other == NOTES_CARD || card == NOTES_CARD)) {
            continue;
        }
        const double jaccard = other == card ? kSameCardJaccard : kCrossCardJaccard;
        const double containment = other == card ? kSameCardContainment : kCrossCardContainment;
        for (const auto& existing : state.cards[c]) {
            if (existing.status == "removed") {
                continue;
            }
            if (detail::item_text_too_similar(existing.text, text, jaccard, containment)) {
                return true;
            }
        }
    }
    return false;
}

Json plain_item_effect(const CardItem& item) {
    Json effect = Json::object();
    effect["entity"] = "item";
    effect["item"] = item.to_json();
    return effect;
}

Json forced_status_inverse(const CardItem& item, const Json& set, const std::string& status) {
    Json inverse = Json::object();
    inverse["op"] = "update_item";
    inverse["id"] = item.id;
    inverse["set"] = set;
    inverse["restore_status"] = status;
    inverse["force"] = true;
    return inverse;
}

// state.find_item(op.get("id", "")).
CardItem* find_target(MeetingState& state, const Json& op) {
    const Json& id = has(op, "id") ? get(op, "id") : Json("");
    if (!id.is_string()) {
        return nullptr;
    }
    return state.find_item(id.get_ref<const std::string&>());
}

void replace_all(std::string& text, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while (!from.empty() && (at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
}

std::optional<OpResult> update_item_rejection(const CardItem& item, const Json& op,
                                              const OpContext& ctx, const Json& changes) {
    if (ctx.is_agent()) {
        if (item.is_protected()) {
            return reject(op, "human_edited", item.id);
        }
        if (!py::py_equal(get(op, "base_revision"), Json(item.revision))) {
            return reject(op, "revision_mismatch", item.id, item.revision);
        }
    }
    if (!changes.is_object() || !(truthy(changes) || truthy(get(op, "restore_status")))) {
        return reject(op, "empty_update", item.id);
    }
    std::optional<std::string> reason;
    if (changes.contains("text")) {
        reason = check_text(changes.at("text"));
    }
    if (!reason && changes.contains("data")) {
        reason = changes.at("data").is_object() ? check_data_size(changes.at("data"))
                                                : std::optional<std::string>("invalid_data");
    }
    // New evidence is always checked; an agent must cite an item that has none.
    if (!reason && (truthy(get(op, "evidence")) || (ctx.is_agent() && item.evidence.empty()))) {
        reason = check_evidence(op, ctx);
    }
    if (reason) {
        return reject(op, std::move(*reason), item.id);
    }
    return std::nullopt;
}

void apply_item_changes(CardItem& item, const Json& changes, bool force) {
    if (changes.contains("text")) {
        item.text = py::strip(changes.at("text").get_ref<const std::string&>());
    }
    if (changes.contains("data")) {
        Json data = changes.at("data");
        if (item.card == NOTES_CARD && !force) {
            // A note block is defined by its heading and stamp, which the note-taker prompt
            // promises persist; the forced undo path restores the exact prior data instead.
            for (const char* key : {"heading", "start_s"}) {
                if (!data.contains(key) && item.data.contains(key)) {
                    data[key] = item.data.at(key);
                }
            }
        }
        item.data = std::move(data);
    }
}

void update_item_evidence(CardItem& item, const Json& op, const Json& changes, bool force) {
    const Json& restored = get(changes, "evidence");
    if (force && restored.is_array()) {
        // Undo restores the exact prior anchor set.
        item.evidence.clear();
        for (const auto& id : restored) {
            if (id.is_string()) {
                item.evidence.push_back(id.get<std::string>());
            }
        }
    } else if (truthy(get(op, "evidence"))) {
        // Anchors are additive: a consolidation pass rewords and merges items, and replacing
        // the list would drop anchors that undo could not bring back.
        for (auto& id : detail::evidence_ids(op)) {
            if (std::find(item.evidence.begin(), item.evidence.end(), id) == item.evidence.end()) {
                item.evidence.push_back(std::move(id));
            }
        }
    }
}

} // namespace

Json item_effect(MeetingState& state, CardItem& item, const Clock& clock) {
    std::vector<Json*> related;
    if (state.insight_review.contains("questions")) {
        Json& questions = state.insight_review["questions"];
        if (!questions.is_array()) {
            throw py::TypeError("insight_review questions are not a list");
        }
        for (auto& q : questions) {
            const Json& item_id = detail::at(q, "item_id");
            if (item_id.is_string() && item_id.get_ref<const std::string&>() == item.id &&
                !truthy(get(q, "superseded"))) {
                related.push_back(&q);
            }
        }
    }
    if (related.empty()) {
        return plain_item_effect(item);
    }
    std::vector<CardItem*> changed{&item};
    for (Json* q : related) {
        (*q)["superseded"] = true;
        const Json& note_id = has(*q, "note_id") ? get(*q, "note_id") : Json("");
        CardItem* note =
            note_id.is_string() ? state.find_item(note_id.get_ref<const std::string&>()) : nullptr;
        if (note == nullptr || note->status == "removed") {
            continue;
        }
        const std::string old_suffix = std::string(kClarificationPrefix) + note->text;
        const std::string superseded_suffix =
            std::string(kSupersededClarificationPrefix) + note->text;
        for (auto& block : state.cards[NOTES_CARD]) {
            if (block.text.find(old_suffix) != std::string::npos) {
                replace_all(block.text, old_suffix, superseded_suffix);
                block.revision += 1;
                block.updated_at = utc_now_iso(clock);
                changed.push_back(&block);
            }
        }
        note->text = std::string(kSupersededNotePrefix) + note->text;
        note->revision += 1;
        note->updated_at = utc_now_iso(clock);
        changed.push_back(note);
    }
    Json effect = Json::object();
    effect["entity"] = "review";
    effect["review"] = state.insight_review;
    Json items = Json::array();
    for (const CardItem* entry : changed) {
        items.push_back(entry->to_json());
    }
    effect["items"] = std::move(items);
    return effect;
}

namespace detail {

std::string normalize_item_text(std::string_view text) {
    std::string out;
    bool separator = false;
    for (const char32_t cp : py::to_code_points(py::lower(text))) {
        if ((cp >= U'a' && cp <= U'z') || (cp >= U'0' && cp <= U'9')) {
            if (separator && !out.empty()) {
                out += ' ';
            }
            separator = false;
            out += static_cast<char>(cp);
        } else {
            separator = true;
        }
    }
    return out;
}

bool item_text_too_similar(std::string_view left, std::string_view right, double jaccard_threshold,
                           double containment_threshold) {
    const std::string a = normalize_item_text(left);
    const std::string b = normalize_item_text(right);
    if (a.empty() || b.empty()) {
        return false;
    }
    if (a == b) {
        return true;
    }
    const auto ta = tokens(a);
    const auto tb = tokens(b);
    if (ta.empty() || tb.empty()) {
        return false;
    }
    std::size_t intersection = 0;
    for (const auto& token : ta) {
        intersection += tb.count(token);
    }
    const std::size_t united = ta.size() + tb.size() - intersection;
    const double jaccard = static_cast<double>(intersection) / static_cast<double>(united);
    if (jaccard >= jaccard_threshold) {
        return true;
    }
    // Containment means something only once both sides have enough tokens; otherwise "Budget
    // approved" would collide with every budget sentence.
    const std::size_t smaller = std::min(ta.size(), tb.size());
    if (smaller >= 4) {
        const double containment = static_cast<double>(intersection) / static_cast<double>(smaller);
        if (containment >= containment_threshold) {
            return true;
        }
    }
    return false;
}

OpResult op_add_item(MeetingState& state, Json& op, const OpContext& ctx) {
    const Json& card_value = get(op, "card");
    const auto index = card_value.is_string() ? card_index(card_value.get_ref<const std::string&>())
                                              : std::nullopt;
    if (!index) {
        return reject(op, "unknown_card");
    }
    const std::string card(CARD_KEYS[*index]);
    if (ctx.is_agent() && card == HUMAN_ONLY_CARD) {
        return reject(op, "human_only_card");
    }
    auto reason = check_text(get(op, "text"));
    if (!reason) {
        reason = check_evidence(op, ctx);
    }
    if (reason) {
        return reject(op, std::move(*reason));
    }
    // The op's own data object is normalized in place, as Python's dict is shared with the op.
    Json local = Json::object();
    Json* data = &local;
    if (op.contains("data") && truthy(op.at("data"))) {
        data = &op.at("data");
    }
    if (!data->is_object()) {
        return reject(op, "invalid_data");
    }
    reason = check_data_size(*data);
    if (!reason) {
        reason = check_card_data(card, *data, op, ctx);
    }
    if (reason) {
        return reject(op, std::move(*reason));
    }
    if (state.cards[*index].size() >= MAX_CARD_ITEMS) {
        return reject(op, "card_full");
    }
    const std::string& text = op.at("text").get_ref<const std::string&>();
    if (ctx.is_agent() && duplicates_live_item(state, card, text)) {
        return reject(op, "duplicate_item");
    }

    CardItem item;
    item.id = ctx.new_id("it");
    item.card = card;
    item.text = py::strip(text);
    item.data = *data;
    item.status = ctx.is_human() ? "edited" : "proposed";
    item.author_type = ctx.is_human() ? "user" : ctx.actor_type;
    item.author_id = ctx.actor_id;
    item.evidence = evidence_ids(op);
    item.created_at = ctx.now_iso();
    item.updated_at = ctx.now_iso();
    state.cards[*index].push_back(item);

    Json inverse = Json::object();
    inverse["op"] = "remove_item";
    inverse["id"] = item.id;
    return accepted(op, item.id, plain_item_effect(item), std::move(inverse));
}

OpResult op_update_item(MeetingState& state, Json& op, const OpContext& ctx) {
    CardItem* item = find_target(state, op);
    if (item == nullptr) {
        return reject(op, "unknown_item");
    }
    const bool force = truthy(get(op, "force")) && ctx.actor_type == "system";
    const Json& raw_changes = get(op, "set");
    const Json changes = truthy(raw_changes) ? raw_changes : Json::object();
    if (auto rejection = update_item_rejection(*item, op, ctx, changes)) {
        return std::move(*rejection);
    }

    Json prev = Json::object();
    prev["text"] = item->text;
    prev["data"] = item->data;
    prev["evidence"] = evidence_json(item->evidence);
    const std::string prev_status = item->status;
    apply_item_changes(*item, changes, force);
    // A repaired sample keeps its provenance; a real agent rewrite is marked so the capture rail
    // can show the synthesized replacement (metadata-only edits are not).
    if (ctx.is_agent() && item->author_type == "system" && item->author_id == "state_repair") {
        const Json& synthesized = get(prev.at("data"), "insight_synthesized");
        if (item->text != prev.at("text").get_ref<const std::string&>() ||
            (synthesized.is_boolean() && synthesized.get<bool>())) {
            item->data["insight_synthesized"] = true;
        }
    }
    update_item_evidence(*item, op, changes, force);
    const Json& restore = get(op, "restore_status");
    if (truthy(restore) && force) {
        item->status = restore.is_string() ? restore.get<std::string>() : py_str(restore);
    } else if (ctx.is_human()) {
        item->status = "edited";
    }
    item->review = Json::object();
    item->citation_check = Json::object();
    item->revision += 1;
    item->updated_at = ctx.now_iso();
    Json effect = item_effect(state, *item, ctx.time());
    return accepted(op, item->id, std::move(effect),
                    forced_status_inverse(*item, prev, prev_status));
}

OpResult op_remove_item(MeetingState& state, Json& op, const OpContext& ctx) {
    CardItem* item = find_target(state, op);
    if (item == nullptr) {
        return reject(op, "unknown_item");
    }
    if (item->status == "removed") {
        return reject(op, "already_removed", item->id);
    }
    if (ctx.is_agent()) {
        if (item->is_protected()) {
            return reject(op, "human_edited", item->id);
        }
        if (!py::py_equal(get(op, "base_revision"), Json(item->revision))) {
            return reject(op, "revision_mismatch", item->id, item->revision);
        }
    }
    const std::string prev_status = item->status;
    item->status = "removed";
    item->revision += 1;
    item->updated_at = ctx.now_iso();
    Json effect = item_effect(state, *item, ctx.time());
    return accepted(op, item->id, std::move(effect),
                    forced_status_inverse(*item, Json::object(), prev_status));
}

OpResult op_pin_item(MeetingState& state, Json& op, const OpContext& ctx, bool pinned) {
    CardItem* item = find_target(state, op);
    if (item == nullptr) {
        return reject(op, "unknown_item");
    }
    item->pinned = pinned;
    item->updated_at = ctx.now_iso();
    Json inverse = Json::object();
    inverse["op"] = pinned ? "unpin_item" : "pin_item";
    inverse["id"] = item->id;
    return accepted(op, item->id, plain_item_effect(*item), std::move(inverse));
}

OpResult op_confirm_item(MeetingState& state, Json& op, const OpContext& ctx) {
    CardItem* item = find_target(state, op);
    if (item == nullptr) {
        return reject(op, "unknown_item");
    }
    const std::string prev_status = item->status;
    item->status = "confirmed";
    item->revision += 1;
    item->updated_at = ctx.now_iso();
    return accepted(op, item->id, plain_item_effect(*item),
                    forced_status_inverse(*item, Json::object(), prev_status));
}

} // namespace detail

} // namespace fv::meeting_state
