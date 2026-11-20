// The meeting intelligence seam: the null provider of a transcript-only build, and the fake.

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "meeting/intelligence.h"
#include "meeting/testing/fake_intelligence.h"

using fv::Json;
using namespace fv::meeting;

TEST_CASE("null intelligence is unavailable and its steps fail", "[meeting][intelligence]") {
    const auto provider = null_intelligence();
    REQUIRE(provider != nullptr);
    CHECK_FALSE(provider->available());
    const RerunContext context{.meeting_id = "m_1"};
    const Json expected = {{"status", "failed"},
                           {"message", "Meeting intelligence is not available yet."}};
    CHECK(provider->polish(context, fv::CancelToken()) == expected);
    CHECK(provider->consolidate(context, fv::CancelToken()) == expected);
    // One shared instance.
    CHECK(null_intelligence() == provider);
}

TEST_CASE("the fake intelligence replays its script", "[meeting][intelligence]") {
    FakeIntelligence fake;
    CHECK(fake.available());
    fake.script_polish({{"status", "failed"}, {"message", "Timed out."}});
    fake.script_consolidate({{"throw", "provider down"}});
    const RerunContext context{.meeting_id = "m_2", .state = {{"meeting_id", "m_2"}}};
    CHECK(fake.polish(context, fv::CancelToken()).at("message") == "Timed out.");
    CHECK(fake.polish(context, fv::CancelToken()).at("status") == "completed");
    CHECK_THROWS_AS(fake.consolidate(context, fv::CancelToken()), std::runtime_error);
    CHECK(fake.polished() == std::vector<std::string>{"m_2", "m_2"});
    CHECK(fake.consolidated() == std::vector<std::string>{"m_2"});
    CHECK(fake.last_context().state.at("meeting_id") == "m_2");
    fake.set_available(false);
    CHECK_FALSE(fake.available());
}
