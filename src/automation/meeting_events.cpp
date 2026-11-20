#include "automation/meeting_events.h"

#include <algorithm>
#include <exception>
#include <optional>

#include "automation/catalog.h"
#include "automation/detail/pyvalue.h"
#include "base/log.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::automation::meeting_events {

namespace {

constexpr log::Logger kLog{"services.automation.meeting_events"};

// Engine statuses that mean the meeting is capturing.
bool running(const Json& status) {
    if (!status.is_string()) {
        return false;
    }
    const auto& text = status.get_ref<const std::string&>();
    return text == "active" || text == "paused";
}

// _seconds: a number rounded to 3 places, else null (a bool is not a number here).
Json seconds(const Json& value) {
    if (!value.is_number()) {
        return nullptr;
    }
    return py::round_ndigits(value.get<double>(), 3);
}

// str(value or ""), as the payload builders read the engine's fields.
std::string text_of(const Json& value) {
    return detail::truthy(value) ? detail::py_str(value) : std::string();
}

// _segment: one committed transcript row as a meeting.segments item.
Json segment(const Json& row) {
    const Json& stated = detail::get(row, "speaker");
    const std::string speaker = stated.is_string() ? stated.get<std::string>() : std::string();
    const Json& id = detail::get(row, "id");
    Json item = Json::object();
    item["segment_id"] = text_of(detail::truthy(id) ? id : detail::get(row, "segment_id"));
    // The engine's rows name the speaker by participant id; the History API's meeting insights
    // list each participant's display name.
    item["speaker"] =
        !speaker.empty() ? speaker : text_of(detail::get(row, "speaker_participant_id"));
    item["text"] = text_of(detail::get(row, "text"));
    item["start_s"] = seconds(detail::get(row, "start_s"));
    item["end_s"] = seconds(detail::get(row, "end_s"));
    return item;
}

MeetingEventForwarder& process_forwarder() {
    static MeetingEventForwarder forwarder;
    return forwarder;
}

} // namespace

void MeetingEventForwarder::forget() {
    std::lock_guard lock(mutex_);
    announced_.clear();
}

bool MeetingEventForwarder::first_announcement(const std::string& meeting_id) {
    std::lock_guard lock(mutex_);
    if (std::ranges::find(announced_, meeting_id) != announced_.end()) {
        return false;
    }
    announced_.push_back(meeting_id);
    while (announced_.size() > ANNOUNCED_LIMIT) {
        announced_.pop_front();
    }
    return true;
}

void MeetingEventForwarder::forward(std::string_view kind, const Json& raw_payload,
                                    std::string_view given_id, EventSink& sink) {
    try {
        const Json payload = raw_payload.is_object() ? raw_payload : Json::object();
        const std::string meeting_id =
            !given_id.empty() ? std::string(given_id) : text_of(detail::get(payload, "meeting_id"));
        if (meeting_id.empty()) {
            return;
        }
        if (kind == "status") {
            if (running(detail::get(payload, "status")) && first_announcement(meeting_id)) {
                const Json& title = detail::get(payload, "title");
                sink.publish(catalog::MEETING_STARTED,
                             catalog::meeting_started(
                                 meeting_id, title.is_string() ? py::strip(title.get<std::string>())
                                                               : std::string()));
            }
        } else if (kind == "ended") {
            const Json& status = detail::get(payload, "status");
            const bool named = status.is_string() && !status.get<std::string>().empty();
            sink.publish(
                catalog::MEETING_ENDED,
                catalog::meeting_ended(meeting_id, named ? status.get<std::string>() : "ended"));
        } else if (kind == "segments") {
            if (payload.contains("removed_ids")) {
                return; // a re-decode replacing the transcript, not new lines
            }
            const Json& items = detail::get(payload, "items");
            Json rows = Json::array();
            if (items.is_array()) {
                for (const Json& row : items) {
                    if (row.is_object() && !py::strip(text_of(detail::get(row, "text"))).empty()) {
                        rows.push_back(segment(row));
                    }
                }
            }
            if (!rows.empty()) {
                sink.publish(catalog::MEETING_SEGMENTS,
                             catalog::meeting_segments(meeting_id, rows));
            }
        } else if (kind == "voice_command") {
            const Json& command = detail::get(payload, "command");
            if (command.is_string()) {
                const std::string said = py::strip(command.get<std::string>());
                if (!said.empty()) {
                    sink.publish(catalog::MEETING_VOICE_COMMAND,
                                 catalog::meeting_voice_command(meeting_id, said));
                }
            }
        }
    } catch (const std::exception& error) {
        kLog.warning("Could not publish meeting event {}: {}", kind, detail::error_type(error));
    } catch (...) {
        kLog.warning("Could not publish meeting event {}: unknown", kind);
    }
}

void forward(std::string_view kind, const Json& payload, std::string_view meeting_id,
             EventSink& sink) {
    process_forwarder().forward(kind, payload, meeting_id, sink);
}

void forget_announced() {
    process_forwarder().forget();
}

} // namespace fv::automation::meeting_events
