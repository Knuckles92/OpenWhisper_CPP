// tests/test_recording_retention.py: the retention resolvers, rotation by count and folder size,
// the usage totals and entry deletion. Where Python patched services.history_manager.db with a
// mock, these tests use a real database and look at the rows the calls would have changed.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "history/manager.h"
#include "history/retention_settings.h"
#include "history_test_support.h"
#include "settings/store.h"

using fv::HistoryManager;
using fv::Json;
using fv::resolve_max_saved_recordings;
using fv::resolve_max_saved_recordings_bytes;
using fv::history_test::list_names;
using fv::history_test::TestDatabase;
using Names = std::vector<std::string>;

namespace {

constexpr std::int64_t MB = 1024 * 1024;
// config.MAX_SAVED_RECORDINGS and config.MAX_SAVED_RECORDINGS_MB.
constexpr std::int64_t max_saved_recordings = 20;
constexpr std::int64_t max_saved_recordings_mb = 1024;

// TestRecordingRotation's folder and its helpers, on a real database.
struct Rotation {
    TestDatabase db;
    std::filesystem::path recordings_dir = db.recordings();

    Rotation() { std::filesystem::create_directories(recordings_dir); }

    std::filesystem::path touch_recording(const std::string& stamp, std::size_t size = 4) {
        const auto path = recordings_dir / ("recording_" + stamp + ".wav");
        fv::history_test::write_file(path, std::string(size, 'R'));
        return path;
    }
    HistoryManager manager(std::optional<std::int64_t> max_recordings,
                           std::optional<std::int64_t> max_bytes = std::nullopt,
                           const fv::Clock* clock = nullptr) {
        return HistoryManager(db.database(), recordings_dir,
                              fv::RecordingRetention{max_recordings, max_bytes},
                              fv::history_test::services_with(nullptr, clock));
    }
    // An entry naming `audio_file`, so a test can see whether its reference was cleared.
    void reference(const std::string& id, const std::string& audio_file) {
        fv::HistoryEntry entry;
        entry.id = id;
        entry.text = id;
        entry.timestamp = "2026-01-01T00:00:00+00:00";
        entry.model = "base";
        entry.audio_file = audio_file;
        db.repository().add_history_entry(entry);
    }
    std::optional<std::string> audio_of(const std::string& id) {
        return db.repository().get_history_entry_by_id(id)->audio_file;
    }
};

} // namespace

// ---- TestResolveMaxSavedRecordings

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordings::test_default_is_custom_config_limit
TEST_CASE("TestResolveMaxSavedRecordings::test_default_is_custom_config_limit",
          "[test_recording_retention]") {
    CHECK(resolve_max_saved_recordings(Json::object()) == max_saved_recordings);
}

// from tests/test_recording_retention.py::TestResolveMaxSavedRecordings::test_keep_all_returns_none
TEST_CASE("TestResolveMaxSavedRecordings::test_keep_all_returns_none",
          "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "keep_all"}, {"max_saved_recordings", 5}};
    CHECK_FALSE(resolve_max_saved_recordings(settings));
}

// from tests/test_recording_retention.py::TestResolveMaxSavedRecordings::test_custom_uses_count
TEST_CASE("TestResolveMaxSavedRecordings::test_custom_uses_count", "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "custom"}, {"max_saved_recordings", 7}};
    CHECK(resolve_max_saved_recordings(settings) == 7);
}

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordings::test_custom_clamps_to_at_least_one
TEST_CASE("TestResolveMaxSavedRecordings::test_custom_clamps_to_at_least_one",
          "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "custom"}, {"max_saved_recordings", 0}};
    CHECK(resolve_max_saved_recordings(settings) == 1);
}

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordings::test_invalid_count_falls_back_to_config
TEST_CASE("TestResolveMaxSavedRecordings::test_invalid_count_falls_back_to_config",
          "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "custom"},
                           {"max_saved_recordings", "nope"}};
    CHECK(resolve_max_saved_recordings(settings) == max_saved_recordings);
}

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordings::test_size_limit_disables_count
TEST_CASE("TestResolveMaxSavedRecordings::test_size_limit_disables_count",
          "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "size_limit"}, {"max_saved_recordings", 5}};
    CHECK_FALSE(resolve_max_saved_recordings(settings));
}

// ---- TestResolveMaxSavedRecordingsBytes

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordingsBytes::test_default_has_no_size_limit
TEST_CASE("TestResolveMaxSavedRecordingsBytes::test_default_has_no_size_limit",
          "[test_recording_retention]") {
    CHECK_FALSE(resolve_max_saved_recordings_bytes(Json::object()));
}

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordingsBytes::test_other_modes_have_no_size_limit
TEST_CASE("TestResolveMaxSavedRecordingsBytes::test_other_modes_have_no_size_limit",
          "[test_recording_retention]") {
    for (const char* mode : {"keep_all", "custom"}) {
        const Json settings = {{"recording_retention_mode", mode}, {"max_saved_recordings_mb", 50}};
        CHECK_FALSE(resolve_max_saved_recordings_bytes(settings));
    }
}

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordingsBytes::test_size_limit_converts_megabytes
TEST_CASE("TestResolveMaxSavedRecordingsBytes::test_size_limit_converts_megabytes",
          "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "size_limit"},
                           {"max_saved_recordings_mb", 250}};
    CHECK(resolve_max_saved_recordings_bytes(settings) == 250 * MB);
}

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordingsBytes::test_size_limit_defaults_to_config
TEST_CASE("TestResolveMaxSavedRecordingsBytes::test_size_limit_defaults_to_config",
          "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "size_limit"}};
    CHECK(resolve_max_saved_recordings_bytes(settings) == max_saved_recordings_mb * MB);
}

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordingsBytes::test_invalid_size_falls_back_to_config
TEST_CASE("TestResolveMaxSavedRecordingsBytes::test_invalid_size_falls_back_to_config",
          "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "size_limit"},
                           {"max_saved_recordings_mb", "big"}};
    CHECK(resolve_max_saved_recordings_bytes(settings) == max_saved_recordings_mb * MB);
}

// from
// tests/test_recording_retention.py::TestResolveMaxSavedRecordingsBytes::test_size_clamps_to_at_least_one_megabyte
TEST_CASE("TestResolveMaxSavedRecordingsBytes::test_size_clamps_to_at_least_one_megabyte",
          "[test_recording_retention]") {
    const Json settings = {{"recording_retention_mode", "size_limit"},
                           {"max_saved_recordings_mb", -3}};
    CHECK(resolve_max_saved_recordings_bytes(settings) == MB);
}

// ---- TestRecordingRotation

// from tests/test_recording_retention.py::TestRecordingRotation::test_rotate_keeps_newest_n
TEST_CASE("TestRecordingRotation::test_rotate_keeps_newest_n", "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(2);
    r.touch_recording("20260101_120000");
    r.touch_recording("20260102_120000");
    r.touch_recording("20260103_120000");

    manager.rotate_recordings();

    CHECK(list_names(r.recordings_dir) ==
          Names{"recording_20260102_120000.wav", "recording_20260103_120000.wav"});
}

// from tests/test_recording_retention.py::TestRecordingRotation::test_keep_all_skips_rotation
TEST_CASE("TestRecordingRotation::test_keep_all_skips_rotation", "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt);
    r.touch_recording("20260101_120000");
    r.touch_recording("20260102_120000");
    r.touch_recording("20260103_120000");

    manager.rotate_recordings();

    CHECK(list_names(r.recordings_dir).size() == 3);
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_set_retention_applies_immediately
TEST_CASE("TestRecordingRotation::test_set_retention_applies_immediately",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt);
    r.touch_recording("20260101_120000");
    r.touch_recording("20260102_120000");
    r.touch_recording("20260103_120000");

    manager.set_retention(1, std::nullopt);

    CHECK(list_names(r.recordings_dir) == Names{"recording_20260103_120000.wav"});
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_size_limit_keeps_newest_that_fit
TEST_CASE("TestRecordingRotation::test_size_limit_keeps_newest_that_fit",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt, 250);
    r.touch_recording("20260101_120000", 100);
    r.touch_recording("20260102_120000", 100);
    r.touch_recording("20260103_120000", 100);
    r.touch_recording("20260104_120000", 100);
    r.reference("first", "recording_20260101_120000.wav");
    r.reference("second", "recording_20260102_120000.wav");
    r.reference("third", "recording_20260103_120000.wav");

    manager.rotate_recordings();

    CHECK(list_names(r.recordings_dir) ==
          Names{"recording_20260103_120000.wav", "recording_20260104_120000.wav"});
    // clear_history_audio_file ran for exactly the two deleted recordings.
    CHECK_FALSE(r.audio_of("first"));
    CHECK_FALSE(r.audio_of("second"));
    CHECK(r.audio_of("third") == "recording_20260103_120000.wav");
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_size_limit_exact_fit_keeps_everything
TEST_CASE("TestRecordingRotation::test_size_limit_exact_fit_keeps_everything",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt, 300);
    r.touch_recording("20260101_120000", 100);
    r.touch_recording("20260102_120000", 100);
    r.touch_recording("20260103_120000", 100);

    manager.rotate_recordings();

    CHECK(list_names(r.recordings_dir).size() == 3);
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_size_limit_always_keeps_newest
TEST_CASE("TestRecordingRotation::test_size_limit_always_keeps_newest",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt, 50);
    r.touch_recording("20260101_120000", 10);
    r.touch_recording("20260102_120000", 200);

    manager.rotate_recordings();

    CHECK(list_names(r.recordings_dir) == Names{"recording_20260102_120000.wav"});
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_count_and_size_limits_both_apply
TEST_CASE("TestRecordingRotation::test_count_and_size_limits_both_apply",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(3, 1000);
    for (int day = 1; day <= 5; ++day) {
        r.touch_recording("2026010" + std::to_string(day) + "_120000", 100);
    }

    manager.rotate_recordings();

    CHECK(list_names(r.recordings_dir) == Names{"recording_20260103_120000.wav",
                                                "recording_20260104_120000.wav",
                                                "recording_20260105_120000.wav"});
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_set_retention_switches_to_size_limit
TEST_CASE("TestRecordingRotation::test_set_retention_switches_to_size_limit",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(10);
    r.touch_recording("20260101_120000", 100);
    r.touch_recording("20260102_120000", 100);
    r.touch_recording("20260103_120000", 100);

    manager.set_retention(std::nullopt, 150);

    CHECK_FALSE(manager.max_recordings());
    CHECK(list_names(r.recordings_dir) == Names{"recording_20260103_120000.wav"});
}

// from tests/test_recording_retention.py::TestRecordingRotation::test_saving_rotates_by_size
TEST_CASE("TestRecordingRotation::test_saving_rotates_by_size", "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt, 150);
    r.touch_recording("20260101_120000", 100);
    const auto source = r.db.root() / "new.wav";
    fv::history_test::write_file(source, std::string(100, 'N'));

    const auto saved = manager.preserve_recording(source);
    std::filesystem::remove(source);

    REQUIRE(saved);
    CHECK(list_names(r.recordings_dir) == Names{*saved});
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_explicit_limit_does_not_read_settings
TEST_CASE("TestRecordingRotation::test_explicit_limit_does_not_read_settings",
          "[test_recording_retention]") {
    // The explicit-limits constructor takes no settings at all, so none can be read.
    Rotation r;
    auto manager = r.manager(4);

    CHECK(manager.max_recordings() == 4);
    CHECK_FALSE(manager.max_bytes());
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_omitted_limits_read_saved_size_policy
TEST_CASE("TestRecordingRotation::test_omitted_limits_read_saved_size_policy",
          "[test_recording_retention]") {
    Rotation r;
    const Json settings = {{"recording_retention_mode", "size_limit"},
                           {"max_saved_recordings_mb", 64}};

    HistoryManager manager(r.db.database(), r.recordings_dir, settings);

    CHECK_FALSE(manager.max_recordings());
    CHECK(manager.max_bytes() == 64 * MB);
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_recordings_over_limit_previews_without_deleting
TEST_CASE("TestRecordingRotation::test_recordings_over_limit_previews_without_deleting",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt);
    r.touch_recording("20260101_120000", 100);
    r.touch_recording("20260102_120000", 100);
    r.touch_recording("20260103_120000", 100);
    r.reference("first", "recording_20260101_120000.wav");

    const auto preview = manager.recordings_over_limit(std::nullopt, 150);

    Names previewed;
    for (const auto& rec : preview) {
        previewed.push_back(rec.filename);
    }
    CHECK(previewed == Names{"recording_20260102_120000.wav", "recording_20260101_120000.wav"});
    CHECK(list_names(r.recordings_dir).size() == 3);
    CHECK(r.audio_of("first") == "recording_20260101_120000.wav");

    manager.set_retention(std::nullopt, 150);
    CHECK(list_names(r.recordings_dir) == Names{"recording_20260103_120000.wav"});
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_recordings_usage_totals_saved_audio
TEST_CASE("TestRecordingRotation::test_recordings_usage_totals_saved_audio",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt);
    r.touch_recording("20260101_120000", 30);
    r.touch_recording("20260102_120000", 70);

    CHECK(manager.get_recordings_usage() == std::pair<std::int64_t, std::int64_t>{2, 100});
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_recordings_with_unparsable_names_fall_back_to_file_time
TEST_CASE("TestRecordingRotation::test_recordings_with_unparsable_names_fall_back_to_file_time",
          "[test_recording_retention]") {
    Rotation r;
    auto manager = r.manager(std::nullopt);
    r.touch_recording("20260101_120000");
    r.touch_recording("20260101_120000-2");
    r.touch_recording("not-a-stamp");

    std::map<std::string, fv::RecordingInfo> recordings;
    for (auto& rec : manager.get_recordings()) {
        recordings.emplace(rec.filename, rec);
    }

    CHECK(recordings.size() == 3);
    CHECK(recordings.contains("recording_20260101_120000.wav"));
    CHECK(recordings.contains("recording_20260101_120000-2.wav"));
    CHECK(recordings.contains("recording_not-a-stamp.wav"));
    CHECK(recordings["recording_20260101_120000.wav"].timestamp == "2026-01-01T12:00:00");
    CHECK_FALSE(recordings["recording_not-a-stamp.wav"].timestamp.empty());
}

// from
// tests/test_recording_retention.py::TestRecordingRotation::test_same_second_recordings_never_overwrite_each_other
TEST_CASE("TestRecordingRotation::test_same_second_recordings_never_overwrite_each_other",
          "[test_recording_retention]") {
    Rotation r;
    // datetime.now() pinned to 2026-01-03 12:00:00 local time.
    const fv::FakeClock clock(fv::history_test::utc(2026, 1, 3, 20), "America/Los_Angeles");
    auto manager = r.manager(std::nullopt, std::nullopt, &clock);
    const auto first_source = r.db.root() / "first.wav";
    const auto second_source = r.db.root() / "second.wav";
    fv::history_test::write_file(first_source, "first");
    fv::history_test::write_file(second_source, "second");

    const auto first_name = manager.preserve_recording(first_source);
    const auto second_name = manager.preserve_recording(second_source);

    CHECK(first_name == "recording_20260103_120000.wav");
    CHECK(second_name == "recording_20260103_120000-2.wav");
    CHECK(fv::history_test::read_file(r.recordings_dir / *first_name) == "first");
    CHECK(fv::history_test::read_file(r.recordings_dir / *second_name) == "second");
}

// ---- TestHistoryEntryDeletion

// from
// tests/test_recording_retention.py::TestHistoryEntryDeletion::test_delete_entry_keeps_audio_by_default
TEST_CASE("TestHistoryEntryDeletion::test_delete_entry_keeps_audio_by_default",
          "[test_recording_retention]") {
    Rotation r;
    const std::string audio_filename = "recording_20260101_120000.wav";
    const auto audio_path = r.recordings_dir / audio_filename;
    fv::history_test::write_file(audio_path, "RIFF");
    auto manager = r.manager(std::nullopt);
    r.reference("entry-test-id", audio_filename);
    r.reference("other", audio_filename);

    CHECK(manager.delete_entry("entry-test-id"));

    CHECK(std::filesystem::exists(audio_path));
    // clear_history_audio_file was not called: the other entry still names the file.
    CHECK(r.audio_of("other") == audio_filename);
}

// from
// tests/test_recording_retention.py::TestHistoryEntryDeletion::test_delete_entry_can_delete_attached_audio
TEST_CASE("TestHistoryEntryDeletion::test_delete_entry_can_delete_attached_audio",
          "[test_recording_retention]") {
    Rotation r;
    const std::string audio_filename = "recording_20260101_120000.wav";
    const auto audio_path = r.recordings_dir / audio_filename;
    fv::history_test::write_file(audio_path, "RIFF");
    auto manager = r.manager(std::nullopt);
    r.reference("entry-test-id", audio_filename);
    r.reference("other", audio_filename);

    CHECK(manager.delete_entry("entry-test-id", true));

    CHECK_FALSE(std::filesystem::exists(audio_path));
    // clear_history_audio_file(audio_filename) ran.
    CHECK_FALSE(r.audio_of("other"));
}

// ---- TestRecordingRetentionPersistence

namespace {

Json saved_and_loaded(const Json& settings) {
    const fv::testkit::TempDataRoot root;
    {
        fv::SettingsStore store(root.path() / "settings.json");
        store.save_all(settings);
    }
    const fv::SettingsStore reloaded(root.path() / "settings.json");
    return *reloaded.snapshot();
}

} // namespace

// from
// tests/test_recording_retention.py::TestRecordingRetentionPersistence::test_save_and_resolve_custom
TEST_CASE("TestRecordingRetentionPersistence::test_save_and_resolve_custom",
          "[test_recording_retention]") {
    const Json loaded =
        saved_and_loaded({{"recording_retention_mode", "custom"}, {"max_saved_recordings", 15}});
    CHECK(resolve_max_saved_recordings(loaded) == 15);
}

// from
// tests/test_recording_retention.py::TestRecordingRetentionPersistence::test_save_and_resolve_keep_all
TEST_CASE("TestRecordingRetentionPersistence::test_save_and_resolve_keep_all",
          "[test_recording_retention]") {
    const Json loaded =
        saved_and_loaded({{"recording_retention_mode", "keep_all"}, {"max_saved_recordings", 15}});
    CHECK_FALSE(resolve_max_saved_recordings(loaded));
}

// from
// tests/test_recording_retention.py::TestRecordingRetentionPersistence::test_save_and_resolve_size_limit
TEST_CASE("TestRecordingRetentionPersistence::test_save_and_resolve_size_limit",
          "[test_recording_retention]") {
    const Json loaded = saved_and_loaded({{"recording_retention_mode", "size_limit"},
                                          {"max_saved_recordings", 15},
                                          {"max_saved_recordings_mb", 512}});
    CHECK_FALSE(resolve_max_saved_recordings(loaded));
    CHECK(resolve_max_saved_recordings_bytes(loaded) == 512 * MB);
}

// ---- beyond the Python file

TEST_CASE("a size cap past int64 saturates instead of overflowing", "[history_manager]") {
    const Json settings = {{"recording_retention_mode", "size_limit"},
                           {"max_saved_recordings_mb", std::int64_t{1} << 50}};
    CHECK(resolve_max_saved_recordings_bytes(settings) == std::numeric_limits<std::int64_t>::max());
}

TEST_CASE("recording names follow strptime's rules", "[history_manager]") {
    Rotation r;
    auto manager = r.manager(std::nullopt);
    // Values Python's datetime.strptime(stem, "%Y%m%d_%H%M%S") gives these names.
    r.touch_recording("2026111_120000");  // month 11, day 1
    r.touch_recording("20260230_120000"); // no 30 February: the file time instead
    r.touch_recording("20260101_12000");  // second 0
    r.touch_recording("20260101_120060"); // second 60 is refused
    std::map<std::string, std::string> stamps;
    for (const auto& rec : manager.get_recordings()) {
        stamps.emplace(rec.filename, rec.timestamp);
    }
    CHECK(stamps["recording_2026111_120000.wav"] == "2026-11-01T12:00:00");
    CHECK(stamps["recording_20260101_12000.wav"] == "2026-01-01T12:00:00");
    CHECK(stamps["recording_20260230_120000.wav"].starts_with("20"));
    CHECK(stamps["recording_20260230_120000.wav"] != "2026-02-30T12:00:00");
}

TEST_CASE("the usage count is cached for two seconds while the folder is unchanged",
          "[history_manager]") {
    Rotation r;
    fv::FakeClock clock;
    auto manager = r.manager(std::nullopt, std::nullopt, &clock);
    r.touch_recording("20260101_120000", 10);
    CHECK(manager.get_recordings_usage() == std::pair<std::int64_t, std::int64_t>{1, 10});
    // Rewriting a file in place leaves the folder's own modification time alone.
    fv::history_test::write_file(r.recordings_dir / "recording_20260101_120000.wav",
                                 std::string(25, 'x'));
    CHECK(manager.get_recordings_usage() == std::pair<std::int64_t, std::int64_t>{1, 10});
    clock.advance(std::chrono::milliseconds(2000));
    CHECK(manager.get_recordings_usage() == std::pair<std::int64_t, std::int64_t>{1, 25});
}

TEST_CASE("a missing recordings folder counts as empty; a folder that cannot be listed raises",
          "[history_manager]") {
    Rotation r;
    fv::FakeClock clock;
    auto manager = r.manager(std::nullopt, std::nullopt, &clock);
    std::filesystem::remove_all(r.recordings_dir);
    CHECK(manager.get_recordings_usage() == std::pair<std::int64_t, std::int64_t>{0, 0});
    // A file where the folder should be: listing it fails as os.scandir's NotADirectoryError.
    fv::history_test::write_file(r.recordings_dir, "not a folder");
    clock.advance(std::chrono::milliseconds(2000));
    CHECK_THROWS_AS(manager.get_recordings_usage(), std::filesystem::filesystem_error);
}
