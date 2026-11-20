// Quit while the engine setup adopts an OpenWhisper runtime or model (hashing and copying
// gigabytes on the io strand, which Quit waits for): the adoption runs under a token that Quit's
// CancelRoot step cancels, so it stops at once instead of holding Quit.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>

#include "base/cancel.h"
#include "controller_support.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;
using namespace std::chrono_literals;

TEST_CASE("sweep: Quit cancels the adoption the engine setup is running", "[widget]") {
    struct Seen {
        std::atomic<bool> entered{false};
        std::atomic<bool> cancelled{false};
        std::atomic<bool> returned{false};
        std::chrono::steady_clock::time_point at;
    };
    auto seen = std::make_shared<Seen>();
    RigOptions options;
    // An adoption that takes as long as a large copy: it ends only when cancelled, or after 10 s.
    options.adoptMissing = [seen](const fv::Json&, const fv::Paths&,
                                  const fv::CancelToken& cancel) {
        seen->entered = true;
        seen->cancelled = cancel.wait_for(10s);
        seen->at = std::chrono::steady_clock::now();
        seen->returned = true;
        return fv::models::AdoptionReport{};
    };
    ControllerRig rig(options);
    rig.mainWindowShown();
    REQUIRE(waitUntil([&] { return seen->entered.load(); }));

    const auto quitAt = std::chrono::steady_clock::now();
    rig.plan().run();
    REQUIRE(waitUntil([&] { return seen->returned.load(); }, 3000ms));
    CHECK(seen->cancelled);
    CHECK(seen->at - quitAt < 2s);
}
