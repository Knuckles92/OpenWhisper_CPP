#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "remote/control.h"

// What the Host Mode dashboard says, as ui_qt/widgets/host_dashboard.py words it: that module's
// pure helpers, Qt-free and UTF-8. Times are the host's time.time() seconds; `zone` is where
// they are shown.
// Affinity: thread-safe (pure functions).
namespace fv::host_text {

// Lines kept in Recent activity (ACTIVITY_ROWS).
inline constexpr int ACTIVITY_ROWS = 12;
// The stat tiles sit four across from this page width, else two, in pixels.
inline constexpr int FOUR_STATS_WIDTH = 700;
// Relative times ("connected 12 min") are redrawn this often while shown, in milliseconds.
inline constexpr int CLOCK_INTERVAL_MS = 30'000;
// Events that arrive together (a live preview's windows) redraw once, in milliseconds.
inline constexpr int COALESCE_MS = 40;
// The MCP server has no events; its status is read this often while shown, in milliseconds.
inline constexpr int MCP_POLL_MS = 1000;

// Settings destinations the page links to.
inline constexpr std::string_view REMOTE_ENGINE_DESTINATION = "remote_engine";
inline constexpr std::string_view MODELS_DESTINATION = "voice_model";
inline constexpr std::string_view MCP_DESTINATION = "mcp";

// _count: "1 transcription", "3 transcriptions".
std::string count(long long n, std::string_view one, std::string_view many);
// _span: "42 s", "4.2 s" under 10, "12 min", "1 h 5 min", "2 h".
std::string span(double seconds);
// _quick: "410 ms" below a second, else as span: how long a decode took.
std::string quick(double seconds);
// _ago: "" for 0, "just now" under 45 s, "N min ago", "N h ago", else "Mon D" in `zone`.
std::string ago(double at, double now, const std::chrono::time_zone& zone);
// _clock: "9:41 AM" in `zone`.
std::string clock(double at, const std::chrono::time_zone& zone);
// _iso_ago: ago() of an ISO 8601 time (naive is local to `zone`), "" when unreadable.
std::string iso_ago(std::string_view iso, double now, const std::chrono::time_zone& zone);
// _device_label: "cuda" and "cpu" upper-cased, anything else as it is.
std::string device_label(std::string_view device);
// _speed: seconds of audio per second of decoding, "38×" or "3.5×"; "—" without both.
std::string speed(double audio_s, double host_s);
// event_text: one Recent activity line.
std::string event_text(const Json& event);
// _EVENT_TONES: the tone of an event's dot (a theme token; "text-muted" for anything else).
std::string event_tone(std::string_view kind);

// group_clients' entry: one paired computer however many connections it opened.
struct GroupedClient {
    std::string device_id; // the grouping key: device_id, else the name
    std::string name;
    std::string address;
    bool busy = false;
    double since = 0.0;
    int connections = 1;
};
// group_clients: merged by device_id (else name), busy OR-ed, the earliest since kept, sorted by
// name ignoring case.
std::vector<GroupedClient> group_clients(const std::vector<remote::ConnectedClient>& clients);

} // namespace fv::host_text
