#include "meeting_state/fast_ops.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "meeting_state/detail/extras.h"
#include "meeting_state/detail/py_value.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

using detail::at;
using detail::get;
using detail::has;
using detail::truthy;

OpResult applied(const Json& op, Json effect) {
    OpResult out;
    out.ok = true;
    out.op = op;
    out.effect = std::move(effect);
    return out;
}

bool from(const OpContext& ctx, std::string_view actor_id) {
    return ctx.actor_type == "system" && ctx.actor_id == actor_id;
}

void require_hashable(const Json& key) {
    if (key.is_array() || key.is_object()) {
        throw py::TypeError("unhashable type");
    }
}

// A dict keyed by pulse id: insertion order, an existing key keeps its place, and keys compare
// as Python's do (1, 1.0 and True are one key).
class PulseMap {
public:
    void set(const Json& key, Json value) {
        require_hashable(key);
        for (auto& entry : entries_) {
            if (py::py_equal(entry.first, key)) {
                entry.second = std::move(value);
                return;
            }
        }
        entries_.emplace_back(key, std::move(value));
    }
    void pop(const Json& key) {
        std::erase_if(entries_, [&](const auto& entry) { return py::py_equal(entry.first, key); });
    }
    std::vector<Json> values() const {
        std::vector<Json> out;
        out.reserve(entries_.size());
        for (const auto& entry : entries_) {
            out.push_back(entry.second);
        }
        return out;
    }

private:
    std::vector<std::pair<Json, Json>> entries_;
};

bool orderable_number(const Json& value) {
    return value.is_number() || value.is_boolean();
}

double number_of(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    return value.get<double>();
}

// p1["start_s"] < p2["start_s"] as Python compares them: numbers with numbers, text with text.
bool earlier(const Json& left, const Json& right) {
    if (orderable_number(left) && orderable_number(right)) {
        if (left.is_number_integer() && right.is_number_integer() && !left.is_number_unsigned() &&
            !right.is_number_unsigned()) {
            return left.get<std::int64_t>() < right.get<std::int64_t>();
        }
        return number_of(left) < number_of(right);
    }
    if (left.is_string() && right.is_string()) {
        return left.get_ref<const std::string&>() < right.get_ref<const std::string&>();
    }
    throw py::TypeError("'<' not supported between these start_s values");
}

} // namespace

OpResult publish_highlights(MeetingState& state, Json& op, const OpContext& ctx) {
    if (!from(ctx, LIVE_SIGNALS_ACTOR)) {
        return reject(op, "system_only");
    }
    const bool final_pass = get(op, "final").is_boolean() && get(op, "final").get<bool>();
    const bool running = final_pass ? (state.status == "ending" || state.status == "ended")
                                    : (state.status == "active" || state.status == "paused");
    if (!running) {
        return reject(op, "inactive");
    }
    const Json pulses = has(op, "pulses") ? get(op, "pulses") : Json::array();
    if (!pulses.is_array() || pulses.size() > PULSE_LABELS.size()) {
        return reject(op, "invalid_pulses");
    }
    if (!state.live_highlights.is_array()) {
        throw py::TypeError("live_highlights is not a list");
    }
    PulseMap existing;
    for (const Json& pulse : state.live_highlights) {
        existing.set(at(pulse, "id"), pulse);
    }
    const Json& minute = get(op, "minute");
    if (!minute.is_null()) {
        if (!minute.is_number_integer() ||
            (!minute.is_number_unsigned() && minute.get<std::int64_t>() < 0)) {
            return reject(op, "invalid_minute");
        }
        const std::string stamp = detail::py_str(minute);
        for (const auto& [kind, label] : PULSE_LABELS) {
            existing.pop(Json("pulse_" + stamp + "_" + std::string(kind)));
        }
    }
    for (const Json& pulse : pulses) {
        const Json& kind = get(detail::mapping(pulse), "kind");
        require_hashable(kind);
        if (!kind.is_string() || !pulse_label(kind.get_ref<const std::string&>())) {
            return reject(op, "invalid_pulse");
        }
        if (ctx.segment_exists) {
            const Json& segment = get(pulse, "segment_id");
            if (!segment.is_string() ||
                !ctx.segment_exists(segment.get_ref<const std::string&>())) {
                return reject(op, "unknown_evidence");
            }
        }
        existing.set(at(pulse, "id"), pulse);
    }
    std::vector<Json> merged = existing.values();
    std::vector<std::pair<Json, Json>> keyed;
    keyed.reserve(merged.size());
    for (Json& pulse : merged) {
        Json start = at(pulse, "start_s");
        keyed.emplace_back(std::move(start), std::move(pulse));
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& a, const auto& b) { return earlier(a.first, b.first); });
    Json kept = Json::array();
    const std::size_t first =
        keyed.size() > MAX_LIVE_HIGHLIGHTS ? keyed.size() - MAX_LIVE_HIGHLIGHTS : 0;
    for (std::size_t i = first; i < keyed.size(); ++i) {
        kept.push_back(std::move(keyed[i].second));
    }
    state.live_highlights = std::move(kept);
    Json effect = Json::object();
    effect["entity"] = "live_highlights";
    effect["pulses"] = state.live_highlights;
    return applied(op, std::move(effect));
}

OpResult citation_check(MeetingState& state, Json& op, const OpContext& ctx) {
    if (!from(ctx, CITATION_VERIFIER_ACTOR)) {
        return reject(op, "system_only");
    }
    const Json id = has(op, "id") ? get(op, "id") : Json("");
    CardItem* item = id.is_string() ? state.find_item(id.get_ref<const std::string&>()) : nullptr;
    if (item == nullptr || item->status == "removed") {
        return reject(op, "unknown_item");
    }
    if (!py::py_equal(Json(item->revision), get(op, "revision"))) {
        return reject(op, "stale_check");
    }
    const Json& check = get(op, "check");
    item->citation_check = truthy(check) ? check : Json::object();
    Json effect = Json::object();
    effect["entity"] = "item";
    effect["item"] = item->to_json();
    return applied(op, std::move(effect));
}

OpResult invalidate_citations(MeetingState& state, Json& op, const OpContext& ctx) {
    if (!from(ctx, CITATION_VERIFIER_ACTOR)) {
        return reject(op, "system_only");
    }
    // set(op.get("segment_ids") or []): a string gives its characters, a mapping its keys.
    const Json& raw = get(op, "segment_ids");
    std::set<std::string> ids;
    bool any = false;
    if (truthy(raw)) {
        for (const Json& id : detail::py_list(raw)) {
            require_hashable(id);
            any = true;
            if (id.is_string()) {
                ids.insert(id.get<std::string>());
            }
        }
    }
    Json items = Json::array();
    for (std::size_t card = 0; card < CARD_KEYS.size(); ++card) {
        for (CardItem& item : state.cards[card]) {
            if (item.status == "removed") {
                continue;
            }
            const bool cited = std::any_of(item.evidence.begin(), item.evidence.end(),
                                           [&](const std::string& id) { return ids.contains(id); });
            if (!any || cited) {
                item.citation_check = Json::object();
                items.push_back(item.to_json());
            }
        }
    }
    Json effect = Json::object();
    effect["entity"] = "review";
    effect["review"] = state.insight_review;
    effect["items"] = std::move(items);
    return applied(op, std::move(effect));
}

OpResult voice_feedback(MeetingState& state, Json& op, const OpContext& ctx) {
    if (!from(ctx, VOICE_COMMAND_ACTOR)) {
        return reject(op, "system_only");
    }
    if (state.status != "active" && state.status != "paused") {
        return reject(op, "inactive");
    }
    const std::string message = has(op, "message") ? detail::py_str(get(op, "message")) : "";
    Json feedback = Json::object();
    feedback["message"] =
        py::slice(message, 0, static_cast<std::int64_t>(MAX_VOICE_FEEDBACK_CHARS));
    feedback["at"] = ctx.now_iso();
    state.voice_feedback = feedback;
    Json effect = Json::object();
    effect["entity"] = "voice_feedback";
    effect["feedback"] = std::move(feedback);
    return applied(op, std::move(effect));
}

} // namespace fv::meeting_state
