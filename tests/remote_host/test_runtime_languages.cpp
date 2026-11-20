// services/remote_asr/runtime.py: the host's language choices are checked before anything is
// saved (tests/test_local_asr_languages.py).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "pyport/errors.h"
#include "remote_host/runtime_state.h"
#include "speech/languages.h"

using namespace fv;
using namespace fv::remote_host;

TEST_CASE("remote choices validate language before persisting", "[test_local_asr_languages]") {
    // from
    // tests/test_local_asr_languages.py::test_remote_choices_validate_language_before_persisting
    // Parakeet only: the other families of the Python loop are engines FioriVoce does not have.
    const models::Host host{
        .platform = std::string(models::platform::kWinAmd64),
        .is_installed =
            [](std::string_view id) { return id == models::component_id::kAsrNvidiaCpu; },
    };
    const Json engine{{"family", "parakeet"}, {"model", "parakeet-v3"}};
    const Json settings{{"local_asr_language", "zh"}};
    const Json before = settings;

    const auto state = runtime_state(engine, settings, host);
    Json expected = Json::array();
    for (const auto code : speech::language_choices("parakeet")) {
        expected.push_back(code);
    }
    CHECK(state["languages"] == expected);
    CHECK(std::ranges::find(state["languages"], state["selected"]["language"]) !=
          state["languages"].end());

    for (const auto& code : state["languages"]) {
        const auto updates = validate_runtime(engine, settings, host, "parakeet", "parakeet-v3",
                                              Json{{"device", "cpu"}, {"language", code}});
        CHECK(updates["local_asr_language"] == code);
    }
    for (const std::string code : {"xx", "zh", "ru"}) {
        if (std::ranges::find(state["languages"], Json(code)) != state["languages"].end()) {
            continue;
        }
        INFO(code);
        try {
            validate_runtime(engine, settings, host, "parakeet", "parakeet-v3",
                             Json{{"device", "cpu"}, {"language", code}});
            FAIL("an unsupported language was accepted");
        } catch (const py::ValueError& error) {
            CHECK(std::string(error.what()).find("language") != std::string::npos);
        }
    }
    CHECK(settings == before);
}
