#include <catch2/catch_test_macros.hpp>

#include "pyport/errors.h"
#include "remote_host/runtime_state.h"

using namespace fv;
using namespace fv::remote_host;

namespace {

const Json kEngine{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"device", "cpu"}};

models::Host cpu_host() {
    return {
        .platform = std::string(models::platform::kWinAmd64),
        .is_installed =
            [](std::string_view id) { return id == models::component_id::kAsrNvidiaCpu; },
    };
}

} // namespace

TEST_CASE("the host describes available Parakeet runtimes and saved device separately") {
    const Json settings{{"local_asr_devices", {{"parakeet", "auto"}}}};
    const auto state = runtime_state(kEngine, settings, cpu_host());
    CHECK(state["selected"]["device"] == "auto");
    CHECK(state["devices"] == Json::array({"auto", "cpu"}));
    REQUIRE(state["dependencies"].size() == 2);
    CHECK(state["dependencies"][0]["ready"] == true);
    CHECK(state["dependencies"][1]["ready"] == false);
    CHECK(state["dependencies"][1]["installable"] == false);
    CHECK(runtime_state(Json{{"family", "api"}}, settings, cpu_host()).empty());
}

TEST_CASE("a remote runtime request can change only an available device or supported language") {
    const Json settings{{"local_asr_devices", {{"example", "auto"}}}};
    const Json patch = validate_runtime(kEngine, settings, cpu_host(), "parakeet", "parakeet-v3",
                                        Json{{"device", "cpu"}, {"language", "fr"}});
    CHECK(patch["local_asr_devices"] == Json{{"example", "auto"}, {"parakeet", "cpu"}});
    CHECK(patch["local_asr_language"] == "fr");
    CHECK_THROWS_AS(validate_runtime(kEngine, settings, cpu_host(), "parakeet", "parakeet-v3",
                                     Json{{"device", "cuda"}}),
                    py::ValueError);
    CHECK_THROWS_AS(validate_runtime(kEngine, settings, cpu_host(), "parakeet", "parakeet-v3",
                                     Json{{"compute_type", "float16"}}),
                    py::ValueError);
    CHECK_THROWS_AS(validate_runtime(kEngine, settings, cpu_host(), "parakeet", "orukeet",
                                     Json{{"language", "fr"}}),
                    py::ValueError);
}
