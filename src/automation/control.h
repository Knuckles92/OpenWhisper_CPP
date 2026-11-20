#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "base/json.h"

// What the live API and MCP may ask the running app to do (services/automation/control.py). The
// desktop app registers an AppControl at startup; the in-app listener calls it from its own
// thread.
namespace fv::automation {

// ControlError::code values.
inline constexpr std::string_view BUSY = "busy";
inline constexpr std::string_view NOT_FOUND = "not_found";
inline constexpr std::string_view UNAVAILABLE = "unavailable";
inline constexpr std::string_view REFUSED = "refused";

// A refusal the API reports as it is; the message names no internals.
class ControlError : public std::runtime_error {
public:
    ControlError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }
    std::string message() const { return what(); }

private:
    std::string code_;
};

// Affinity: value type.
struct LiveState {
    bool recording = false;
    bool busy = false;
    bool meeting = false;
    std::string mode;
    std::string action_id;
    std::string job_id;
    std::string version;

    // {recording, busy, meeting, mode, action_id, job_id, version}; an empty action_id or
    // job_id is null.
    Json to_dict() const;
    friend bool operator==(const LiveState&, const LiveState&) = default;
};

// Every method may be called from any thread, returns within about two seconds, and throws
// ControlError for a refusal.
// Affinity: thread-safe.
class AppControl {
public:
    virtual ~AppControl() = default;
    virtual LiveState state() = 0;
    // The saved actions' public views (Action::public_dict), as a list.
    virtual Json list_actions() = 0;
    // Runs a saved action on `text`; the job id.
    virtual std::string run_action(std::string_view action_id, std::string_view text,
                                   std::string_view source) = 0;
    // Starts a recording, for an action when `action_id` is set; the job id.
    virtual std::string start_recording(std::optional<std::string> action_id,
                                        std::string_view source) = 0;
    virtual void stop_recording() = 0;
    virtual void cancel() = 0;
    virtual void notify(std::string_view text, std::string_view title = {}) = 0;
};

// The process-wide registration; null clears it.
void set_app_control(std::shared_ptr<AppControl> control);
// The registered control, or null.
std::shared_ptr<AppControl> app_control();
// The registered control; throws ControlError(UNAVAILABLE, "<product> isn't ready for this
// yet.") when there is none.
std::shared_ptr<AppControl> require_app_control();

} // namespace fv::automation
