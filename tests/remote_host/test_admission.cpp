// How much the host takes at once (services/remote_asr/host.py _Reservation and
// _WorkAdmission; tests/test_remote_host_admission.py): slots for handshakes and paired
// connections, and the FIFO work admission, alone and over loopback TLS.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "host_rig.h"
#include "remote_host/admission.h"

using fv::Json;
using fv::remote_host::SlotPool;
using fv::remote_host::WorkAdmission;
using namespace fv::remote_host::rig;
using namespace std::chrono_literals;

namespace {

// tests/test_remote_host_admission.py _BlockingEngine: the first transcribe waits until
// released; each answers "job <n>".
class BlockingEngine final : public fv::remote_host::HostEngine {
public:
    std::vector<std::string> identity() const override { return {"fake", "speech"}; }
    Json describe() const override {
        return Json{
            {"family", "fake"}, {"label", "Test"}, {"available", true}, {"streaming", true}};
    }
    Json transcribe(std::span<const float>, std::optional<std::string>,
                    std::span<const std::string>) override {
        int number = 0;
        {
            std::unique_lock lock(mutex_);
            number = ++calls_;
            if (number == 1) {
                entered_ = true;
                changed_.notify_all();
                changed_.wait_for(lock, 5s, [&] { return released_; });
            }
        }
        return Json{{"text", "job " + std::to_string(number)}, {"segments", Json::array()}};
    }
    Json stream(std::string_view, std::span<const float>, std::optional<std::string>,
                bool) override {
        return Json{{"events", Json::array()}};
    }

    bool wait_entered(std::chrono::milliseconds timeout = 3s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return entered_; });
    }
    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    int calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    int calls_ = 0;
};

struct AdmissionRig {
    AdmissionRig(int connections, int handshakes, int active, int queued,
                 std::function<void(fv::remote_host::SpeechHostDeps&)> more = {})
        : rig([&](fv::remote_host::SpeechHostDeps& deps) {
              deps.limits.max_connections = connections;
              deps.limits.max_handshakes = handshakes;
              deps.limits.max_active_jobs = active;
              deps.limits.max_queued_jobs = queued;
              if (more) {
                  more(deps);
              }
          }) {
        rig.serve(engine);
        const auto paired = rig.pair();
        REQUIRE(!paired.token.empty());
        token = paired.token;
        REQUIRE(wait_for([&] { return rig.host->free_connection_slots() == connections; }));
        REQUIRE(wait_for([&] { return rig.host->free_handshake_slots() == handshakes; }));
    }
    ~AdmissionRig() { engine->release(); }

    Connected connection() {
        auto out = connect(rig.port, token);
        REQUIRE(out.ready.value("type", "") == "ready");
        return out;
    }

    std::shared_ptr<BlockingEngine> engine = std::make_shared<BlockingEngine>();
    HostRig rig;
    std::string token;
};

std::future<Json> transcribe_later(RawClient& client) {
    return std::async(std::launch::async, [&client] {
        return client.request("transcribe", Json::object(), std::vector<float>(1600, 0.0f), 10s);
    });
}

} // namespace

// ---- the pieces
// ----------------------------------------------------------------------------------

TEST_CASE("a slot pool hands out its slots without blocking and takes them back once") {
    SlotPool slots(2);
    fv::remote_host::Reservation first(slots);
    fv::remote_host::Reservation second(slots);
    fv::remote_host::Reservation third(slots);
    CHECK(first.acquire());
    CHECK(second.acquire());
    CHECK_FALSE(third.acquire());
    CHECK(slots.available() == 0);
    first.release();
    first.release();
    CHECK(slots.available() == 1);
    CHECK(SlotPool(0).capacity() == 1);
}

TEST_CASE("the work admission takes requests in order and refuses past its queue") {
    fv::FakeClock clock;
    fv::ManualExecutor timers(clock);
    WorkAdmission admission(timers, 1, 2, 30'000ms);
    std::vector<std::string> log;
    const auto note = [&](std::string name) {
        return [&log, name](bool admitted) {
            log.push_back(name + (admitted ? "+" : "-"));
        };
    };
    CHECK(admission.acquire(note("a")) == nullptr);
    const auto b = admission.acquire(note("b"));
    CHECK(admission.acquire(note("c")) != nullptr);
    CHECK(admission.acquire(note("d")) == nullptr);
    CHECK(log == std::vector<std::string>{"a+", "d-"});
    CHECK(admission.queued() == 2);
    admission.release();
    CHECK(log == std::vector<std::string>{"a+", "d-", "b+"});
    admission.release();
    CHECK(log == std::vector<std::string>{"a+", "d-", "b+", "c+"});
    CHECK(admission.active() == 1);
    admission.release();
    CHECK(admission.active() == 0);
    // A ticket of a waiter already admitted cancels nothing.
    admission.cancel(b);
    CHECK(log.size() == 4);
}

TEST_CASE("a waiter gives up after the longest wait and a cancelled one lets the next in") {
    fv::FakeClock clock;
    fv::ManualExecutor timers(clock);
    WorkAdmission admission(timers, 1, 4, 30'000ms);
    std::vector<std::string> log;
    const auto note = [&](std::string name) {
        return [&log, name](bool admitted) {
            log.push_back(name + (admitted ? "+" : "-"));
        };
    };
    admission.acquire(note("a"));
    CHECK(admission.acquire(note("b")) != nullptr);
    clock.advance(10s);
    const auto c = admission.acquire(note("c"));
    clock.advance(20s);
    CHECK(log == std::vector<std::string>{"a+", "b-"});
    admission.cancel(c);
    CHECK(log == std::vector<std::string>{"a+", "b-", "c-"});
    CHECK(admission.queued() == 0);
    CHECK(admission.acquire(note("d")) != nullptr);
    admission.cancel_all();
    CHECK(log.back() == "d-");
    admission.release();
    CHECK(admission.acquire(note("e")) == nullptr);
    CHECK(log.back() == "e+");
}

// ---- over loopback ------------------------------------------------------------------------------

TEST_CASE("test_connection_cap_is_retryable_and_releases_after_disconnect") {
    AdmissionRig fixture(1, 5, 1, 1);
    auto first = fixture.connection();
    REQUIRE(wait_for([&] { return fixture.rig.host->connected_clients().size() == 1; }));
    auto second = connect(fixture.rig.port, fixture.token);
    CHECK(second.ready == Json{{"type", "error"},
                               {"code", "busy"},
                               {"message", "The host is busy. Try again shortly."},
                               {"retry_after_ms", 1000}});
    CHECK(second.client->wait_closed());
    CHECK(second.client->close_code() == fv::remote::CLOSE_BUSY);
    CHECK(second.client->close_reason() == "host busy");
    first.client->close();
    REQUIRE(wait_for([&] { return fixture.rig.host->connected_clients().empty(); }));
    REQUIRE(wait_for([&] { return fixture.rig.host->free_connection_slots() == 1; }));
    CHECK(fixture.connection().ready.value("type", "") == "ready");
}

TEST_CASE("test_a_connection_that_never_says_hello_leaves_paired_slots_free") {
    AdmissionRig fixture(1, 2, 1, 1);
    RawClient idle(fixture.rig.port);
    REQUIRE(idle.connected());
    REQUIRE(wait_for([&] { return fixture.rig.host->free_handshake_slots() == 1; }));
    auto paired = fixture.connection();
    CHECK(fixture.rig.host->free_connection_slots() == 0);
    CHECK(wait_for([&] { return fixture.rig.host->free_handshake_slots() == 1; }));
}

TEST_CASE("test_paired_connections_return_their_handshake_slot") {
    AdmissionRig fixture(3, 1, 1, 1);
    std::vector<Connected> connections;
    for (int i = 0; i < 3; ++i) {
        connections.push_back(fixture.connection());
    }
    REQUIRE(wait_for([&] { return fixture.rig.host->free_handshake_slots() == 1; }));
    RawClient probe(fixture.rig.port);
    probe.send(Json{{"type", "probe"}, {"protocol", 1}});
    const auto answer = probe.receive();
    REQUIRE(answer.has_value());
    CHECK(answer->at("protocol") == 1);
    probe.wait_closed();
    REQUIRE(wait_for([&] { return fixture.rig.host->free_handshake_slots() == 1; }));
    auto extra = connect(fixture.rig.port, fixture.token);
    CHECK(extra.ready.value("code", "") == "busy");
    CHECK(wait_for([&] { return fixture.rig.host->free_handshake_slots() == 1; }));
}

// The host half: a refusal before hello is sent before the close, so a client whose hello is
// sent late still reads the delay (client.py reads the queued reply after a failed send).
TEST_CASE("test_connection_refusal_before_hello_preserves_retry_delay") {
    AdmissionRig fixture(5, 1, 1, 1);
    RawClient holder(fixture.rig.port);
    REQUIRE(holder.connected());
    REQUIRE(wait_for([&] { return fixture.rig.host->free_handshake_slots() == 0; }));
    RawClient refused(fixture.rig.port);
    REQUIRE(refused.connected());
    refused.send(hello_message(fixture.token));
    const auto reply = refused.receive();
    REQUIRE(reply.has_value());
    CHECK(reply->value("code", "") == "busy");
    CHECK(reply->at("retry_after_ms") == 1000);
    CHECK(refused.wait_closed());
    CHECK(refused.close_code() == fv::remote::CLOSE_BUSY);
}

TEST_CASE("test_fifo_job_queue_rejects_overload_before_audio_decode_and_recovers") {
    AdmissionRig fixture(5, 5, 1, 1);
    auto first = fixture.connection();
    auto second = fixture.connection();
    auto third = fixture.connection();
    auto one = transcribe_later(*first.client);
    REQUIRE(fixture.engine->wait_entered());
    auto two = transcribe_later(*second.client);
    REQUIRE(wait_for([&] { return fixture.rig.host->queued_jobs() == 1; }));

    const Json refused =
        third.client->request("transcribe", Json::object(), std::vector<float>(1600, 0.0f));
    CHECK(refused == Json{{"id", 1},
                          {"code", "busy"},
                          {"error", "The host is busy. Try again shortly."},
                          {"retry_after_ms", 1000}});
    // Refused before anything was decoded.
    CHECK(fixture.engine->calls() == 1);

    fixture.engine->release();
    CHECK(one.get().at("result").at("text") == "job 1");
    CHECK(two.get().at("result").at("text") == "job 2");
    CHECK(third.client->request("transcribe", Json::object(), std::vector<float>(1600, 0.0f))
              .at("result")
              .at("text") == "job 3");
}

TEST_CASE("test_disconnected_queued_client_releases_its_slot") {
    AdmissionRig fixture(5, 5, 1, 1);
    auto first = fixture.connection();
    auto second = fixture.connection();
    auto third = fixture.connection();
    auto one = transcribe_later(*first.client);
    REQUIRE(fixture.engine->wait_entered());
    Json header = Json{{"id", 1}, {"op", "transcribe"}};
    second.client->send_binary(fv::remote::pack_request(header, std::vector<float>(1600, 0.0f)));
    REQUIRE(wait_for([&] { return fixture.rig.host->queued_jobs() == 1; }));
    second.client->close();
    CHECK(wait_for([&] { return fixture.rig.host->queued_jobs() == 0; }));
    CHECK(fixture.engine->calls() == 1);
    fixture.engine->release();
    CHECK(one.get().at("result").at("text") == "job 1");
    CHECK(third.client->request("transcribe", Json::object(), std::vector<float>(1600, 0.0f))
              .at("result")
              .at("text") == "job 2");
}

namespace {

// Every transcribe waits for a permit, unless the gate is open.
class PermitEngine final : public fv::remote_host::HostEngine {
public:
    std::vector<std::string> identity() const override { return {"fake", "speech"}; }
    Json describe() const override {
        return Json{{"family", "fake"}, {"label", "Test"}, {"available", true}};
    }
    Json transcribe(std::span<const float>, std::optional<std::string>,
                    std::span<const std::string>) override {
        std::unique_lock lock(mutex_);
        ++entered_;
        changed_.notify_all();
        changed_.wait_for(lock, 5s, [&] { return open_ || permits_ > 0; });
        if (!open_ && permits_ > 0) {
            --permits_;
        }
        return Json{{"text", "ok"}, {"segments", Json::array()}};
    }
    bool wait_entered(int count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 3s, [&] { return entered_ >= count; });
    }
    void permit() {
        std::lock_guard lock(mutex_);
        ++permits_;
        changed_.notify_all();
    }
    // Closing the gate also forgets a permit that the open gate made unnecessary.
    void set_open(bool open) {
        std::lock_guard lock(mutex_);
        open_ = open;
        if (!open) {
            permits_ = 0;
        }
        changed_.notify_all();
    }
    int entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    int entered_ = 0;
    int permits_ = 0;
    bool open_ = false;
};

} // namespace

// A queued client that leaves just as a slot frees up gives the slot back. The host's eight pool
// threads are all busy (one decode, seven record requests held in their callback), so the
// departed client's close waits on its strand; the decode then ends, admits the queued request
// behind that close, and its own thread runs the close next, which drops the connection and the
// admitted task with it. The slot must come back then, or every later request would wait out
// job_wait behind a slot nobody holds.
TEST_CASE("a client leaving as it is admitted gives its work slot back") {
    constexpr int kPoolThreads = 8;
    std::mutex mutex;
    std::condition_variable changed;
    int holding = 0;
    bool released = false;
    auto engine = std::make_shared<PermitEngine>();
    HostRig rig([&](fv::remote_host::SpeechHostDeps& deps) {
        deps.limits.max_active_jobs = 1;
        deps.limits.max_queued_jobs = 1;
        deps.limits.job_wait = 2s;
        deps.records = [&](std::string_view, const Json&, std::span<const std::byte>,
                           const fv::remote::PairedDevice&) {
            std::unique_lock lock(mutex);
            ++holding;
            changed.notify_all();
            changed.wait_for(lock, 10s, [&] { return released; });
            --holding;
            return Json::object();
        };
    });
    rig.serve(engine);
    const auto release_records = [&] {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    };
    auto first = rig.connected();
    auto second = connect(rig.port, rig.token);
    REQUIRE(second.ready.value("type", "") == "ready");
    std::vector<Connected> holders;
    for (int i = 0; i < kPoolThreads - 1; ++i) {
        holders.push_back(connect(rig.port, rig.token));
        REQUIRE(holders.back().ready.value("type", "") == "ready");
    }

    auto one = transcribe_later(*first.client);
    REQUIRE(engine->wait_entered(1));
    second.client->send_binary(fv::remote::pack_request(Json{{"id", 1}, {"op", "transcribe"}},
                                                        std::vector<float>(160, 0.0f)));
    REQUIRE(wait_for([&] { return rig.host->queued_jobs() == 1; }));
    for (auto& holder : holders) {
        holder.client->send_binary(
            fv::remote::pack_request(Json{{"id", 1}, {"op", "records_list"}}));
    }
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, 5s, [&] { return holding == kPoolThreads - 1; }));
    }
    second.client->abort();
    // The close reaches the reactor and waits on the second client's strand.
    std::this_thread::sleep_for(200ms);
    CHECK(rig.host->connected_clients().size() == kPoolThreads + 1);
    engine->set_open(true);
    CHECK(one.get().at("result").at("text") == "ok");
    REQUIRE(wait_for([&] { return rig.host->connected_clients().size() == kPoolThreads; }));
    release_records();
    for (auto& holder : holders) {
        CHECK(holder.client->receive().has_value());
    }

    const Json next =
        first.client->request("transcribe", Json::object(), std::vector<float>(160, 0.0f));
    CHECK(next.contains("result"));
    CHECK(rig.host->queued_jobs() == 0);
}

TEST_CASE("test_host_time_includes_waiting_for_a_work_slot") {
    AdmissionRig fixture(5, 5, 1, 1);
    auto first = fixture.connection();
    auto second = fixture.connection();
    auto one = transcribe_later(*first.client);
    REQUIRE(fixture.engine->wait_entered());
    auto two = transcribe_later(*second.client);
    REQUIRE(wait_for([&] { return fixture.rig.host->queued_jobs() == 1; }));
    std::this_thread::sleep_for(300ms);
    fixture.engine->release();
    one.get();
    const Json reply = two.get();
    CHECK(reply.at("result").at("text") == "job 2");
    CHECK(reply.at("host_ms").get<double>() >= 300.0);
}

TEST_CASE("test_record_transfers_do_not_wait_for_speech_work") {
    AdmissionRig fixture(5, 5, 1, 0, [](fv::remote_host::SpeechHostDeps& deps) {
        deps.records = [](std::string_view op, const Json&, std::span<const std::byte>,
                          const fv::remote::PairedDevice&) {
            return Json{{"op", op}};
        };
    });
    auto first = fixture.connection();
    auto second = fixture.connection();
    auto one = transcribe_later(*first.client);
    REQUIRE(fixture.engine->wait_entered());
    CHECK(second.client->request("records_list").at("result") == Json{{"op", "records_list"}});
    const Json refused =
        second.client->request("transcribe", Json::object(), std::vector<float>(1600, 0.0f));
    CHECK(refused.value("code", "") == "busy");
    fixture.engine->release();
    one.get();
}
