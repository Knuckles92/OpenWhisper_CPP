// The live engine's titles (tests/test_meeting_title_consistency.py): a dashboard title and a
// later rename reach the meeting row and the saved state the same way.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "engine_support.h"
#include "history/titles.h"
#include "meeting_state/stored.h"
#include "pyport/json.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;

namespace {

void check_saved_title(EngineRig& rig, const std::string& meeting_id, const std::string& title) {
    const Json row = *rig.sql.repo->get_meeting(meeting_id);
    CHECK(row.at("title") == title);
    CHECK(fv::py::loads(row.at("state_json").get<std::string>()).at("title") == title);
    CHECK(fv::meeting_state::open_store(rig.sql.repo, row, false, Json::object())
              ->snapshot()
              .at("title") == title);
}

} // namespace

TEST_CASE("test_live_engine_title_and_runtime_updates_follow_the_same_contract",
          "[test_meeting_title_consistency]") {
    // from tests/test_meeting_title_consistency.py::
    // test_live_engine_title_and_runtime_updates_follow_the_same_contract (over a started
    // engine's own meeting rather than a store opened by hand)
    EngineRig rig;
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = false;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const std::string id = engine.meeting_id();
    std::string title;
    for (std::size_t i = 0; i < fv::MAX_TITLE_LENGTH; ++i) {
        title += "\xF0\x9F\x93\x9D"; // U+1F4DD MEMO
    }
    REQUIRE(
        engine.apply_client_action("host", std::nullopt, Json{{"op", "set_title"}, {"text", title}})
            .at(0)
            .ok);
    check_saved_title(rig, id, title);
    rig.sql.repo->rename_meeting(id, "External rename");
    CHECK(engine.store()->update_runtime_fields(Json{{"status", "ended"}}));
    CHECK(engine.store()->snapshot().at("title") == "External rename");
    check_saved_title(rig, id, "External rename");
}
