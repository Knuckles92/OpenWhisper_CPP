// services/cleanup_profiles.py's cleanup_may_run over the rewrite shortcuts
// (tests/test_personalize_coordinator.py).

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

#include "base/json.h"
#include "text/cleanup_profiles.h"

using fv::Json;

TEST_CASE("rewrite shortcuts warm the cleanup sdk", "[test_personalize_coordinator]") {
    // from tests/test_personalize_coordinator.py::test_rewrite_shortcuts_warm_the_cleanup_sdk
    // Command Mode and transforms always use the cleanup model, so their shortcuts count.
    const std::vector<std::pair<Json, bool>> rows{
        {Json::object(), false},
        {Json{{"hotkeys", {{"command_mode", "ctrl+alt+k"}}}}, true},
        {Json{{"hotkeys", {{"command_mode", ""}}}}, false},
        {Json::parse(R"({"text_transforms": [{"id": "p", "name": "Polish",
            "instruction": "Polish it.", "hotkey": "ctrl+alt+p"}]})"),
         true},
        {Json::parse(R"({"text_transforms": [{"id": "p", "name": "Polish",
            "instruction": "Polish it.", "hotkey": ""}]})"),
         false},
    };
    for (auto [settings, expected] : rows) {
        settings["transcript_cleanup_profiles"] = Json::array();
        INFO(settings.dump());
        CHECK(fv::text::cleanup_may_run(settings) == expected);
    }
}
