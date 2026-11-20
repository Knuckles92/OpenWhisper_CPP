// end() never makes the last reference wait for the writer: a shared session stays alive until
// its queued PNG writes are done, and is destroyed on the writer thread (Python's end() never
// waits).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "support.h"
#include "walkthrough/session.h"
#include "walkthrough/testing/fake_shot_image.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using namespace std::chrono_literals;

namespace {

// A screenshot whose save waits until the test opens the gate.
class GatedImage final : public ShotImage {
public:
    int width() const override { return 4; }
    int height() const override { return 3; }
    double device_pixel_ratio() const override { return 1.0; }
    void save_png(const std::filesystem::path& path) const override {
        {
            std::unique_lock lock(mutex_);
            started_ = true;
            changed_.notify_all();
            changed_.wait(lock, [&] { return open_; });
        }
        inner_.save_png(path);
    }
    std::vector<std::uint8_t> png_bytes() const override { return inner_.png_bytes(); }

    bool wait_started() const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 10s, [&] { return started_; });
    }
    void open() const {
        std::lock_guard lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    FakeShotImage inner_{4, 3, 0x336699};
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    mutable bool started_ = false;
    mutable bool open_ = false;
};

// Opens the gate however the test leaves, so the writer thread never waits for ever.
struct Opener {
    const GatedImage& image;
    ~Opener() { image.open(); }
};

bool wait_for(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

} // namespace

TEST_CASE("dropping the last reference after end does not wait for a screenshot being written",
          "[walkthrough]") {
    const TempFolder tmp;
    auto gated = std::make_shared<GatedImage>();
    Opener opener{*gated};
    WalkthroughSession::Options options;
    options.folder = tmp / "capture";
    auto session = std::make_shared<WalkthroughSession>(std::move(options));
    std::weak_ptr<WalkthroughSession> watch = session;
    const auto first = session->add_shot(gated, 1.0);
    const auto second = session->add_shot(std::make_shared<FakeShotImage>(4, 3, 0x112233), 2.0);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(gated->wait_started());

    session->end();
    // Dropped on another thread, so a wait shows as a timeout here rather than a hung test.
    std::mutex mutex;
    std::condition_variable dropped_cv;
    bool dropped = false;
    std::thread dropper([&] {
        session.reset();
        std::lock_guard lock(mutex);
        dropped = true;
        dropped_cv.notify_all();
    });
    {
        std::unique_lock lock(mutex);
        CHECK(dropped_cv.wait_for(lock, 1s, [&] { return dropped; }));
    }
    if (!dropped) {
        gated->open();
    }
    dropper.join();
    // The writer keeps it alive until both writes are done.
    CHECK_FALSE(watch.expired());

    gated->open();
    CHECK(wait_for(second->path));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!watch.expired() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(watch.expired());
    CHECK(std::filesystem::exists(first->path));
}

TEST_CASE("a session owned alone still waits for its writer when destroyed", "[walkthrough]") {
    const TempFolder tmp;
    WalkthroughSession::Options options;
    options.folder = tmp / "capture";
    std::filesystem::path path;
    {
        auto session = std::make_unique<WalkthroughSession>(std::move(options));
        const auto shot = session->add_shot(std::make_shared<FakeShotImage>(4, 3, 0x445566), 1.0);
        REQUIRE(shot);
        path = shot->path;
        session->end();
    }
    CHECK(std::filesystem::exists(path));
}
