// Golden vectors of services/runtime/meeting.py (tests/golden/services.runtime.meeting.json):
// redact_meeting_url, _with_history_target, _ended_message, _recovered_follow_up_step and
// _require_dashboard_address.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>

#include "golden/golden.h"
#include "meeting/runtime.h"
#include "meeting/runtime_messages.h"
#include "runtime_support.h"

using namespace fv::meeting;
using fv::Json;

TEST_CASE("redact_meeting_url matches Python", "[golden][services.runtime.meeting]") {
    fv::golden::for_each("services.runtime.meeting", "redact_meeting_url",
                         [](const fv::golden::Case& c) {
                             const auto url = c.arg<std::optional<std::string>>(0);
                             CHECK(redact_meeting_url(url.value_or("")) == c.result<std::string>());
                         });
}

TEST_CASE("_with_history_target matches Python", "[golden][services.runtime.meeting]") {
    fv::golden::for_each(
        "services.runtime.meeting", "_with_history_target", [](const fv::golden::Case& c) {
            const std::string view = c.arg_count() > 2 ? c.arg<std::string>(2) : std::string();
            CHECK(with_history_target(c.arg<std::string>(0), c.arg<std::string>(1), view) ==
                  c.result<std::string>());
        });
}

TEST_CASE("_ended_message matches Python", "[golden][services.runtime.meeting]") {
    fv::golden::for_each("services.runtime.meeting", "_ended_message",
                         [](const fv::golden::Case& c) {
                             CHECK(ended_message(c.arg<Json>(0), c.arg<std::string>(1),
                                                 c.arg<bool>(2)) == c.result<std::string>());
                         });
}

TEST_CASE("_recovered_follow_up_step matches Python", "[golden][services.runtime.meeting]") {
    fv::golden::for_each("services.runtime.meeting", "_recovered_follow_up_step",
                         [](const fv::golden::Case& c) {
                             CHECK(recovered_follow_up_step(c.arg<Json>(0), c.arg<Json>(1)) ==
                                   c.result<std::optional<std::string>>());
                         });
}

TEST_CASE("_require_dashboard_address matches Python", "[golden][services.runtime.meeting]") {
    fv::golden::for_each(
        "services.runtime.meeting", "_require_dashboard_address", [](const fv::golden::Case& c) {
            runtime_test::FakeEngine engine;
            if (c.raises()) {
                CHECK(c.raises_type() == "RuntimeError");
                try {
                    require_dashboard_address(engine, c.arg<Json>(0));
                    FAIL("no error");
                } catch (const std::runtime_error& error) {
                    CHECK(std::string(error.what()) == messages::kDashboardNoAddress);
                }
                CHECK(engine.abort_calls == 1);
                return;
            }
            require_dashboard_address(engine, c.arg<Json>(0));
            CHECK((engine.abort_calls == 1) == c.result<Json>()["aborted"].get<bool>());
        });
}
