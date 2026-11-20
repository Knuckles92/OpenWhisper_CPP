// The refusal gates of services/application_controller.py, replayed from the golden vectors of
// services.application_controller, plus the busy reasons of ui_qt/ui_controller.py.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "appcore/activity.h"
#include "base/json.h"
#include "golden/golden.h"

using fv::ActivitySnapshot;
using fv::Json;

namespace {

bool flag(const Json& flags, const char* name) {
    return flags.at(name).get<bool>();
}

// The gate's refusal as the vector records it: {"refused": bool, "statuses": [text]}.
void check_refusal(const std::optional<std::string>& refusal, const Json& expected) {
    CHECK(refusal.has_value() == expected.at("refused").get<bool>());
    const auto& statuses = expected.at("statuses");
    if (refusal) {
        REQUIRE(statuses.size() == 1);
        CHECK(*refusal == statuses[0].get<std::string>());
    } else {
        CHECK(statuses.empty());
    }
}

void check_rebranded(const std::optional<std::string>& actual,
                     const std::optional<std::string>& expected) {
    REQUIRE(actual.has_value() == expected.has_value());
    if (expected) {
        CHECK(fv::golden::equal_rebranded(*expected, *actual));
    }
}

} // namespace

TEST_CASE("dictation refusal replays the golden vectors", "[golden][activity]") {
    fv::golden::for_each("services.application_controller", "_refuse_dictation_during_meeting",
                         [](const fv::golden::Case& c) {
                             const Json flags = c.arg<Json>(0);
                             ActivitySnapshot activity;
                             activity.backup_in_progress = flag(flags, "backup_in_progress");
                             activity.meeting_setup_open = flag(flags, "meeting_setup_open");
                             activity.meeting_active = flag(flags, "meeting_active");
                             check_refusal(fv::refuse_dictation(activity), c.result<Json>());
                         });
}

TEST_CASE("engine load refusal replays the golden vectors", "[golden][activity]") {
    fv::golden::for_each("services.application_controller", "_refuse_engine_load_during_meeting",
                         [](const fv::golden::Case& c) {
                             ActivitySnapshot activity;
                             activity.meeting_active = flag(c.arg<Json>(0), "meeting_active");
                             check_refusal(fv::refuse_engine_load(activity), c.result<Json>());
                         });
}

TEST_CASE("backup refusal replays the golden vectors", "[golden][activity]") {
    fv::golden::for_each(
        "services.application_controller", "_acquire_backup", [](const fv::golden::Case& c) {
            const Json flags = c.arg<Json>(0);
            ActivitySnapshot activity;
            activity.shutting_down = flag(flags, "shutting_down");
            activity.startup_ready = flag(flags, "startup_ready");
            activity.recovery_running = flag(flags, "recovery_running");
            activity.library_change = flag(flags, "library_change");
            activity.write_up_running = flag(flags, "write_up_running");
            activity.backup_in_progress = flag(flags, "backup_in_progress");
            activity.recording = flag(flags, "recording");
            activity.transcribing =
                flag(flags, "job_active") || flag(flags, "backend_transcribing");
            activity.meeting_active = flag(flags, "meeting_active");
            activity.meeting_finishing =
                flag(flags, "meeting_finalizing") || flag(flags, "meeting_background_engines") ||
                flag(flags, "meeting_archive_starting") || flag(flags, "meeting_retry");
            activity.rule_dictation_busy = flag(flags, "rule_dictation_busy");
            check_rebranded(fv::backup_refusal(activity), c.result<std::optional<std::string>>());
        });
}

TEST_CASE("walkthrough library refusal replays the golden vectors", "[golden][activity]") {
    fv::golden::for_each("services.application_controller", "_acquire_walkthrough_library",
                         [](const fv::golden::Case& c) {
                             const Json flags = c.arg<Json>(0);
                             ActivitySnapshot activity;
                             activity.shutting_down = flag(flags, "shutting_down");
                             activity.startup_ready = flag(flags, "startup_ready");
                             activity.recovery_running = flag(flags, "recovery_running");
                             activity.backup_in_progress = flag(flags, "backup_in_progress");
                             activity.library_change = flag(flags, "library_change");
                             activity.write_up_running = flag(flags, "write_up_running");
                             activity.walkthrough_job =
                                 flag(flags, "job_active") && flag(flags, "job_is_walkthrough");
                             check_rebranded(fv::walkthrough_library_refusal(activity),
                                             c.result<std::optional<std::string>>());
                         });
}

TEST_CASE("dictation refusals name the backup first, then setup, then the meeting", "[activity]") {
    ActivitySnapshot activity;
    CHECK_FALSE(fv::refuse_dictation(activity));
    activity.meeting_active = true;
    CHECK(fv::refuse_dictation(activity) ==
          "Meeting Mode is active — end the meeting to use dictation");
    activity.meeting_setup_open = true;
    CHECK(fv::refuse_dictation(activity) == "Close Meeting Mode setup before using Quick Record.");
    activity.backup_in_progress = true;
    CHECK(fv::refuse_dictation(activity) == "Wait for backup or restore preparation to finish");
}

TEST_CASE("the product name in the startup refusals comes from the identity", "[activity]") {
    ActivitySnapshot activity; // startup not ready
    CHECK(fv::backup_refusal(activity) ==
          "Wait for OpenWhisper_CPP to finish starting before backing up or restoring.");
    CHECK(fv::walkthrough_library_refusal(activity) ==
          "Wait for OpenWhisper_CPP to finish starting.");
}

// from ui_qt/ui_controller.py _update_work_is_busy (2616-2625): the confirmations name the first
TEST_CASE("busy reasons come in the order the confirmations name them", "[activity]") {
    ActivitySnapshot activity;
    CHECK(fv::busy_reasons(activity).empty());
    activity.component_installing = true;
    activity.transcribing = true;
    activity.meeting_active = true;
    activity.recording = true;
    CHECK(fv::busy_reasons(activity) == std::vector<std::string_view>{"a recording", "a meeting",
                                                                      "transcription",
                                                                      "a component install"});
    activity.recording = false;
    activity.meeting_active = false;
    CHECK(fv::busy_reasons(activity).front() == "transcription");
    // Neither a backup nor a walkthrough edit is work the quit confirmation names.
    ActivitySnapshot other;
    other.backup_in_progress = true;
    other.library_change = true;
    other.write_up_running = true;
    CHECK(fv::busy_reasons(other).empty());
}
