#include "meeting_state/types.h"

namespace fv::meeting_state {

namespace {

template <class T> Json optional_json(const std::optional<T>& value) {
    return value ? Json(*value) : Json();
}

} // namespace

Json OpResult::to_json() const {
    Json out = Json::object();
    out["ok"] = ok;
    out["op"] = op;
    out["reason"] = optional_json(reason);
    out["target_id"] = optional_json(target_id);
    out["effect"] = effect;
    out["inverse"] = inverse;
    out["current_revision"] = optional_json(current_revision);
    out["seq"] = optional_json(seq);
    return out;
}

} // namespace fv::meeting_state
