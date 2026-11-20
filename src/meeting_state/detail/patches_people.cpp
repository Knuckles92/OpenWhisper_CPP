#include <algorithm>

#include "meeting_state/detail/patches_internal.h"
#include "meeting_state/detail/py_value.h"
#include "pyport/number.h"
#include "pyport/text.h"

// Participant and quiet-inbox question ops (patches.py).
namespace fv::meeting_state::detail {

namespace {

Json participant_effect(const Participant& participant) {
    Json effect = Json::object();
    effect["entity"] = "participant";
    effect["participant"] = participant.to_json();
    return effect;
}

Json question_effect(const Question& question) {
    Json effect = Json::object();
    effect["entity"] = "question";
    effect["question"] = question.to_json();
    return effect;
}

Json rename_inverse(const std::string& participant_id, const std::string& name,
                    const std::string& source) {
    Json inverse = Json::object();
    inverse["op"] = "rename_participant";
    inverse["participant_id"] = participant_id;
    inverse["display_name"] = name;
    inverse["_name_source"] = source;
    return inverse;
}

Json reopen_inverse(const std::string& question_id) {
    Json inverse = Json::object();
    inverse["op"] = "reopen_question";
    inverse["question_id"] = question_id;
    return inverse;
}

// d.get(key, "") as a lookup key: anything but text finds nothing.
std::optional<std::string> key_of(const Json& op, std::string_view key) {
    if (!has(op, key)) {
        return std::string();
    }
    const Json& value = get(op, key);
    if (!value.is_string()) {
        return std::nullopt;
    }
    return value.get<std::string>();
}

Participant* find_participant(MeetingState& state, const Json& op, std::string_view key) {
    const auto id = key_of(op, key);
    return id ? state.participants.find(*id) : nullptr;
}

Question* find_question(MeetingState& state, const Json& op) {
    const auto id = key_of(op, "question_id");
    return id ? state.questions.find(*id) : nullptr;
}

std::string stripped(const Json& op, std::string_view key) {
    return py::strip(op.at(key).get_ref<const std::string&>());
}

} // namespace

OpResult op_upsert_participant(MeetingState& state, Json& op, const OpContext& ctx) {
    auto reason = check_text(get(op, "display_name"), MAX_NAME_LEN);
    if (!reason) {
        reason = check_evidence(op, ctx);
    }
    if (reason) {
        return reject(op, std::move(*reason));
    }
    // What the agent generates stays visibly provisional, and it may only mint remote-speaker
    // clusters, never a second "me" or a fake guest.
    const Json kind = has(op, "kind") ? get(op, "kind") : Json(AGENT_PARTICIPANT_KIND);
    bool is_provisional = true;
    if (ctx.is_agent()) {
        if (!kind.is_string() || kind.get_ref<const std::string&>() != AGENT_PARTICIPANT_KIND) {
            return reject(op, "invalid_kind");
        }
    } else {
        is_provisional = truthy(get(op, "is_provisional"));
    }
    const std::string kind_text = kind.is_string() ? kind.get<std::string>() : std::string();

    const Json& pid = get(op, "id");
    if (truthy(pid)) {
        Participant* participant =
            pid.is_string() ? state.participants.find(pid.get_ref<const std::string&>()) : nullptr;
        if (participant == nullptr) {
            return reject(op, "unknown_participant");
        }
        const std::string id = pid.get<std::string>();
        if (ctx.is_agent() && participant->name_source == "human") {
            return reject(op, "human_named", id);
        }
        const std::string prev_name = participant->display_name;
        const std::string prev_source = participant->name_source;
        participant->display_name = stripped(op, "display_name");
        participant->name_source = ctx.is_agent() ? "agent_inferred" : "human";
        participant->is_provisional = is_provisional;
        participant->updated_at = ctx.now_iso();
        return accepted(op, participant->id, participant_effect(*participant),
                        rename_inverse(id, prev_name, prev_source));
    }
    // "me" and "guest" identities are established by the app (as the system actor); a client
    // minting one could impersonate the host, who is found as the first "me".
    if ((kind_text == "me" || kind_text == "guest") && ctx.actor_type != "system") {
        return reject(op, "invalid_kind");
    }
    if (state.participants.size() >= MAX_PARTICIPANTS) {
        return reject(op, "participant_limit");
    }
    Participant participant;
    participant.id = ctx.new_id("p");
    participant.display_name = stripped(op, "display_name");
    participant.kind = kind_text;
    participant.name_source = ctx.is_agent() ? "agent_inferred" : "human";
    participant.is_provisional = is_provisional;
    participant.created_at = ctx.now_iso();
    participant.updated_at = ctx.now_iso();
    if (!kind.is_string() ||
        (kind_text != "me" && kind_text != "others_cluster" && kind_text != "guest")) {
        return reject(op, "invalid_kind");
    }
    std::string id = participant.id;
    const Participant& added = state.participants.set(std::move(id), participant);
    // Participants are never removed, so a new one has no inverse; renames do.
    return accepted(op, added.id, participant_effect(added));
}

OpResult op_suggest_participant_name(MeetingState& state, Json& op, const OpContext& ctx) {
    Participant* participant = find_participant(state, op, "participant_id");
    if (participant == nullptr) {
        return reject(op, "unknown_participant");
    }
    if (participant->name_source == "human") {
        return reject(op, "human_named", participant->id);
    }
    auto reason = check_text(get(op, "display_name"), MAX_NAME_LEN);
    if (!reason) {
        reason = check_evidence(op, ctx);
    }
    if (reason) {
        return reject(op, std::move(*reason), participant->id);
    }
    const std::string prev_name = participant->display_name;
    const std::string prev_source = participant->name_source;
    participant->display_name = stripped(op, "display_name");
    participant->name_source = "agent_inferred";
    participant->is_provisional = true;
    participant->updated_at = ctx.now_iso();
    return accepted(op, participant->id, participant_effect(*participant),
                    rename_inverse(participant->id, prev_name, prev_source));
}

OpResult op_rename_participant(MeetingState& state, Json& op, const OpContext& ctx) {
    if (ctx.is_agent()) {
        return reject(op, "agent_forbidden");
    }
    Participant* participant = find_participant(state, op, "participant_id");
    if (participant == nullptr) {
        return reject(op, "unknown_participant");
    }
    if (auto reason = check_text(get(op, "display_name"), MAX_NAME_LEN)) {
        return reject(op, std::move(*reason), participant->id);
    }
    const std::string prev_name = participant->display_name;
    const std::string prev_source = participant->name_source;
    participant->display_name = stripped(op, "display_name");
    // Undo restores the original name_source through the private field.
    if (ctx.actor_type == "system") {
        const Json& source = has(op, "_name_source") ? get(op, "_name_source") : Json("human");
        participant->name_source = source.is_string() ? source.get<std::string>() : py_str(source);
    } else {
        participant->name_source = "human";
    }
    participant->is_provisional = false;
    participant->updated_at = ctx.now_iso();
    return accepted(op, participant->id, participant_effect(*participant),
                    rename_inverse(participant->id, prev_name, prev_source));
}

OpResult op_ask_question(MeetingState& state, Json& op, const OpContext& ctx) {
    auto reason = check_text(get(op, "text"));
    if (!reason) {
        reason = check_evidence(op, ctx);
    }
    if (reason) {
        return reject(op, std::move(*reason));
    }
    // The agent keeps a quiet inbox; humans get a far higher ceiling that only bounds an
    // abusive client.
    if (ctx.actor_type != "system") {
        const std::size_t cap = ctx.is_agent() ? MAX_OPEN_QUESTIONS : MAX_OPEN_QUESTIONS_HUMAN;
        if (state.open_question_count() >= cap) {
            return reject(op, "question_limit");
        }
    }
    Question question;
    question.id = ctx.new_id("q");
    question.text = stripped(op, "text");
    question.evidence = evidence_ids(op);
    question.asked_at = ctx.now_iso();
    std::string id = question.id;
    const Question& added = state.questions.set(std::move(id), question);
    Json inverse = Json::object();
    inverse["op"] = "dismiss_question";
    inverse["question_id"] = added.id;
    return accepted(op, added.id, question_effect(added), std::move(inverse));
}

OpResult op_resolve_question(MeetingState& state, Json& op, const OpContext& ctx) {
    Question* question = find_question(state, op);
    if (question == nullptr) {
        return reject(op, "unknown_question");
    }
    if (question->status != "open") {
        return reject(op, "already_closed", question->id);
    }
    auto reason = check_text(get(op, "answer_text"));
    if (!reason) {
        reason = check_evidence(op, ctx);
    }
    if (reason) {
        return reject(op, std::move(*reason), question->id);
    }
    const auto confidence =
        has(op, "confidence") ? py::py_float(get(op, "confidence")) : std::optional<double>(0.0);
    if (!confidence) {
        return reject(op, "invalid_confidence", question->id);
    }
    Json inverse;
    if (*confidence >= RESOLVE_CONFIDENCE) {
        question->status = "resolved";
        question->answer = stripped(op, "answer_text");
        question->answer_source = "audio";
        question->confidence = *confidence;
        question->resolved_at = ctx.now_iso();
        question->resolved_by = ctx.actor_id;
        for (auto& id : evidence_ids(op)) {
            if (std::find(question->evidence.begin(), question->evidence.end(), id) ==
                question->evidence.end()) {
                question->evidence.push_back(std::move(id));
            }
        }
        inverse = reopen_inverse(question->id);
    } else if (*confidence >= SUGGEST_CONFIDENCE) {
        question->suggested_answer = stripped(op, "answer_text");
        question->suggested_confidence = *confidence;
    } else {
        return reject(op, "low_confidence", question->id);
    }
    return accepted(op, question->id, question_effect(*question), std::move(inverse));
}

OpResult op_answer_question(MeetingState& state, Json& op, const OpContext& ctx) {
    if (ctx.is_agent()) {
        return reject(op, "agent_forbidden");
    }
    Question* question = find_question(state, op);
    if (question == nullptr) {
        return reject(op, "unknown_question");
    }
    if (auto reason = check_text(get(op, "answer_text"))) {
        return reject(op, std::move(*reason), question->id);
    }
    Json entry = Json::object();
    entry["author_type"] = ctx.actor_type;
    entry["author_id"] = ctx.actor_id ? Json(*ctx.actor_id) : Json();
    entry["text"] = stripped(op, "answer_text");
    entry["ts"] = ctx.now_iso();
    question->thread.push_back(std::move(entry));
    question->status = "resolved";
    question->answer = stripped(op, "answer_text");
    question->answer_source = "user";
    question->suggested_answer.reset();
    question->suggested_confidence = Json();
    question->resolved_at = ctx.now_iso();
    question->resolved_by = ctx.actor_id;
    return accepted(op, question->id, question_effect(*question), reopen_inverse(question->id));
}

OpResult op_dismiss_question(MeetingState& state, Json& op, const OpContext& ctx) {
    Question* question = find_question(state, op);
    if (question == nullptr) {
        return reject(op, "unknown_question");
    }
    if (question->status == "dismissed") {
        return reject(op, "already_closed", question->id);
    }
    const std::string prev_status = question->status;
    question->status = "dismissed";
    question->resolved_at = ctx.now_iso();
    question->resolved_by = ctx.actor_id;
    return accepted(op, question->id, question_effect(*question),
                    prev_status == "open" ? reopen_inverse(question->id) : Json());
}

OpResult op_reopen_question(MeetingState& state, Json& op, const OpContext& ctx) {
    if (ctx.is_agent()) {
        return reject(op, "agent_forbidden");
    }
    Question* question = find_question(state, op);
    if (question == nullptr) {
        return reject(op, "unknown_question");
    }
    if (question->status == "open") {
        return reject(op, "already_open", question->id);
    }
    question->status = "open";
    question->resolved_at.reset();
    question->resolved_by.reset();
    return accepted(op, question->id, question_effect(*question));
}

} // namespace fv::meeting_state::detail
