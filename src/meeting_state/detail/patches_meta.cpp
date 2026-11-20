#include <algorithm>
#include <exception>

#include "base/log.h"
#include "history/titles.h"
#include "meeting_state/detail/patches_internal.h"
#include "meeting_state/detail/py_value.h"
#include "pyport/text.h"

// Meeting-level ops (topic, rolling summary, title, brief, cloud flag) and the segment ops the
// store's segment handler applies (patches.py).
namespace fv::meeting_state::detail {

namespace {

constexpr log::Logger logger("meeting.state.patches");

bool host_or_system(const OpContext& ctx) {
    return ctx.actor_type == "host" || ctx.actor_type == "system";
}

// op.get(key, fallback).
Json value_or(const Json& op, std::string_view key, Json fallback) {
    return has(op, key) ? get(op, key) : fallback;
}

// A non-empty text segment id, or nullopt (invalid_segment).
std::optional<std::string> segment_id_of(const Json& op) {
    const Json& id = get(op, "segment_id");
    if (!id.is_string() || id.get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    return id.get<std::string>();
}

} // namespace

OpResult op_set_topic(MeetingState& state, Json& op, const OpContext& ctx) {
    auto reason = check_text(get(op, "text"), MAX_TOPIC_LEN);
    if (!reason) {
        reason = check_evidence(op, ctx);
    }
    if (reason) {
        return reject(op, std::move(*reason));
    }
    const std::string prev = state.topic.current;
    Json prev_evidence = Json::array();
    if (state.topic.history.is_array() && !state.topic.history.empty()) {
        prev_evidence = at(state.topic.history.back(), "evidence");
    }
    const std::string text = py::strip(op.at("text").get_ref<const std::string&>());
    const Json& evidence = get(op, "evidence");
    Json entry = Json::object();
    entry["text"] = text;
    entry["ts"] = ctx.now_iso();
    entry["evidence"] = truthy(evidence) ? evidence : Json::array();
    entry["actor_type"] = ctx.actor_type;
    state.topic.history.push_back(std::move(entry));
    state.topic.current = text;

    Json effect = Json::object();
    effect["entity"] = "topic";
    effect["topic"] = state.topic.to_json();
    Json inverse;
    // The first topic is not undoable.
    if (!prev.empty()) {
        inverse = Json::object();
        inverse["op"] = "set_topic";
        inverse["text"] = prev;
        inverse["evidence"] = std::move(prev_evidence);
    }
    return accepted(op, std::nullopt, std::move(effect), std::move(inverse));
}

OpResult op_set_rolling_summary(MeetingState& state, Json& op, const OpContext& ctx) {
    const Json text = value_or(op, "text", "");
    // Too long is invalid_text here, unlike every other text field.
    if (!text.is_string() || py::len(text.get_ref<const std::string&>()) > MAX_SUMMARY_LEN) {
        return reject(op, "invalid_text");
    }
    // A blank summary silently wipes visible content; only undo (the system restoring an
    // empty value) may clear it.
    if (py::strip(text.get_ref<const std::string&>()).empty() && ctx.actor_type != "system") {
        return reject(op, "invalid_text");
    }
    if (auto reason = check_evidence(op, ctx)) {
        return reject(op, std::move(*reason));
    }
    const std::string prev = state.rolling_summary;
    const auto prev_evidence = state.rolling_summary_evidence;
    state.rolling_summary = text.get<std::string>();
    state.rolling_summary_evidence = evidence_ids(op);

    Json effect = Json::object();
    effect["entity"] = "rolling_summary";
    effect["text"] = state.rolling_summary;
    effect["evidence"] = evidence_json(state.rolling_summary_evidence);
    Json inverse = Json::object();
    inverse["op"] = "set_rolling_summary";
    inverse["text"] = prev;
    inverse["evidence"] = evidence_json(prev_evidence);
    return accepted(op, std::nullopt, std::move(effect), std::move(inverse));
}

OpResult op_set_title(MeetingState& state, Json& op, const OpContext& ctx) {
    // As the host-only REST rename route: guests must not retitle.
    if (!host_or_system(ctx)) {
        return reject(op, "host_only");
    }
    if (auto reason = title_error(get(op, "text"))) {
        return reject(op, std::move(*reason));
    }
    const std::string prev = state.title;
    state.title = normalize_title(op.at("text"));
    Json effect = Json::object();
    effect["entity"] = "title";
    effect["text"] = state.title;
    Json inverse;
    if (!prev.empty()) {
        inverse = Json::object();
        inverse["op"] = "set_title";
        inverse["text"] = prev;
    }
    return accepted(op, std::nullopt, std::move(effect), std::move(inverse));
}

OpResult op_set_meeting_intent(MeetingState& state, Json& op, const OpContext& ctx) {
    // The brief steers every agent pass, so a guest must not redirect what the record
    // captures. Blank text is a valid clear of the host's own direction.
    if (!host_or_system(ctx)) {
        return reject(op, "host_only");
    }
    const Json text = value_or(op, "text", "");
    if (!text.is_string() || py::len(text.get_ref<const std::string&>()) > MAX_INTENT_LEN) {
        return reject(op, "invalid_text");
    }
    const std::string previous = state.intent.text;
    state.intent = MeetingIntent{py::strip(text.get_ref<const std::string&>()), ctx.now_iso(),
                                 ctx.actor_id.value_or("")};
    Json effect = Json::object();
    effect["entity"] = "intent";
    effect["intent"] = state.intent.to_json();
    Json inverse = Json::object();
    inverse["op"] = "set_meeting_intent";
    inverse["text"] = previous;
    return accepted(op, std::nullopt, std::move(effect), std::move(inverse));
}

OpResult op_set_cloud_enabled(MeetingState& state, Json& op, const OpContext& ctx) {
    if (!host_or_system(ctx)) {
        return reject(op, "host_only");
    }
    const bool enabled = truthy(get(op, "enabled"));
    const bool prev = state.cloud_enabled;
    state.cloud_enabled = enabled;
    Json effect = Json::object();
    effect["entity"] = "cloud_enabled";
    effect["enabled"] = enabled;
    Json inverse = Json::object();
    inverse["op"] = "set_cloud_enabled";
    inverse["enabled"] = prev;
    return accepted(op, std::nullopt, std::move(effect), std::move(inverse));
}

OpResult op_reassign_segment_speaker(MeetingState& state, Json& op, const OpContext& ctx) {
    if (ctx.is_agent()) {
        return reject(op, "agent_forbidden");
    }
    const auto segment_id = segment_id_of(op);
    if (!segment_id) {
        return reject(op, "invalid_segment");
    }
    const Json participant_id = get(op, "participant_id");
    if (!participant_id.is_null() &&
        !(participant_id.is_string() &&
          state.participants.contains(participant_id.get_ref<const std::string&>()))) {
        return reject(op, "unknown_participant");
    }
    // A row not committed yet, or replaced by a re-decode, would fail persistence and take the
    // rest of the batch with it.
    if (ctx.segment_exists && !ctx.segment_exists(*segment_id)) {
        return reject(op, "unknown_segment", *segment_id);
    }
    // Human pins are authoritative: a diarizer batch computed before a pin landed must not
    // revert it. A human may still overwrite an earlier pin.
    if (!ctx.is_human() && !truthy(get(op, "force")) && ctx.segment_pinned) {
        bool already_pinned = true;
        try {
            already_pinned = ctx.segment_pinned(*segment_id);
        } catch (const std::exception& error) {
            // Fail closed rather than clobber a pin.
            logger.error_with(error, "segment_pinned predicate failed for " + *segment_id);
        }
        if (already_pinned) {
            return reject(op, "segment_pinned", *segment_id);
        }
    }
    const bool system = ctx.actor_type == "system";
    Json effect = Json::object();
    effect["entity"] = "segment_speaker";
    effect["segment_id"] = *segment_id;
    effect["participant_id"] = participant_id;
    effect["source"] =
        system ? value_or(op, "_source", "diarizer") : Json(ctx.is_human() ? "human" : "diarizer");
    effect["pinned"] = system ? truthy(get(op, "_pinned")) : ctx.is_human();
    // The store fills the inverse in: it knows the prior row.
    return accepted(op, *segment_id, std::move(effect));
}

OpResult op_revise_segment_text(MeetingState& state, Json& op, const OpContext& ctx) {
    const auto segment_id = segment_id_of(op);
    if (!segment_id) {
        return reject(op, "invalid_segment");
    }
    if (ctx.segment_exists && !ctx.segment_exists(*segment_id)) {
        return reject(op, "unknown_segment", *segment_id);
    }
    if (ctx.is_agent()) {
        // Agents see the correction-projected transcript; revising it would bake a human's
        // replacement into the raw row and defeat undo.
        for (const auto& item : state.cards[HUMAN_ONLY_CARD]) {
            const Json& kind = get(item.data, "kind");
            if (item.status != "removed" && kind.is_string() &&
                kind.get_ref<const std::string&>() == "occurrence_correction" &&
                std::find(item.evidence.begin(), item.evidence.end(), *segment_id) !=
                    item.evidence.end()) {
                return reject(op, "human_scoped_correction", *segment_id);
            }
        }
    }
    if (auto reason = check_text(get(op, "text"), MAX_TEXT_LEN)) {
        return reject(op, std::move(*reason), *segment_id);
    }
    // Agents cite the segment they edit.
    if (ctx.is_agent()) {
        const auto cited = evidence_ids(op);
        if (std::find(cited.begin(), cited.end(), *segment_id) == cited.end()) {
            return reject(op, "missing_evidence", *segment_id);
        }
    }
    Json effect = Json::object();
    effect["entity"] = "segment_text";
    effect["segment_id"] = *segment_id;
    effect["text"] = py::strip(op.at("text").get_ref<const std::string&>());
    return accepted(op, *segment_id, std::move(effect));
}

} // namespace fv::meeting_state::detail
