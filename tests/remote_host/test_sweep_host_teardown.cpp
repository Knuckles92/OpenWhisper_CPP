// SpeechHost going away while one of the server's callbacks is still running on the reactor and
// a decode is still running on the host's pool: the host's state is destroyed by its owner,
// which waits for both, and never by the callback on the reactor thread (open item B32).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "host_rig.h"
#include "remote/protocol.h"
#include "remote_host/registry.h"
#include "remote_host/speech_host.h"

using fv::Json;
using namespace fv::remote_host;
using namespace std::chrono_literals;

namespace {

// Holds the first reactor callback after `arm()` inside the host's reactor_timing hook, which
// runs at the end of each callback, until `release()`.
struct ReactorHold {
    std::mutex mutex;
    std::condition_variable changed;
    bool armed = false;
    bool taken = false;
    bool released = false;

    void on_callback() {
        std::unique_lock lock(mutex);
        if (!armed || taken) {
            return;
        }
        taken = true;
        changed.notify_all();
        changed.wait(lock, [this] { return released; });
    }
    bool wait_taken() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [this] { return taken; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }
};

// A decode that waits until released: the model switch or long window a pool thread runs.
class HeldEngine final : public HostEngine {
public:
    std::vector<std::string> identity() const override { return {"fake", "parakeet", "held"}; }
    Json describe() const override {
        return Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"label", "Fake Parakeet"},
                    {"device", "cuda"},     {"streaming", false},     {"available", true},
                    {"status", "ready"}};
    }
    Json transcribe(std::span<const float>, std::optional<std::string>,
                    std::span<const std::string>) override {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
        finished_ = true;
        return Json{{"text", "ok"}, {"segments", Json::array()}};
    }
    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [this] { return entered_; });
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }
    bool finished() {
        std::lock_guard lock(mutex_);
        return finished_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    bool finished_ = false;
};

} // namespace

TEST_CASE("sweep B32: the host is destroyed by its owner while a callback and a decode run") {
    fv::net::Reactor reactor{2};
    testing::MemoryDeviceStore store;
    DeviceRegistry registry(store.loader(), store.saver(), fv::system_clock());
    auto engine = std::make_shared<HeldEngine>();
    auto hold = std::make_shared<ReactorHold>();
    SpeechHostDeps deps{
        .reactor = reactor,
        .registry = registry,
        .identity = rig::test_identity(),
        .host_name = "devbox",
        .clock = fv::system_clock(),
    };
    deps.engine_provider = [engine] {
        return engine;
    };
    deps.reactor_timing = [hold](std::chrono::nanoseconds) {
        hold->on_callback();
    };
    deps.limits.stop_grace = 200ms;
    auto host = std::make_unique<SpeechHost>(std::move(deps));
    const auto started = host->start(0, "127.0.0.1", std::nullopt);
    REQUIRE(started.has_value());

    const auto paired = rig::pair_with_code(*started, host->open_pairing());
    REQUIRE_FALSE(paired.token.empty());
    auto connection = rig::connect(*started, paired.token);
    REQUIRE(connection.ready.value("type", "") == "ready");

    // A decode on the pool, then a callback held on the reactor.
    Json decode = Json{{"id", 1}, {"op", "transcribe"}};
    connection.client->send_binary(fv::remote::pack_request(decode, std::vector<float>(160, 0)));
    REQUIRE(engine->wait_entered());
    {
        std::lock_guard lock(hold->mutex);
        hold->armed = true;
    }
    connection.client->send_binary(fv::remote::pack_request(Json{{"id", 2}, {"op", "describe"}}));
    REQUIRE(hold->wait_taken());

    std::atomic<bool> destroyed = false;
    std::thread owner([&] {
        host.reset();
        destroyed = true;
    });
    // The owner waits for the callback: it never leaves the host's state to it.
    std::this_thread::sleep_for(1500ms);
    CHECK_FALSE(destroyed.load());
    hold->release();
    // ... and then for the decode still running on the pool, on its own thread.
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(destroyed.load());
    engine->release();
    owner.join();
    CHECK(destroyed.load());
    CHECK(engine->finished());
}
