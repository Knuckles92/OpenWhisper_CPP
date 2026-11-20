#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The WHATWG event-stream format ("Server-sent events", 9.2.6) that the providers' streaming
// endpoints speak: lines end with CR, LF or CRLF, a leading U+FEFF is dropped once, a line
// starting with ':' is a comment (OpenRouter's ": OPENROUTER PROCESSING" keep-alive), a blank
// line dispatches the event. Bytes are kept as sent; the JSON readers check them.
namespace fv::llm {

struct SseEvent {
    std::string event; // the "event" field, "message" when the block set none
    std::string data;  // the "data" lines joined with LF, without the last LF
    std::string id;    // the last event ID: an "id" field persists until the next one
    // The last "retry" field (all ASCII digits) since the previous event, in milliseconds.
    std::optional<int> retry_ms;
};

using SseHandler = std::function<void(const SseEvent&)>;

// Affinity: not thread-safe; one stream per parser.
class SseParser {
public:
    // Any split of the stream into calls gives the same events, a CRLF split between two calls
    // included.
    void feed(std::string_view bytes, const SseHandler& on_event);
    // The end of the stream: an unterminated last line is taken as a line, and an event whose
    // blank line never came is dispatched. WHATWG discards that event; providers that close the
    // connection right after their last data line would lose it.
    void finish(const SseHandler& on_event);

private:
    void line(std::string_view text, const SseHandler& on_event);
    void dispatch(const SseHandler& on_event);

    std::string pending_;   // the current line, not yet ended
    bool started_ = false;  // the BOM check is behind us
    bool after_cr_ = false; // the last byte ended a line with CR: an LF next belongs to it
    std::string event_;     // the block's event type
    std::string data_;      // the block's data, each line followed by LF
    std::string last_id_;   // the last event ID buffer
    std::optional<int> retry_;
};

} // namespace fv::llm
