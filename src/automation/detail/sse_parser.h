#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Claude Code channel's reader of the live event stream (services/automation/channel_bridge.py
// SseParser). Its own, not fv_http's: the resume point follows the WHATWG rule that an id takes
// effect only when its event block ends, so a stream that breaks halfway through an event
// resumes before it.
namespace fv::automation {

// Affinity: value type.
struct SseEvent {
    std::string type;
    std::string data;
    std::string last_id; // the resume point once this event's block ended

    friend bool operator==(const SseEvent&, const SseEvent&) = default;
};

// One line at a time, as https://html.spec.whatwg.org/multipage/server-sent-events.html reads
// them: "field: value" with one leading space dropped, ":" comments ignored, an "id" holding NUL
// ignored, "data" lines joined with "\n"; a blank line ends the block and yields its event when
// it had data (type "message" when it named none).
// Affinity: one thread at a time.
class SseParser {
public:
    explicit SseParser(std::string last_id = {});

    // One line without its line ending; the event a blank line ends, if any.
    std::optional<SseEvent> feed(std::string_view line);
    const std::string& last_id() const { return last_id_; }

private:
    void reset();

    std::string last_id_;
    std::string type_;
    std::vector<std::string> data_;
    std::optional<std::string> id_;
};

namespace detail {

// One line of the stream as read: bytes.decode("utf-8", "replace") with the trailing "\r" and
// "\n" characters stripped (str.rstrip("\r\n")).
std::string sse_line(std::string_view raw);

// bytes.decode("utf-8", "replace"): each maximal invalid subpart becomes one U+FFFD.
std::string decode_utf8_replace(std::string_view bytes);

} // namespace detail

} // namespace fv::automation
