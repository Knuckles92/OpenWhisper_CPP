// settings_dictionary.engine_line for a connected host
// (tests/test_personalize_s4_settings_page.py).

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "base/json.h"
#include "text/personalize_copy.h"

using fv::Json;
namespace dictionary_page = fv::text::dictionary_page;

TEST_CASE("a connected host says whether hints reach it", "[test_personalize_s4_settings_page]") {
    // from
    // tests/test_personalize_s4_settings_page.py::TestRecognitionAndLearning::test_a_connected_host_says_whether_hints_reach_it
    const Json settings{{"selected_model", "remote"}};
    dictionary_page::RemoteBackend host{"model", "Whisper turbo on devbox", "devbox"};
    const auto line = [&] {
        return dictionary_page::engine_line(settings,
                                            dictionary_page::engine_info(settings, {}, host));
    };
    CHECK(line() == "Your engine (Whisper turbo on devbox) also steers the speech model toward "
                    "your words. They're sent to devbox with your audio.");
    host.support = "after";
    CHECK(line().find("can't be steered") != std::string::npos);
}
