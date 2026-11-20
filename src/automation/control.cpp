#include "automation/control.h"

#include <format>
#include <mutex>
#include <utility>

#include "base/identity.h"

namespace fv::automation {

namespace {

struct Registration {
    std::mutex mutex;
    std::shared_ptr<AppControl> control;
};

Registration& registration() {
    static Registration instance;
    return instance;
}

Json text_or_null(const std::string& value) {
    return value.empty() ? Json(nullptr) : Json(value);
}

} // namespace

Json LiveState::to_dict() const {
    Json data = Json::object();
    data["recording"] = recording;
    data["busy"] = busy;
    data["meeting"] = meeting;
    data["mode"] = mode;
    data["action_id"] = text_or_null(action_id);
    data["job_id"] = text_or_null(job_id);
    data["version"] = version;
    return data;
}

void set_app_control(std::shared_ptr<AppControl> control) {
    auto& state = registration();
    std::lock_guard lock(state.mutex);
    state.control = std::move(control);
}

std::shared_ptr<AppControl> app_control() {
    auto& state = registration();
    std::lock_guard lock(state.mutex);
    return state.control;
}

std::shared_ptr<AppControl> require_app_control() {
    auto control = app_control();
    if (!control) {
        throw ControlError(std::string(UNAVAILABLE),
                           std::format("{} isn't ready for this yet.", identity().product));
    }
    return control;
}

} // namespace fv::automation
