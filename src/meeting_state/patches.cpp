#include "meeting_state/patches.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

#include "base/log.h"
#include "meeting_state/detail/meeting_ops.h"
#include "meeting_state/detail/patches_internal.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.state.patches");

using detail::get;
using detail::truthy;

struct Entry {
    Handler handler;
    OpDomains domains;
    OpFlags flags;
};

struct Registry {
    std::shared_mutex mutex;
    // Entries are never removed or replaced, so a found entry stays valid without the lock.
    std::map<std::string, Entry, std::less<>> entries;
    std::map<std::string, OpDomains, std::less<>> domains;
};

template <std::size_t N>
bool listed(const std::array<std::string_view, N>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

OpFlags builtin_flags(std::string_view name) {
    return {listed(AGENT_OPS, name), listed(AGENT_ONLY_OPS, name), listed(HOST_ONLY_OPS, name),
            listed(SEGMENT_OPS, name)};
}

void add(Registry& registry, std::string name, Handler handler, OpDomains domains, OpFlags flags) {
    if (registry.entries.contains(name)) {
        throw std::logic_error("meeting-state op already registered: " + name);
    }
    registry.domains.emplace(name, domains);
    registry.entries.emplace(std::move(name), Entry{std::move(handler), domains, flags});
}

void add_builtin(Registry& registry, std::string_view name, Handler handler, OpDomains domains) {
    add(registry, std::string(name), std::move(handler), domains, builtin_flags(name));
}

void register_builtins(Registry& registry) {
    // Card edits may also supersede insight-review questions (item_effect).
    const OpDomains items{.cards = true, .review = true};
    add_builtin(registry, "add_item", detail::op_add_item, items);
    add_builtin(registry, "update_item", detail::op_update_item, items);
    add_builtin(registry, "remove_item", detail::op_remove_item, items);
    add_builtin(
        registry, "pin_item",
        [](MeetingState& s, Json& o, const OpContext& c) {
            return detail::op_pin_item(s, o, c, true);
        },
        items);
    add_builtin(
        registry, "unpin_item",
        [](MeetingState& s, Json& o, const OpContext& c) {
            return detail::op_pin_item(s, o, c, false);
        },
        items);
    add_builtin(registry, "confirm_item", detail::op_confirm_item, items);
    add_builtin(registry, "set_topic", detail::op_set_topic, {.topic = true});
    add_builtin(registry, "set_rolling_summary", detail::op_set_rolling_summary, {.summary = true});
    add_builtin(registry, "set_title", detail::op_set_title, {.title = true});
    add_builtin(registry, "set_meeting_intent", detail::op_set_meeting_intent, {.intent = true});
    add_builtin(registry, "set_cloud_enabled", detail::op_set_cloud_enabled, {.cloud = true});
    const OpDomains people{.participants = true};
    add_builtin(registry, "upsert_participant", detail::op_upsert_participant, people);
    add_builtin(registry, "suggest_participant_name", detail::op_suggest_participant_name, people);
    add_builtin(registry, "rename_participant", detail::op_rename_participant, people);
    const OpDomains questions{.questions = true};
    add_builtin(registry, "ask_question", detail::op_ask_question, questions);
    add_builtin(registry, "resolve_question", detail::op_resolve_question, questions);
    add_builtin(registry, "answer_question", detail::op_answer_question, questions);
    add_builtin(registry, "dismiss_question", detail::op_dismiss_question, questions);
    add_builtin(registry, "reopen_question", detail::op_reopen_question, questions);
    const OpDomains segments{.segments = true};
    add_builtin(registry, "reassign_segment_speaker", detail::op_reassign_segment_speaker,
                segments);
    add_builtin(registry, "revise_segment_text", detail::op_revise_segment_text, segments);
    // Python's handler table merges the review, fast and custom-report handlers at import.
    detail::add_meeting_ops(
        [&registry](std::string name, Handler handler, OpDomains domains, OpFlags flags) {
            add(registry, std::move(name), std::move(handler), domains, flags);
        });
}

Registry& registry() {
    static Registry instance;
    static const bool registered = (register_builtins(instance), true);
    (void)registered;
    return instance;
}

const Entry* find_entry(std::string_view name) {
    Registry& reg = registry();
    std::shared_lock lock(reg.mutex);
    const auto it = reg.entries.find(name);
    return it == reg.entries.end() ? nullptr : &it->second;
}

} // namespace

std::string OpContext::new_id(std::string_view prefix) const {
    return meeting_state::new_id(prefix, ids ? *ids : random_ids());
}

std::string OpContext::now_iso() const {
    return utc_now_iso(time());
}

void register_op(std::string name, Handler handler, OpDomains domains, OpFlags flags) {
    Registry& reg = registry();
    std::unique_lock lock(reg.mutex);
    add(reg, std::move(name), std::move(handler), domains, flags);
}

const std::map<std::string, OpDomains, std::less<>>& op_domains() {
    return registry().domains;
}

std::optional<OpDomains> op_domain(std::string_view name) {
    const Entry* entry = find_entry(name);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return entry->domains;
}

std::optional<OpFlags> op_flags(std::string_view name) {
    const Entry* entry = find_entry(name);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return entry->flags;
}

std::set<std::string, std::less<>> all_ops() {
    Registry& reg = registry();
    std::shared_lock lock(reg.mutex);
    std::set<std::string, std::less<>> names;
    for (const auto& [name, entry] : reg.entries) {
        names.insert(name);
    }
    return names;
}

OpResult reject(const Json& op, std::string reason) {
    OpResult result;
    result.ok = false;
    result.op = op;
    result.reason = std::move(reason);
    return result;
}

OpResult reject(const Json& op, std::string reason, std::string target_id,
                std::optional<std::int64_t> current_revision) {
    OpResult result = reject(op, std::move(reason));
    result.target_id = std::move(target_id);
    result.current_revision = current_revision;
    return result;
}

std::optional<std::string> check_evidence(const Json& op, const OpContext& ctx) {
    const Json& raw = get(op, "evidence");
    const bool present = truthy(raw);
    if (ctx.is_agent() && !present) {
        return "missing_evidence";
    }
    if (!present) {
        return std::nullopt;
    }
    if (!raw.is_array() || raw.size() > MAX_EVIDENCE_REFS) {
        return "invalid_evidence";
    }
    if (ctx.segment_exists) {
        for (const auto& id : raw) {
            if (!id.is_string() || !ctx.segment_exists(id.get_ref<const std::string&>())) {
                return "unknown_evidence";
            }
        }
        return std::nullopt;
    }
    // Python stores ids of any type when nothing checks them; ids here are text.
    for (const auto& id : raw) {
        if (!id.is_string()) {
            return "invalid_evidence";
        }
    }
    return std::nullopt;
}

std::optional<std::string> check_text(const Json& value, std::size_t max_len) {
    if (!value.is_string()) {
        return "invalid_text";
    }
    const auto& text = value.get_ref<const std::string&>();
    if (py::strip(text).empty()) {
        return "invalid_text";
    }
    if (py::len(text) > max_len) {
        return "text_too_long";
    }
    return std::nullopt;
}

std::optional<std::string> check_data_size(const Json& data) {
    // Every applied op rewrites the whole snapshot, so an unbounded blob from a browser or the
    // model is a memory and write-amplification problem. The measure is Python's json.dumps.
    if (py::len(py::dumps(data)) > MAX_DATA_BYTES) {
        return "data_too_large";
    }
    return std::nullopt;
}

std::vector<OpResult> apply_ops(MeetingState& candidate, const OpContext& ctx, const Json& ops) {
    if (!ops.is_array()) {
        throw py::TypeError("ops must be a list");
    }
    std::vector<OpResult> results;
    results.reserve(ops.size());
    for (const auto& submitted : ops) {
        if (!submitted.is_object() || !submitted.contains("op")) {
            Json shown = submitted;
            if (!submitted.is_object()) {
                shown = Json::object();
                shown["op"] = submitted;
            }
            results.push_back(reject(shown, "malformed_op"));
            continue;
        }
        const Json& name_value = submitted.at("op");
        if (name_value.is_array() || name_value.is_object()) {
            // The handler lookup hashes the name, which Python refuses for a list or a dict;
            // the whole batch fails.
            throw py::TypeError("unhashable type");
        }
        const Entry* entry =
            name_value.is_string() ? find_entry(name_value.get_ref<const std::string&>()) : nullptr;
        if (entry == nullptr) {
            results.push_back(reject(submitted, "unknown_op"));
            continue;
        }
        const std::string& name = name_value.get_ref<const std::string&>();
        if (ctx.is_agent() && !entry->flags.agent) {
            results.push_back(reject(submitted, "agent_forbidden"));
            continue;
        }
        if (ctx.is_agent()) {
            if (auto reason = check_evidence(submitted, ctx)) {
                results.push_back(reject(submitted, std::move(*reason)));
                continue;
            }
        }
        if (entry->flags.agent_only && ctx.actor_type != "agent" && ctx.actor_type != "system") {
            results.push_back(reject(submitted, "agent_only"));
            continue;
        }
        if (entry->flags.host_only && ctx.actor_type == "user") {
            results.push_back(reject(submitted, "host_only"));
            continue;
        }
        Json op = submitted;
        try {
            results.push_back(entry->handler(candidate, op, ctx));
        } catch (const std::exception& error) {
            logger.error_with(error, "State op " + name + " raised; rejecting");
            results.push_back(reject(op, "internal_error"));
        }
    }
    return results;
}

Json filter_notes_ops(const Json& ops,
                      const std::optional<std::set<std::string, std::less<>>>& known_note_ids) {
    Json kept = Json::array();
    if (!ops.is_array()) {
        return kept;
    }
    for (const auto& op : ops) {
        if (!op.is_object()) {
            continue;
        }
        const Json& name = get(op, "op");
        if (!name.is_string() || !listed(NOTES_ONLY_OPS, name.get_ref<const std::string&>())) {
            continue;
        }
        if (name.get_ref<const std::string&>() == "add_item") {
            const Json& card = get(op, "card");
            if (card.is_string() && card.get_ref<const std::string&>() == NOTES_CARD) {
                kept.push_back(op);
            }
            continue;
        }
        const Json& id = get(op, "id");
        if (known_note_ids && id.is_string() &&
            known_note_ids->contains(id.get_ref<const std::string&>())) {
            kept.push_back(op);
        }
    }
    return kept;
}

std::set<std::string, std::less<>> live_note_ids(const Json& state_snapshot) {
    std::set<std::string, std::less<>> ids;
    const Json& cards = get(state_snapshot, "cards");
    const Json& notes = get(cards, NOTES_CARD);
    if (!notes.is_array()) {
        return ids;
    }
    for (const auto& item : notes) {
        if (item.is_object() && truthy(get(item, "id"))) {
            ids.insert(detail::py_str(get(item, "id")));
        }
    }
    return ids;
}

namespace detail {

OpResult accepted(const Json& op, std::optional<std::string> target_id, Json effect, Json inverse) {
    OpResult result;
    result.ok = true;
    result.op = op;
    result.target_id = std::move(target_id);
    result.effect = std::move(effect);
    result.inverse = std::move(inverse);
    return result;
}

std::vector<std::string> evidence_ids(const Json& op) {
    std::vector<std::string> ids;
    const Json& raw = get(op, "evidence");
    if (!raw.is_array()) {
        return ids;
    }
    for (const auto& id : raw) {
        if (id.is_string()) {
            ids.push_back(id.get<std::string>());
        }
    }
    return ids;
}

Json evidence_json(const std::vector<std::string>& ids) {
    Json out = Json::array();
    for (const auto& id : ids) {
        out.push_back(id);
    }
    return out;
}

} // namespace detail

} // namespace fv::meeting_state
