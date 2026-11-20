// test_typed_settings.py::test_the_topic_shift_switch_shows_what_the_meeting_uses: the Fast
// judgments page and the meeting read typesafe_topic_shift_enabled the same way.

#include <catch2/catch_test_macros.hpp>

#include "base/json.h"
#include "meeting_intel/intel_settings.h"
#include "settings/keys.h"
#include "settings/resolve.h"

TEST_CASE("test_the_topic_shift_switch_shows_what_the_meeting_uses", "[unit]") {
    const auto& key = fv::keys::typesafe_topic_shift_enabled;
    const std::string name(key.key());
    for (const bool stored : {true, false}) {
        const fv::Json settings{{name, stored}};
        CHECK(key.read(settings) == stored);
        CHECK(fv::resolve_bool_setting(name, settings) == stored);
    }
    CHECK(key.read(fv::Json::object()));
    CHECK(fv::resolve_bool_setting(name, fv::Json::object()));
    for (const fv::Json& junk : {fv::Json(nullptr), fv::Json("yes"), fv::Json(1), fv::Json(0),
                                 fv::Json::array(), fv::Json::object()}) {
        INFO(junk.dump());
        const fv::Json settings{{"typesafe_enabled", true}, {name, junk}};
        CHECK(key.read(settings));
        CHECK(fv::meeting_intel::resolve_typesafe_topic_shift_enabled(settings));
    }
}
