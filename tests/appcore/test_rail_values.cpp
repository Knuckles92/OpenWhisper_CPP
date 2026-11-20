// The App and Recording rail values and the retention decisions of
// ui_qt/dialogs/settings_dialog.py, replayed from the golden vectors of
// ui_qt.dialogs.settings_dialog.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "appcore/rail_values.h"
#include "appcore/retention_policy.h"
#include "base/json.h"
#include "golden/golden.h"
#include "hotkeys/grammar.h"

using fv::Json;
using fv::appcore::RetentionLimits;

namespace {

constexpr const char* kModule = "ui_qt.dialogs.settings_dialog";

std::string windows_display(std::string_view hotkey) {
    return fv::hotkeys::format_hotkey_display(hotkey, fv::hotkeys::Vocabulary::Windows);
}

Json limits_json(const RetentionLimits& limits) {
    return Json::array({limits.max_recordings ? Json(*limits.max_recordings) : Json(nullptr),
                        limits.max_bytes ? Json(*limits.max_bytes) : Json(nullptr)});
}

} // namespace

TEST_CASE("rail values replay the golden vectors", "[golden][rail_values]") {
    fv::golden::for_each(kModule, "_general_rail_value", [](const fv::golden::Case& c) {
        CHECK(fv::appcore::general_rail_value(c.arg<Json>(0)) == c.result<std::string>());
    });
    // Generated on Windows, where no desktop is Omarchy's.
    fv::golden::for_each(kModule, "appearance_rail_value", [](const fv::golden::Case& c) {
        CHECK(fv::appcore::appearance_rail_value(c.arg<Json>(0), false) == c.result<std::string>());
    });
    fv::golden::for_each(kModule, "developer_rail_value", [](const fv::golden::Case& c) {
        CHECK(fv::appcore::developer_rail_value(c.arg<Json>(0)) == c.result<std::string>());
    });
    fv::golden::for_each(
        kModule, "SettingsDialog._hotkey_rail_value", [](const fv::golden::Case& c) {
            const Json expected = c.result<Json>();
            CHECK(fv::appcore::hotkeys_rail_value(c.arg<Json>(0), windows_display) ==
                  expected.at("value").get<std::string>());
        });
}

TEST_CASE("an unsaved theme on an Omarchy desktop reads as Omarchy's", "[rail_values]") {
    CHECK(fv::appcore::appearance_rail_value(Json::object(), true) == "Omarchy desktop · Bars");
    CHECK(fv::appcore::appearance_rail_value(Json{{"ui_theme", "light"}}, true) == "Light · Bars");
    CHECK(fv::appcore::hotkeys_rail_value(Json::object(), windows_display) == "Not set");
    CHECK(fv::appcore::hotkeys_rail_value(Json{{"record_toggle", 5}}, windows_display) ==
          "Not set");
}

TEST_CASE("the retention commit replays the golden vectors", "[golden][retention_policy]") {
    fv::golden::for_each(
        kModule, "SettingsDialog._commit_retention", [](const fv::golden::Case& c) {
            const Json saved = c.arg<Json>(0);
            const Json mode = c.arg<Json>(1);
            const Json count = c.arg<Json>(2);
            const Json mb = c.arg<Json>(3);
            const bool answer = c.arg<bool>(4);
            const Json expected = c.result<Json>();

            const RetentionLimits before = fv::appcore::retention_limits(saved);
            const RetentionLimits after = fv::appcore::retention_limits(mode, count, mb);
            const bool changed = fv::appcore::retention_changed(before, after);
            if (!changed) {
                CHECK(expected.at("confirm").is_null());
                CHECK(expected.at("persisted").is_null());
                CHECK(expected.at("reloaded").is_null());
                CHECK_FALSE(expected.at("applied").get<bool>());
                return;
            }
            CHECK(fv::golden::equal(expected.at("confirm"), limits_json(after)));
            if (answer) {
                const Json edits = fv::appcore::retention_edits(mode, count, mb);
                CHECK(fv::golden::equal(expected.at("persisted"), edits));
                CHECK(edits.dump() == expected.at("persisted").dump());
                CHECK(expected.at("applied").get<bool>());
            } else {
                CHECK(expected.at("persisted").is_null());
                CHECK(fv::golden::equal(expected.at("reloaded"), saved));
            }
        });
}

TEST_CASE("the retention confirmation replays the golden vectors", "[golden][retention_policy]") {
    fv::golden::for_each(
        kModule, "SettingsDialog._confirm_recording_removal", [](const fv::golden::Case& c) {
            const RetentionLimits limits{c.arg<std::optional<std::int64_t>>(0),
                                         c.arg<std::optional<std::int64_t>>(1)};
            const auto sizes = c.arg<std::vector<std::int64_t>>(2);
            const bool answer = c.arg<bool>(3);
            const Json expected = c.result<Json>();
            // Limits that apply no cap delete nothing: no preview, no question.
            const bool previews = fv::appcore::shrinks(RetentionLimits{}, limits);
            CHECK(previews == !expected.at("previewed").empty());
            if (previews) {
                CHECK(fv::golden::equal(expected.at("previewed")[0], limits_json(limits)));
            }
            if (!previews || sizes.empty()) {
                CHECK(expected.at("asked").is_null());
                CHECK(expected.at("result").get<bool>());
                return;
            }
            const auto count = static_cast<std::int64_t>(sizes.size());
            const std::int64_t total = std::accumulate(sizes.begin(), sizes.end(), std::int64_t{0});
            const Json asked = expected.at("asked");
            CHECK(fv::appcore::delete_confirm_title(count) == asked.at("title").get<std::string>());
            CHECK(fv::appcore::delete_confirm_text(count, total) ==
                  asked.at("text").get<std::string>());
            CHECK(asked.at("default").get<std::string>() == "Cancel");
            CHECK(expected.at("result").get<bool>() == answer);
        });
    fv::golden::for_each(
        kModule, "SettingsDialog._apply_recordings_usage", [](const fv::golden::Case& c) {
            CHECK(fv::appcore::recordings_usage_text(
                      c.arg<std::int64_t>(0), c.arg<std::int64_t>(1)) == c.result<std::string>());
        });
}

TEST_CASE("unchanged limits save nothing and uncapped limits never ask", "[retention_policy]") {
    const RetentionLimits keep_all{};
    const RetentionLimits twenty{20, std::nullopt};
    const RetentionLimits ten{10, std::nullopt};
    CHECK_FALSE(fv::appcore::retention_changed(twenty, twenty));
    CHECK_FALSE(fv::appcore::shrinks(twenty, twenty));
    CHECK(fv::appcore::shrinks(twenty, ten));
    CHECK(fv::appcore::shrinks(keep_all, twenty));
    CHECK(fv::appcore::retention_changed(twenty, keep_all));
    CHECK_FALSE(fv::appcore::shrinks(twenty, keep_all));
    CHECK(fv::appcore::retention_limits(Json("size_limit"), Json(20), Json(2)) ==
          RetentionLimits{std::nullopt, 2 * 1024 * 1024});
    CHECK(fv::appcore::retention_limits(Json("custom"), Json(0), Json(1024)) ==
          RetentionLimits{1, std::nullopt});
    CHECK(fv::appcore::delete_confirm_title(1) == "Delete 1 saved recording?");
    CHECK(fv::appcore::retention_check_failed("disk gone") ==
          "Couldn't check saved recordings: disk gone");
}
