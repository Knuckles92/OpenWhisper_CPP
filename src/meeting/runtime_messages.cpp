#include "meeting/runtime_messages.h"

#include <format>

#include "base/identity.h"

namespace fv::meeting::messages {

namespace {

std::string prefixed(std::string_view prefix, std::string_view error) {
    std::string text(prefix);
    text += error;
    return text;
}

} // namespace

std::string update_preparing() {
    return std::format("{} is preparing an update. Try again shortly.", identity().product);
}

std::string could_not_start(std::string_view message) {
    if (message.starts_with(kLinuxSystemAudioPrefix)) {
        message = kSystemAudioUnavailable;
    }
    return prefixed("Could not start the meeting: ", message);
}

std::string could_not_pause(std::string_view error) {
    return prefixed("Could not pause the meeting: ", error);
}

std::string could_not_resume(std::string_view error) {
    return prefixed("Could not resume the meeting: ", error);
}

std::string could_not_end(std::string_view error) {
    return prefixed("Could not end the meeting: ", error);
}

std::string could_not_change_insights(std::string_view error) {
    return prefixed("Could not change AI insights: ", error);
}

std::string background_message(std::size_t count, std::string_view outcome) {
    std::string pending;
    if (count != 0) {
        pending = std::format("{} meeting{} finishing in the background. Results appear in Past "
                              "Meetings.",
                              count, count != 1 ? "s" : "");
    }
    std::string text(outcome);
    if (!text.empty() && !pending.empty()) {
        text += ' ';
    }
    return text + pending;
}

std::string retry_failed(std::string_view error) {
    return prefixed("Post-meeting retry failed: ", error);
}

std::string could_not_load_past(std::string_view error) {
    return prefixed("Could not load the past meeting: ", error);
}

std::string could_not_open_past(std::string_view error) {
    return prefixed("Could not open the past meeting: ", error);
}

std::string could_not_copy(std::string_view error) {
    return prefixed("Could not copy the transcript: ", error);
}

std::string could_not_delete(std::string_view error) {
    return prefixed("Could not delete the meeting: ", error);
}

std::string could_not_clear(std::string_view error) {
    return prefixed("Could not clear past meetings: ", error);
}

std::string recovery_scan_failed(std::string_view error) {
    return std::format("{} Details: {}", kRecoveryScanFailed, error);
}

std::string could_not_finalize(std::string_view error) {
    return prefixed("Could not finalize the meeting: ", error);
}

std::string unknown_meeting(std::string_view meeting_id) {
    return std::format("Unknown meeting '{}'", meeting_id);
}

std::string could_not_discard(std::string_view error) {
    return prefixed("Could not discard the meeting: ", error);
}

} // namespace fv::meeting::messages
