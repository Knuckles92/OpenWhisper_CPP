// tests/test_format_utils.py: the duration and timestamp formats, and TranscriptionHistory.create.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

#include "base/testing/fake_clock.h"
#include "history/entry.h"
#include "history/format_utils.h"
#include "history_test_support.h"
#include "pyport/datetime.h"

using fv::format_audio_duration;
using fv::format_timestamp;
using fv::history_test::utc;

// from tests/test_format_utils.py::test_format_audio_duration_keeps_subsecond_precision
TEST_CASE("test_format_audio_duration_keeps_subsecond_precision", "[test_format_utils]") {
    CHECK(format_audio_duration(3.72) == "3.7s");
    CHECK(format_audio_duration(0.0) == "0.0s");
    CHECK(format_audio_duration(59.9) == "59.9s");
    CHECK(format_audio_duration(125.0) == "2m 5s");
    CHECK(format_audio_duration(3600.0) == "1h");
}

TEST_CASE("durations past int64 keep every digit of the hours, as Python's int does",
          "[test_format_utils]") {
    CHECK(format_audio_duration(9.2e18) == "2555555555555555h 33m");
    CHECK(format_audio_duration(9223372036854775808.0) == "2562047788015215h 30m");
    CHECK(format_audio_duration(1e19) == "2777777777777777h 46m");
    CHECK(format_audio_duration(3600.0 * 4611686018427387904.0) == "4611686018427387904h");
}

// from tests/test_format_utils.py::test_format_timestamp_converts_aware_utc_to_local
TEST_CASE("test_format_timestamp_converts_aware_utc_to_local", "[test_format_utils]") {
    // The local zone is the clock's: Pacific daylight time, then Tokyo.
    fv::FakeClock clock(utc(2026, 8, 20), "America/Los_Angeles");
    CHECK(format_timestamp("2026-08-20T16:46:00+00:00", clock) == "Aug 20, 2026 09:46 AM");
    clock.set_zone("Asia/Tokyo");
    CHECK(format_timestamp("2026-08-20T16:46:00+00:00", clock) == "Aug 21, 2026 01:46 AM");
}

// from tests/test_format_utils.py::test_format_timestamp_keeps_naive_legacy_wall_time
TEST_CASE("test_format_timestamp_keeps_naive_legacy_wall_time", "[test_format_utils]") {
    const fv::FakeClock clock(utc(2026, 8, 20), "Asia/Tokyo");
    CHECK(format_timestamp("2026-08-20T16:46:00", clock) == "Aug 20, 2026 04:46 PM");
}

// from tests/test_format_utils.py::test_format_timestamp_returns_unreadable_values_unchanged
TEST_CASE("test_format_timestamp_returns_unreadable_values_unchanged", "[test_format_utils]") {
    CHECK(format_timestamp("") == "");
    CHECK(format_timestamp("not a date") == "not a date");
    CHECK(format_timestamp(std::optional<std::string>{}) == std::nullopt);
}

// from tests/test_format_utils.py::test_history_create_stores_aware_utc_and_empty_preview
TEST_CASE("test_history_create_stores_aware_utc_and_empty_preview", "[test_format_utils]") {
    fv::NewEntry fields;
    fields.text = "   ";
    fields.model = "local_whisper";
    fields.source_name = "sample.wav";
    const fv::HistoryEntry entry = fv::create_entry(fields);

    const fv::py::DateTime parsed = fv::py::fromisoformat(entry.timestamp);
    REQUIRE(parsed.utc_offset_us.has_value());
    CHECK(*parsed.utc_offset_us == 0);
    CHECK(entry.preview_text() == "Empty transcript");
    CHECK(entry.source_name == "sample.wav");
    CHECK(entry.id.size() == 36);
}

TEST_CASE("create_entry writes the clock's UTC time with microseconds and +00:00",
          "[test_format_utils]") {
    fv::FakeClock clock(utc(2026, 3, 15, 14, 30) + std::chrono::microseconds(123456),
                        "America/Los_Angeles");
    fv::NewEntry fields;
    fields.text = "hi";
    fields.model = "base";
    CHECK(fv::create_entry(fields, clock).timestamp == "2026-03-15T14:30:00.123456+00:00");
    clock.set_utc(utc(2026, 3, 15, 14, 30));
    CHECK(fv::create_entry(fields, clock).timestamp == "2026-03-15T14:30:00+00:00");
}
