// PreviewEvents from services/application_controller.py, replayed from the golden vectors of
// services.application_controller with a fake clock and timer on both sides.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "appcore/preview_throttle.h"
#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "golden/golden.h"

using fv::Json;
using fv::appcore::PreviewThrottle;
using std::chrono::milliseconds;

namespace {

// A UI executor that writes each timer into the trace as ["timer", <now ms>, <ms>].
class TimerTrace final : public fv::Executor {
public:
    TimerTrace(fv::ManualExecutor& inner, fv::FakeClock& clock, Json& trace)
        : inner_(inner), clock_(clock), trace_(trace) {}
    void post(fv::Task task) override { inner_.post(std::move(task)); }
    fv::TimerHandle post_after(milliseconds delay, fv::Task task) override {
        trace_.push_back(Json::array({"timer", now_ms(clock_), delay.count()}));
        return inner_.post_after(delay, std::move(task));
    }
    bool is_current() const override { return inner_.is_current(); }

    static long long now_ms(const fv::FakeClock& clock) {
        return std::chrono::duration_cast<milliseconds>(clock.steady().time_since_epoch()).count();
    }

private:
    fv::ManualExecutor& inner_;
    fv::FakeClock& clock_;
    Json& trace_;
};

struct Throttle {
    fv::FakeClock clock;
    fv::ManualExecutor executor{clock};
    Json trace = Json::array();
    TimerTrace ui{executor, clock, trace};
    PreviewThrottle throttle{
        clock, ui, [this](std::string job, std::string text, bool final) {
            trace.push_back(Json::array({"publish", TimerTrace::now_ms(clock), job, text, final}));
        }};

    void at(long long t) {
        const auto now = TimerTrace::now_ms(clock);
        if (t > now) {
            executor.advance(milliseconds(t - now));
        }
    }
};

} // namespace

TEST_CASE("preview offers replay the golden vectors", "[golden][preview_throttle]") {
    fv::golden::for_each(
        "services.application_controller", "PreviewEvents.offer", [](const fv::golden::Case& c) {
            Throttle t;
            for (const auto& offer : c.arg<Json>(0)) {
                t.at(offer[0].get<long long>());
                t.throttle.offer(offer[1].get<std::string>(), offer[2].get<std::string>(),
                                 offer[3].get<bool>());
            }
            // Whatever is still held goes out when its timer comes due.
            t.executor.advance(milliseconds(1000));
            const Json expected = c.result<Json>();
            INFO("expected " << expected.dump());
            INFO("actual   " << t.trace.dump());
            CHECK(fv::golden::equal(expected, t.trace));
        });
}

TEST_CASE("text that arrives too soon is held and only the newest is sent", "[preview_throttle]") {
    Throttle t;
    t.throttle.offer("j1", "a", false);
    t.at(100);
    t.throttle.offer("j1", "ab", false);
    t.at(120);
    t.throttle.offer("j1", "abc", false);
    t.at(1000);
    CHECK(t.trace == Json::parse(R"([["publish", 0, "j1", "a", false], ["timer", 100, 151],
                                      ["publish", 251, "j1", "abc", false]])"));
}

TEST_CASE("a final preview goes out at once and drops the held text", "[preview_throttle]") {
    Throttle t;
    t.throttle.offer("j1", "a", false);
    t.at(100);
    t.throttle.offer("j1", "ab", false);
    t.at(150);
    t.throttle.offer("j1", "abc", true);
    t.at(1000);
    CHECK(t.trace == Json::parse(R"([["publish", 0, "j1", "a", false], ["timer", 100, 151],
                                      ["publish", 150, "j1", "abc", true]])"));
    CHECK(PreviewThrottle::kIntervalS == 0.25);
}

TEST_CASE("a destroyed throttle's timer publishes nothing", "[preview_throttle]") {
    fv::FakeClock clock;
    fv::ManualExecutor executor{clock};
    int published = 0;
    {
        PreviewThrottle throttle(clock, executor, [&](auto, auto, bool) { ++published; });
        throttle.offer("j1", "a", false);
        clock.advance(milliseconds(10));
        throttle.offer("j1", "ab", false);
    }
    executor.advance(milliseconds(1000));
    CHECK(published == 1);
}
