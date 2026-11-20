// The app's one walkthrough-library gate when the review holding it goes away mid-work: Python's
// hosts refuse to close while busy; here a review destroyed while busy cancels its work and gives
// the gate back once that work has ended.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <thread>

#include <QCoreApplication>

#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "ui_walkthrough/walkthrough_review.h"

TEST_CASE("a review destroyed while busy releases the gate when its work ends", "[widget]") {
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    int acquired = 0;
    int released = 0;
    fv::Task queued;
    fv::ReviewHost host;
    host.acquire = [&acquired]() -> std::optional<QString> {
        ++acquired;
        return std::nullopt;
    };
    host.release = [&released] {
        ++released;
    };
    host.submit = [&queued](fv::Task task) {
        queued = std::move(task);
        return true;
    };
    bool ran = false;
    auto review = std::make_unique<fv::WalkthroughReview>(settings, host);
    review->run([&ran](const fv::CancelToken&) {
        ran = true;
        return fv::ReviewOutcome{};
    });
    REQUIRE(review->isBusy());
    REQUIRE(acquired == 1);

    review.reset();
    // The work has not ended: the library stays held.
    QCoreApplication::processEvents();
    CHECK(released == 0);

    std::thread worker(std::move(queued));
    worker.join();
    QCoreApplication::processEvents();
    CHECK(released == 1);
    // Cancelled before it started: the work never ran.
    CHECK_FALSE(ran);
}

TEST_CASE("a review that finished before it went releases the gate once", "[widget]") {
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    int released = 0;
    fv::Task queued;
    fv::ReviewHost host;
    host.release = [&released] {
        ++released;
    };
    host.submit = [&queued](fv::Task task) {
        queued = std::move(task);
        return true;
    };
    auto review = std::make_unique<fv::WalkthroughReview>(settings, host);
    review->run([](const fv::CancelToken&) { return fv::ReviewOutcome{}; });
    std::thread worker(std::move(queued));
    worker.join();
    QCoreApplication::processEvents();
    REQUIRE_FALSE(review->isBusy());
    review.reset();
    QCoreApplication::processEvents();
    CHECK(released == 1);
}
