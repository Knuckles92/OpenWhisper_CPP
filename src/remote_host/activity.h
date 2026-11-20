#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"

// What paired computers have asked of this host since sharing started
// (services/remote_asr/activity.py). Kept in memory only: the counts start over when sharing
// starts again or the app restarts, and nothing here is written to disk. The host records into
// it from its connections; the Host Mode dashboard reads snapshots.
namespace fv::remote_host {

// Events kept for the dashboard's recent activity list.
inline constexpr int kMaxEvents = 60;
// A failure's reason is cut to this many code points in the event list.
inline constexpr int kMaxReasonChars = 160;

// A snapshot with nothing counted: {since, transcriptions, audio_s, host_s, previews, errors,
// last_at, serial, devices, events}.
Json empty_snapshot(double since = 0.0);

// Totals, per-computer counts, and a short list of recent events. Events are objects with
// `serial` (increasing, also across reset), `at` (wall clock, seconds since the epoch), `kind`
// (transcribed, failed, connected, disconnected, paired, switched) and `name`, plus what the
// kind needs: `audio_s` and `host_s` for a transcription, `reason` for a failure, `via` for a
// pairing, `label` for a model switch. Times are the clock's UTC as seconds (time.time()).
// Affinity: thread-safe.
class HostActivity {
public:
    // py::ValueError "maxlen must be non-negative" for a negative max_events, as deque's.
    explicit HostActivity(Clock& clock, int max_events = kMaxEvents);

    // Starts counting again (sharing just started); the serial keeps rising.
    void reset();

    void transcribed(std::string_view device_id, std::string_view name, double audio_s,
                     double host_s);
    // A live preview window: counted, but too frequent to list.
    void previewed(std::string_view device_id, std::string_view name);
    // The reason's whitespace runs become one space, and past kMaxReasonChars it is cut and
    // ends in "…".
    void failed(std::string_view device_id, std::string_view name, std::string_view reason);
    // A computer's dictation and a meeting each hold a connection, so "connected" is listed for
    // the first only and "disconnected" for the last.
    void connected(std::string_view device_id, std::string_view name);
    void disconnected(std::string_view device_id, std::string_view name);
    void paired(std::string_view device_id, std::string_view name, std::string_view via);
    // A paired computer switched this computer's model.
    void switched(std::string_view name, std::string_view label);
    // Its counts go by the new name; events already listed keep the old one.
    void renamed(std::string_view device_id, std::string_view name);

    // Totals, per-device counts ({id: {name, transcriptions, audio_s, host_s, previews, errors,
    // last_at}}), and the newest `events` events first (all by default).
    Json snapshot(std::optional<int> events = {}) const;
    // When counting started: the clock at construction or the last reset.
    double started_at() const;

private:
    struct Device {
        std::string name;
        std::int64_t transcriptions = 0;
        double audio_s = 0.0;
        double host_s = 0.0;
        std::int64_t previews = 0;
        std::int64_t errors = 0;
        double last_at = 0.0;
    };

    double now() const;
    Device& device(std::string_view device_id, std::string_view name);
    void event(std::string_view kind, std::string_view name, Json fields);

    Clock& clock_;
    mutable std::mutex mutex_;
    std::size_t max_events_;
    std::deque<Json> events_;
    // In first-seen order, as Python's dict keeps them: the totals add up in that order.
    std::vector<std::pair<std::string, Device>> devices_;
    // Open connections per device.
    std::vector<std::pair<std::string, int>> connections_;
    std::int64_t serial_ = 0;
    double started_at_ = 0.0;
};

} // namespace fv::remote_host
