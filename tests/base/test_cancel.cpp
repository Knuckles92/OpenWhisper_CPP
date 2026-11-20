// CancelSource and CancelToken (architecture 3.2, rule 9 of section 4).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#include "base/cancel.h"

using namespace std::chrono_literals;
using fv::CancelSource;
using fv::CancelToken;

TEST_CASE("a default token is never cancelled and its wait is a plain sleep") {
    const CancelToken token;
    CHECK_FALSE(token.cancelled());
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(token.wait_for(20ms));
    CHECK(std::chrono::steady_clock::now() - start >= 20ms);
    bool ran = false;
    auto subscription = token.on_cancel([&] { ran = true; });
    CHECK_FALSE(subscription.connected());
    CHECK_FALSE(ran);
}

TEST_CASE("wait_for returns early, true, when another thread cancels") {
    CancelSource source;
    const CancelToken token = source.token();
    std::thread canceller([&] {
        std::this_thread::sleep_for(20ms);
        source.cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    CHECK(token.wait_for(10s));
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    canceller.join();
    CHECK(token.cancelled());
    CHECK(source.cancelled());
    CHECK(token.wait_for(10s)); // already cancelled: at once
}

TEST_CASE("wait_for times out, false, when nobody cancels") {
    CancelSource source;
    CHECK_FALSE(source.token().wait_for(10ms));
}

TEST_CASE("on_cancel fires once, on the cancelling thread, in registration order") {
    CancelSource source;
    std::vector<int> order;
    std::thread::id ran_on;
    auto first = source.token().on_cancel([&] {
        order.push_back(1);
        ran_on = std::this_thread::get_id();
    });
    auto second = source.token().on_cancel([&] { order.push_back(2); });
    CHECK(first.connected());

    std::thread canceller([&] { source.cancel(); });
    const auto canceller_id = canceller.get_id();
    canceller.join();
    source.cancel(); // a second cancel runs nothing

    CHECK(order == std::vector<int>{1, 2});
    CHECK(ran_on == canceller_id);
    CHECK_FALSE(first.connected());
}

TEST_CASE("on_cancel on a cancelled token runs at once") {
    CancelSource source;
    source.cancel();
    bool ran = false;
    auto subscription = source.token().on_cancel([&] { ran = true; });
    CHECK(ran);
    CHECK_FALSE(subscription.connected());
}

TEST_CASE("a disconnected cancel callback never runs") {
    CancelSource source;
    int calls = 0;
    {
        auto subscription = source.token().on_cancel([&] { ++calls; });
    }
    auto kept = source.token().on_cancel([&] { calls += 10; });
    kept.disconnect();
    source.cancel();
    CHECK(calls == 0);
}

TEST_CASE("a throwing cancel callback does not stop the others") {
    CancelSource source;
    bool second = false;
    auto a = source.token().on_cancel([] { throw std::runtime_error("boom"); });
    auto b = source.token().on_cancel([&] { second = true; });
    source.cancel();
    CHECK(second);
}

TEST_CASE("a child source is cancelled with its parent, and alone without it") {
    CancelSource root;
    CancelSource child(root.token());
    CancelSource other_child(root.token());
    other_child.cancel();
    CHECK(other_child.cancelled());
    CHECK_FALSE(root.cancelled());
    CHECK_FALSE(child.cancelled());

    root.cancel();
    CHECK(child.cancelled());

    CancelSource late(root.token()); // parent already cancelled
    CHECK(late.cancelled());
}

TEST_CASE("copies of a source share its state") {
    CancelSource source;
    CancelSource copy = source;
    copy.cancel();
    CHECK(source.cancelled());
    CHECK(source.token().cancelled());
}
