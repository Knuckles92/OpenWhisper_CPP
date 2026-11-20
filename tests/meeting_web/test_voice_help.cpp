// meeting_web/voice_help.h with OpenWhisper_CPP's identity: the default wake names are the
// product's own, and a wake name of either identity is shown as that product (lead amendments 19,
// 25).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "base/identity.h"
#include "meeting_web/voice_help.h"

using fv::Json;
namespace mw = fv::meeting_web;

TEST_CASE("the default wake names are the product's", "[voice_help]") {
    const auto names = mw::resolve_voice_command_names(Json::object());
    REQUIRE(names.size() == 5);
    CHECK(names[3] == fv::identity().wake_name);
    CHECK(names[4] == fv::identity().wake_name_spaced);

    const Json guide = mw::voice_command_guide(Json::object());
    CHECK(guide.at("names") ==
          Json::array({"Assistant", "Note taker", std::string(fv::identity().product)}));
    CHECK(guide.at("primary") == "Assistant");
    CHECK(guide.at("examples").size() == 6);
    CHECK(guide.at("examples")[0] ==
          Json{{"label", "Take a note"}, {"phrase", "Assistant, note that the launch is Friday."}});
}

TEST_CASE("a stored wake name of either identity shows as that product", "[voice_help]") {
    const auto legacy = fv::legacy_identity();
    const auto current = fv::identity();
    const Json settings = {
        {"typesafe_voice_command_names",
         Json::array({std::string(current.wake_name_spaced), std::string(legacy.wake_name),
                      std::string(current.wake_name), std::string(legacy.wake_name_spaced),
                      "Scribe"})}};
    const Json guide = mw::voice_command_guide(settings);
    CHECK(guide.at("names") ==
          Json::array({std::string(current.product), std::string(legacy.product), "Scribe"}));
    CHECK(guide.at("primary") == std::string(current.product));
}

TEST_CASE("an unusable setting falls back to the defaults", "[voice_help]") {
    for (const Json& stored : {Json(), Json::array(), Json::array({"  ", 5}), Json("x")}) {
        const Json settings = {{"typesafe_voice_command_names", stored}};
        CHECK(mw::resolve_voice_command_names(settings) == mw::default_voice_command_names());
    }
}
