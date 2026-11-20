// host_dashboard.py's pure wording helpers (A18-45, A18-54): _span, _quick, _ago, _clock,
// _iso_ago, _device_label, _speed, event_text, _EVENT_TONES and group_clients.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "ui_remote/host_text.h"

namespace {

using fv::Json;
namespace host_text = fv::host_text;

const std::chrono::time_zone& utc() {
    return *std::chrono::locate_zone("UTC");
}

// 2026-11-14 10:00 UTC
constexpr double kNow = 1'794'650'400.0;

} // namespace

TEST_CASE("Host Mode spans, quick times and speeds read as OpenWhisper's", "[unit]") {
    CHECK(host_text::span(4.2) == "4.2 s");
    CHECK(host_text::span(42) == "42 s");
    CHECK(host_text::span(-3) == "0.0 s");
    CHECK(host_text::span(12 * 60 + 5) == "12 min");
    CHECK(host_text::span(3900) == "1 h 5 min");
    CHECK(host_text::span(7200) == "2 h");
    CHECK(host_text::quick(0.41) == "410 ms");
    CHECK(host_text::quick(1.5) == "1.5 s");
    CHECK(host_text::speed(90, 4.5) == "20×");
    CHECK(host_text::speed(7, 2) == "3.5×");
    CHECK(host_text::speed(0, 2) == "—");
    CHECK(host_text::speed(3, 0) == "—");
    CHECK(host_text::count(1, "transcription", "transcriptions") == "1 transcription");
    CHECK(host_text::count(3, "transcription", "transcriptions") == "3 transcriptions");
    CHECK(host_text::device_label("cuda") == "CUDA");
    CHECK(host_text::device_label("CPU") == "CPU");
    CHECK(host_text::device_label("mps") == "mps");
}

TEST_CASE("Host Mode relative times and clock times", "[unit]") {
    CHECK(host_text::ago(0, kNow, utc()).empty());
    CHECK(host_text::ago(kNow - 30, kNow, utc()) == "just now");
    CHECK(host_text::ago(kNow - 50, kNow, utc()) == "1 min ago");
    CHECK(host_text::ago(kNow - 12 * 60, kNow, utc()) == "12 min ago");
    CHECK(host_text::ago(kNow - 3 * 3600, kNow, utc()) == "3 h ago");
    CHECK(host_text::ago(kNow - 10 * 86400, kNow, utc()) == "Nov 4");
    CHECK(host_text::clock(kNow, utc()) == "10:00 AM");
    CHECK(host_text::clock(kNow + 3 * 3600 + 5 * 60, utc()) == "1:05 PM");
    CHECK(host_text::clock(kNow - 10 * 3600, utc()) == "12:00 AM");
    CHECK(host_text::iso_ago("2026-11-14T07:00:00+00:00", kNow, utc()) == "3 h ago");
    CHECK(host_text::iso_ago("2026-11-14T09:30:00", kNow, utc()) == "30 min ago");
    CHECK(host_text::iso_ago("not a time", kNow, utc()).empty());
    CHECK(host_text::iso_ago("", kNow, utc()).empty());
}

TEST_CASE("Host Mode activity lines and their fallbacks", "[unit]") {
    CHECK(host_text::event_text({{"kind", "transcribed"},
                                 {"name", "laptop"},
                                 {"audio_s", 8.8},
                                 {"host_s", 0.41}}) == "laptop · 8.8 s of audio, back in 410 ms");
    CHECK(host_text::event_text({{"kind", "failed"}, {"name", "laptop"}}) ==
          "laptop · couldn't transcribe: unknown error");
    CHECK(host_text::event_text({{"kind", "connected"}}) == "A paired computer connected");
    CHECK(host_text::event_text({{"kind", "disconnected"}, {"name", ""}}) ==
          "A paired computer disconnected");
    CHECK(host_text::event_text({{"kind", "paired"}, {"name", "desk"}}) == "Paired desk");
    CHECK(host_text::event_text({{"kind", "paired"}, {"name", "desk"}, {"via", "tailscale"}}) ==
          "Paired desk over Tailscale");
    CHECK(host_text::event_text({{"kind", "switched"}, {"name", "desk"}}) ==
          "desk switched the engine to another model");
    CHECK(host_text::event_text({{"kind", "switched"}, {"name", "desk"}, {"label", "v3"}}) ==
          "desk switched the engine to v3");
    CHECK(host_text::event_text({{"kind", "something new"}, {"name", "desk"}}) == "desk");
    CHECK(host_text::event_tone("transcribed") == "success");
    CHECK(host_text::event_tone("failed") == "danger");
    CHECK(host_text::event_tone("connected") == "accent");
    CHECK(host_text::event_tone("disconnected") == "text-muted");
    CHECK(host_text::event_tone("paired") == "purple");
    CHECK(host_text::event_tone("switched") == "warning");
    CHECK(host_text::event_tone("unknown") == "text-muted");
}

TEST_CASE("Host Mode groups connections per paired computer", "[unit]") {
    const auto grouped = host_text::group_clients({
        {.device_id = "d1", .name = "laptop", .address = "a", .busy = false, .since = 100},
        {.device_id = "", .name = "Desk", .address = "b", .busy = false, .since = 0},
        {.device_id = "d1", .name = "laptop", .address = "a", .busy = true, .since = 50},
        {.device_id = "", .name = "", .address = "c"},
    });
    REQUIRE(grouped.size() == 3);
    CHECK(grouped[0].name == "A paired computer");
    CHECK(grouped[1].name == "Desk");
    CHECK(grouped[1].device_id == "Desk");
    CHECK(grouped[2].device_id == "d1");
    CHECK(grouped[2].busy);
    CHECK(grouped[2].since == 50);
    CHECK(grouped[2].connections == 2);
}
