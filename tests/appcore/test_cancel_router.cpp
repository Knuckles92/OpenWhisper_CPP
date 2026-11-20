// Cancel routing of services/application_controller.py (cancel, _cancel_actions, 1129-1148).
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "appcore/cancel_router.h"
#include "base/testing/manual_executor.h"

using fv::CancelRouter;

namespace {

struct Router {
    fv::ManualExecutor ui;
    CancelRouter router;
    std::vector<std::string> said;
    fv::Subscription link = router.nothing_to_cancel.connect(
        ui, [this](const std::string& text) { said.push_back(text); });
};

} // namespace

TEST_CASE("the recording or transcription is canceled before anything else", "[cancel_router]") {
    Router r;
    std::vector<std::string> asked;
    r.router.add(300, [&] {
        asked.emplace_back("actions");
        return true;
    });
    r.router.add(100, [&] {
        asked.emplace_back("transcription");
        return true;
    });
    r.router.add(200, [&] {
        asked.emplace_back("write-up");
        return true;
    });
    CHECK(r.router.cancel());
    CHECK(asked == std::vector<std::string>{"transcription"});
    r.ui.run_all();
    CHECK(r.said.empty());
}

TEST_CASE("each step is asked in order until one cancels something", "[cancel_router]") {
    Router r;
    std::vector<std::string> asked;
    r.router.add(100, [&] {
        asked.emplace_back("transcription");
        return false;
    });
    r.router.add(200, [&] {
        asked.emplace_back("write-up");
        return false;
    });
    r.router.add(300, [&] {
        asked.emplace_back("actions");
        return true;
    });
    r.router.add(300, [&] {
        asked.emplace_back("later actions");
        return true;
    });
    CHECK(r.router.cancel());
    CHECK(asked == std::vector<std::string>{"transcription", "write-up", "actions"});
}

TEST_CASE("with nothing running the status and notice say Nothing to cancel", "[cancel_router]") {
    Router r;
    r.router.add(100, [] { return false; });
    CHECK_FALSE(r.router.cancel());
    r.ui.run_all();
    CHECK(r.said == std::vector<std::string>{"Nothing to cancel"});
    CHECK(fv::kNothingToCancel == "Nothing to cancel");

    CancelRouter empty;
    CHECK_FALSE(empty.cancel());
}
