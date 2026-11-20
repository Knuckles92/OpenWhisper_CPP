#pragma once

#include <string_view>

#include "base/json.h"

namespace fv {

// Where lower layers publish product events (a19's event bus: "transcription.completed" and
// the rest) without depending on fv_automation, which implements it.
// Affinity: thread-safe; publish never blocks on subscribers and never throws.
class EventSink {
public:
    virtual ~EventSink() = default;
    // replay = false keeps frequent, short-lived events (live preview text) out of the replay
    // buffer, as Python's publish(..., replay=False).
    virtual void publish(std::string_view event_type, Json data, bool replay = true) = 0;
};

// Discards every event: the sink until fv_automation provides the real one, and for tests that
// do not look at events.
EventSink& null_event_sink();

} // namespace fv
