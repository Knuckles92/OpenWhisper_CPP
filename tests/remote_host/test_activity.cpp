// HostActivity (services/remote_asr/activity.py): the four test_host_mode.py tests of what the
// host counts, then the golden vectors (tests/golden/services.remote_asr.activity.json).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "remote_host/activity.h"

using fv::FakeClock;
using fv::Json;
using fv::remote_host::empty_snapshot;
using fv::remote_host::HostActivity;
using fv::remote_host::kMaxReasonChars;
using namespace std::chrono_literals;

namespace {

constexpr const char* kModule = "services.remote_asr.activity";

// The tests' Clock(now=1000.0): wall time 1000 s after the epoch.
FakeClock clock_at(double seconds) {
    return FakeClock(std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(seconds))));
}

void tick(FakeClock& clock, double seconds) {
    clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(seconds)));
}

std::vector<std::string> kinds(const Json& snapshot) {
    std::vector<std::string> out;
    for (const auto& event : snapshot.at("events")) {
        out.push_back(event.at("kind").get<std::string>());
    }
    return out;
}

} // namespace

TEST_CASE("activity counts each computers requests and lists newest first", "[test_host_mode]") {
    // from
    // tests/test_host_mode.py::test_activity_counts_each_computers_requests_and_lists_newest_first
    FakeClock clock = clock_at(1000.0);
    HostActivity activity(clock);
    activity.connected("d1", "laptop");
    clock.advance(5s);
    activity.transcribed("d1", "laptop", 8.8, 0.4);
    activity.previewed("d1", "laptop");
    clock.advance(5s);
    activity.transcribed("d2", "desk", 2.0, 0.1);

    const Json snapshot = activity.snapshot();
    CHECK(snapshot["transcriptions"] == 2);
    CHECK(snapshot["audio_s"].get<double>() == Catch::Approx(10.8));
    CHECK(snapshot["host_s"].get<double>() == Catch::Approx(0.5));
    CHECK(snapshot["previews"] == 1);
    CHECK(snapshot["last_at"].get<double>() == 1010.0);
    CHECK(snapshot["devices"]["d1"]["transcriptions"] == 1);
    CHECK(snapshot["devices"]["d1"]["previews"] == 1);
    // Live preview windows are counted but too frequent to list.
    CHECK(kinds(snapshot) == std::vector<std::string>{"transcribed", "transcribed", "connected"});
    CHECK(snapshot["events"][0]["name"] == "desk");
    CHECK(snapshot["serial"] == 3);
    const Json newest = activity.snapshot(1)["events"];
    REQUIRE(newest.size() == 1);
    CHECK(newest[0]["name"] == "desk");
}

TEST_CASE("a computer connects once however many connections it opens", "[test_host_mode]") {
    // from tests/test_host_mode.py::test_a_computer_connects_once_however_many_connections_it_opens
    FakeClock clock = clock_at(1000.0);
    HostActivity activity(clock);
    activity.connected("d1", "laptop");
    activity.connected("d1", "laptop"); // a meeting, beside its dictation
    activity.disconnected("d1", "laptop");
    CHECK(kinds(activity.snapshot()) == std::vector<std::string>{"connected"});
    activity.disconnected("d1", "laptop");
    CHECK(kinds(activity.snapshot()) == std::vector<std::string>{"disconnected", "connected"});
}

TEST_CASE("sharing again starts a new session but keeps the serial rising", "[test_host_mode]") {
    // from
    // tests/test_host_mode.py::test_sharing_again_starts_a_new_session_but_keeps_the_serial_rising
    FakeClock clock = clock_at(1000.0);
    HostActivity activity(clock);
    activity.connected("d1", "laptop");
    activity.transcribed("d1", "laptop", 3.0, 0.2);
    const auto serial = activity.snapshot()["serial"].get<std::int64_t>();
    clock.advance(1000s);
    activity.reset();
    // A connection from before the restart closes afterwards.
    activity.disconnected("d1", "laptop");

    const Json snapshot = activity.snapshot();
    CHECK(snapshot["since"].get<double>() == 2000.0);
    CHECK(activity.started_at() == 2000.0);
    CHECK(snapshot["transcriptions"] == 0);
    CHECK(snapshot["events"] == Json::array());
    CHECK(snapshot["devices"] == Json::object());
    activity.paired("d2", "desk", "tailscale");
    CHECK(activity.snapshot()["serial"].get<std::int64_t>() > serial);
}

TEST_CASE("a failure keeps a short one line reason", "[test_host_mode]") {
    // from tests/test_host_mode.py::test_a_failure_keeps_a_short_one_line_reason
    FakeClock clock = clock_at(1000.0);
    HostActivity activity(clock);
    activity.failed("d1", "laptop", "Engine\n   exploded " + std::string(400, 'x'));
    const Json event = activity.snapshot()["events"][0];
    CHECK(event["kind"] == "failed");
    const auto reason = event["reason"].get<std::string>();
    CHECK(reason.starts_with("Engine exploded x"));
    CHECK(fv::py::len(reason) == static_cast<std::size_t>(kMaxReasonChars));
    CHECK(reason.ends_with("…"));
    CHECK(activity.snapshot()["errors"] == 1);
}

TEST_CASE("a failure's reason joins every whitespace run as str.split does", "[activity]") {
    FakeClock clock = clock_at(1000.0);
    HostActivity activity(clock);
    // str.isspace: the em space, no-break space, ideographic space and the ASCII separators
    // \x1c to \x1f split as well as spaces and line ends.
    // (Each escape ends its literal: a hex escape would swallow the letter after it.)
    activity.failed("d1", "laptop",
                    "\xe2\x80\x83"
                    "a\xc2\xa0"
                    "b\xe3\x80\x80"
                    "c\x1c"
                    "d\x1f\r\n e\v\f ");
    CHECK(activity.snapshot()["events"][0]["reason"] == "a b c d e");
    // Cut at 159 code points, not bytes, then the trailing space before the ellipsis goes.
    const std::string accented = [] {
        std::string out;
        for (int i = 0; i < 158; ++i) {
            out += "\xc3\xa9";
        }
        return out + " zz";
    }();
    activity.failed("d1", "laptop", accented);
    const auto reason = activity.snapshot()["events"][0]["reason"].get<std::string>();
    CHECK(fv::py::len(reason) == 159);
    CHECK(reason.ends_with("\xc3\xa9\xe2\x80\xa6"));
}

TEST_CASE("the event list keeps the newest max_events", "[activity]") {
    FakeClock clock = clock_at(1000.0);
    HostActivity activity(clock, 2);
    activity.switched("laptop", "A");
    activity.switched("laptop", "B");
    activity.switched("laptop", "C");
    const Json snapshot = activity.snapshot();
    REQUIRE(snapshot["events"].size() == 2);
    CHECK(snapshot["events"][0]["label"] == "C");
    CHECK(snapshot["events"][1]["label"] == "B");
    CHECK(snapshot["serial"] == 3);
    CHECK(activity.snapshot(-1)["events"] == Json::array());
    CHECK_THROWS_AS(HostActivity(clock, -1), fv::py::ValueError);
}

TEST_CASE("empty_snapshot has the dashboard's shape", "[activity]") {
    const Json snapshot = empty_snapshot(12.5);
    std::vector<std::string> keys;
    for (const auto& item : snapshot.items()) {
        keys.push_back(item.key());
    }
    CHECK(keys == std::vector<std::string>{"since", "transcriptions", "audio_s", "host_s",
                                           "previews", "errors", "last_at", "serial", "devices",
                                           "events"});
    CHECK(snapshot["since"].get<double>() == 12.5);
    CHECK(snapshot["audio_s"].is_number_float());
    CHECK(snapshot["transcriptions"].is_number_integer());
}

// ---- golden vectors ----

TEST_CASE("empty_snapshot matches Python", "[golden]") {
    fv::golden::for_each(kModule, "empty_snapshot", [](const fv::golden::Case& c) {
        const Json actual =
            c.arg_count() == 0 ? empty_snapshot() : empty_snapshot(c.arg<double>(0));
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("HostActivity scripts match Python", "[golden]") {
    fv::golden::for_each(kModule, "script", [](const fv::golden::Case& c) {
        FakeClock clock = clock_at(1000.0);
        HostActivity activity(clock, c.arg<int>(0));
        Json snapshots = Json::array();
        for (const auto& step : c.arg<Json>(1)) {
            const auto op = step.at(0).get<std::string>();
            const auto text = [&](std::size_t i) {
                return step.at(i).get<std::string>();
            };
            if (op == "tick") {
                tick(clock, step.at(1).get<double>());
            } else if (op == "snapshot") {
                const Json& events = step.at(1);
                snapshots.push_back(activity.snapshot(
                    events.is_null() ? std::nullopt : std::optional<int>(events.get<int>())));
            } else if (op == "reset") {
                activity.reset();
            } else if (op == "connected") {
                activity.connected(text(1), text(2));
            } else if (op == "disconnected") {
                activity.disconnected(text(1), text(2));
            } else if (op == "transcribed") {
                activity.transcribed(text(1), text(2), step.at(3).get<double>(),
                                     step.at(4).get<double>());
            } else if (op == "previewed") {
                activity.previewed(text(1), text(2));
            } else if (op == "failed") {
                activity.failed(text(1), text(2), text(3));
            } else if (op == "paired") {
                activity.paired(text(1), text(2), text(3));
            } else if (op == "switched") {
                activity.switched(text(1), text(2));
            } else if (op == "renamed") {
                activity.renamed(text(1), text(2));
            } else {
                FAIL("unknown step " << op);
            }
        }
        const Json expected = c.result<Json>();
        CHECK(fv::golden::equal(expected, snapshots));
        if (!fv::golden::equal(expected, snapshots)) {
            // Name the first snapshot that differs, so a failure is readable.
            for (std::size_t i = 0; i < expected.size() && i < snapshots.size(); ++i) {
                if (!fv::golden::equal(expected[i], snapshots[i])) {
                    FAIL_CHECK("snapshot " << i << "\nexpected " << expected[i].dump()
                                           << "\nactual   " << snapshots[i].dump());
                    break;
                }
            }
        }
    });
}
