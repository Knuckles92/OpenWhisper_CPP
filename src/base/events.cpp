#include "base/events.h"

namespace fv {

namespace {

class NullEventSink final : public EventSink {
public:
    void publish(std::string_view, Json, bool) override {}
};

} // namespace

EventSink& null_event_sink() {
    static NullEventSink sink;
    return sink;
}

} // namespace fv
