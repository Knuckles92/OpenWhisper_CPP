// The upload client readied while a recording is made (tests/test_api_upload_warmup.py), against
// UploadClients' prepared-client slot: a scripted UploadClient stands for OpenWhisper's upload
// worker process, and a ManualExecutor runs the idle timer.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/manual_executor.h"
#include "cloud_test_support.h"
#include "speech/cloud_backend.h"

using namespace fv;
using namespace fv::speech;
using namespace fv::speech::test;
using namespace std::chrono_literals;

namespace {

// A gate a scripted step waits on (threading.Event).
class Gate {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

struct Script {
    std::shared_ptr<Gate> prepare_gate;
    std::shared_ptr<Gate> transcribe_gate;
    bool prepare_fails = false;
};

// Stands for SpeechProcess: records its operations, and can stall or fail one.
class ScriptedWorker final : public UploadClient {
public:
    ScriptedWorker(CloudConnection connection, Script script)
        : connection(std::move(connection)), script_(std::move(script)) {}

    void prepare(const CancelToken&) override {
        record("prepare");
        if (script_.prepare_gate) {
            REQUIRE(script_.prepare_gate->wait(5s));
            if (closed()) {
                throw EngineError("Transcription canceled");
            }
        }
        if (script_.prepare_fails) {
            throw std::runtime_error("no SDK");
        }
    }
    std::string transcribe(const UploadRequest&, const CancelToken&) override {
        record("transcribe");
        if (script_.transcribe_gate) {
            REQUIRE(script_.transcribe_gate->wait(5s));
            if (closed()) {
                throw EngineError("Transcription canceled");
            }
        }
        return "transcript";
    }
    bool alive() const override { return !closed() && !died; }
    void close() override { closed_ = true; }

    bool closed() const { return closed_; }
    std::vector<std::string> ops() const {
        std::lock_guard lock(mutex_);
        return ops_;
    }

    CloudConnection connection;
    std::atomic<bool> died{false};

private:
    void record(const char* op) {
        std::lock_guard lock(mutex_);
        ops_.emplace_back(op);
    }

    Script script_;
    mutable std::mutex mutex_;
    std::vector<std::string> ops_;
    std::atomic<bool> closed_{false};
};

// The workers a factory started, in order.
struct Workers {
    std::mutex mutex;
    std::vector<std::shared_ptr<ScriptedWorker>> started;
    Script script;

    UploadClientFactory factory() {
        return [this](const CloudConnection& connection) {
            std::lock_guard lock(mutex);
            auto worker = std::make_shared<ScriptedWorker>(connection, script);
            started.push_back(worker);
            return worker;
        };
    }
    std::size_t size() {
        std::lock_guard lock(mutex);
        return started.size();
    }
    std::shared_ptr<ScriptedWorker> at(std::size_t index) {
        std::lock_guard lock(mutex);
        return started.at(index);
    }
    std::vector<std::vector<std::string>> ops() {
        std::lock_guard lock(mutex);
        std::vector<std::vector<std::string>> all;
        for (const auto& worker : started) {
            all.push_back(worker->ops());
        }
        return all;
    }
};

const CloudConnection kConnection{"http://127.0.0.1:9/v1", "sk-test", 30.0};

std::string upload(UploadClients& client) {
    return client.create(UploadRequest{"a.wav", "whisper-1", "json", {}}, CancelToken());
}

bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

using Ops = std::vector<std::vector<std::string>>;

} // namespace

// from tests/test_api_upload_warmup.py::test_an_upload_uses_the_worker_readied_for_it
TEST_CASE("test_an_upload_uses_the_worker_readied_for_it", "[test_api_upload_warmup]") {
    Workers workers;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    client.warm();
    CHECK(workers.ops() == Ops{{"prepare"}});
    CHECK(workers.at(0)->connection == kConnection);
    CHECK(upload(client) == "transcript");
    CHECK(workers.ops() == Ops{{"prepare", "transcribe"}});
    CHECK(workers.at(0)->closed());
}

// from tests/test_api_upload_warmup.py::test_an_upload_without_a_readied_worker_starts_its_own
TEST_CASE("test_an_upload_without_a_readied_worker_starts_its_own", "[test_api_upload_warmup]") {
    Workers workers;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    CHECK(upload(client) == "transcript");
    CHECK(upload(client) == "transcript");
    CHECK(workers.ops() == Ops{{"transcribe"}, {"transcribe"}});
    CHECK(workers.at(0)->closed());
    CHECK(workers.at(1)->closed());
}

// from tests/test_api_upload_warmup.py::test_each_upload_takes_at_most_one_readied_worker
TEST_CASE("test_each_upload_takes_at_most_one_readied_worker", "[test_api_upload_warmup]") {
    Workers workers;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    client.warm();
    client.warm();
    CHECK(workers.size() == 1);
    upload(client);
    upload(client);
    CHECK(workers.ops() == Ops{{"prepare", "transcribe"}, {"transcribe"}});
    client.warm();
    CHECK(workers.at(2)->ops() == std::vector<std::string>{"prepare"});
    CHECK_FALSE(workers.at(2)->closed());
}

// from tests/test_api_upload_warmup.py::test_a_failed_warm_up_leaves_the_upload_to_start_its_own
TEST_CASE("test_a_failed_warm_up_leaves_the_upload_to_start_its_own", "[test_api_upload_warmup]") {
    Workers workers;
    workers.script.prepare_fails = true;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    client.warm();
    CHECK(workers.at(0)->closed());
    workers.script.prepare_fails = false;
    CHECK(upload(client) == "transcript");
    CHECK(workers.at(0)->ops() == std::vector<std::string>{"prepare"});
    CHECK(workers.at(1)->ops() == std::vector<std::string>{"transcribe"});
}

// from tests/test_api_upload_warmup.py::test_an_upload_waits_for_a_warm_up_still_under_way
TEST_CASE("test_an_upload_waits_for_a_warm_up_still_under_way", "[test_api_upload_warmup]") {
    Workers workers;
    const auto release = std::make_shared<Gate>();
    workers.script.prepare_gate = release;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    std::thread warming([&] { client.warm(); });
    REQUIRE(wait_for([&] {
        return workers.size() == 1 && workers.at(0)->ops() == std::vector<std::string>{"prepare"};
    }));
    std::string result;
    std::atomic<bool> done{false};
    std::thread uploading([&] {
        result = upload(client);
        done = true;
    });
    std::this_thread::sleep_for(200ms);
    // Still waiting, and not on a second worker.
    CHECK_FALSE(done);
    CHECK(workers.size() == 1);
    release->open();
    uploading.join();
    warming.join();
    CHECK(result == "transcript");
    CHECK(workers.at(0)->ops() == std::vector<std::string>{"prepare", "transcribe"});
    CHECK(workers.size() == 1);
}

// from tests/test_api_upload_warmup.py::test_a_readied_worker_that_died_is_replaced
TEST_CASE("test_a_readied_worker_that_died_is_replaced", "[test_api_upload_warmup]") {
    Workers workers;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    client.warm();
    workers.at(0)->died = true;
    CHECK(upload(client) == "transcript");
    CHECK(workers.at(0)->ops() == std::vector<std::string>{"prepare"});
    CHECK(workers.at(0)->closed());
    CHECK(workers.at(1)->ops() == std::vector<std::string>{"transcribe"});

    client.warm();
    workers.at(2)->died = true;
    client.warm();
    CHECK(workers.at(2)->closed());
    CHECK(workers.at(3)->ops() == std::vector<std::string>{"prepare"});
}

// from
// tests/test_api_upload_warmup.py::test_closing_the_client_ends_a_warm_up_and_the_upload_waiting_on_it
TEST_CASE("test_closing_the_client_ends_a_warm_up_and_the_upload_waiting_on_it",
          "[test_api_upload_warmup]") {
    Workers workers;
    const auto release = std::make_shared<Gate>();
    workers.script.prepare_gate = release;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    std::thread warming([&] { client.warm(); });
    REQUIRE(wait_for([&] {
        return workers.size() == 1 && workers.at(0)->ops() == std::vector<std::string>{"prepare"};
    }));
    std::string error;
    std::thread uploading([&] {
        try {
            upload(client);
        } catch (const std::exception& failure) {
            error = failure.what();
        }
    });
    std::this_thread::sleep_for(100ms);
    client.close();
    release->open();
    uploading.join();
    warming.join();
    CHECK(error == "Transcription canceled");
    CHECK(workers.size() == 1);
    CHECK(workers.at(0)->closed());
    client.warm();
    CHECK(workers.size() == 1);
}

TEST_CASE("a cancelled upload stops waiting for a warm-up still under way",
          "[test_api_upload_warmup]") {
    // Python's cancel closed the whole client; here the job's token ends the wait on its own.
    Workers workers;
    const auto release = std::make_shared<Gate>();
    workers.script.prepare_gate = release;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    std::thread warming([&] { client.warm(); });
    REQUIRE(wait_for([&] {
        return workers.size() == 1 && workers.at(0)->ops() == std::vector<std::string>{"prepare"};
    }));
    CancelSource cancel;
    std::string error;
    std::atomic<bool> done{false};
    std::thread uploading([&] {
        try {
            client.create(UploadRequest{"a.wav", "whisper-1", "json", {}}, cancel.token());
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        done = true;
    });
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(done);
    cancel.cancel();
    CHECK(wait_for([&] { return done.load(); }, 5s));
    uploading.join();
    CHECK(error == "Transcription canceled");
    // The worker it gave up on is closed and nothing was uploaded.
    CHECK(workers.at(0)->closed());
    release->open();
    warming.join();
    CHECK(workers.at(0)->ops() == std::vector<std::string>{"prepare"});
    // The client stays usable for the next upload.
    CHECK(upload(client) == "transcript");
}

// from tests/test_api_upload_warmup.py::test_a_readied_worker_nobody_uses_is_closed
TEST_CASE("test_a_readied_worker_nobody_uses_is_closed", "[test_api_upload_warmup]") {
    Workers workers;
    ManualExecutor timers;
    UploadClients client(CloudConnection{std::nullopt, "sk-test", kHostedTimeoutS},
                         workers.factory(), timers, 50ms);
    client.warm();
    CHECK_FALSE(workers.at(0)->closed());
    timers.advance(50ms);
    CHECK(workers.at(0)->closed());
    CHECK(upload(client) == "transcript");
    CHECK(workers.at(1)->ops() == std::vector<std::string>{"transcribe"});
}

// from tests/test_api_upload_warmup.py::test_the_idle_limit_never_closes_a_worker_an_upload_took
TEST_CASE("test_the_idle_limit_never_closes_a_worker_an_upload_took", "[test_api_upload_warmup]") {
    Workers workers;
    const auto release = std::make_shared<Gate>();
    workers.script.transcribe_gate = release;
    ManualExecutor timers;
    UploadClients client(kConnection, workers.factory(), timers);
    client.warm();
    std::string result;
    std::thread uploading([&] { result = upload(client); });
    REQUIRE(wait_for(
        [&] { return workers.at(0)->ops() == std::vector<std::string>{"prepare", "transcribe"}; }));
    // Even an idle limit that comes due now finds the worker taken.
    timers.advance(kStandbyIdle + 1s);
    CHECK_FALSE(workers.at(0)->closed());
    release->open();
    uploading.join();
    CHECK(result == "transcript");
    CHECK(workers.size() == 1);
}

// ---- the worker's side: a client prepared for one connection ----

// from tests/test_api_upload_warmup.py::test_the_worker_uploads_with_the_client_it_prepared
TEST_CASE("test_the_worker_uploads_with_the_client_it_prepared", "[test_api_upload_warmup]") {
    // Adapted: the backend readies a client for its connection; the upload takes that client,
    // which then serves nothing else.
    CloudRig rig;
    Workers workers;
    rig.options.upload_clients = workers.factory();
    rig.stub_splits({});
    auto& backend = rig.make("whisper-1");
    backend.warm();
    REQUIRE(workers.size() == 1);
    CHECK(workers.at(0)->ops() == std::vector<std::string>{"prepare"});
    CHECK(workers.at(0)->connection == CloudConnection{std::nullopt, "sk-test", kHostedTimeoutS});
    backend.decode(file_job(rig.chunk_files(1).front()));
    CHECK(workers.size() == 1);
    CHECK(workers.at(0)->ops() == std::vector<std::string>{"prepare", "transcribe"});
    CHECK(workers.at(0)->closed());
    // Taken once: the next upload builds its own.
    backend.decode(file_job(rig.chunk_files(1).front()));
    CHECK(workers.size() == 2);
}

// from tests/test_api_upload_warmup.py::test_a_client_prepared_for_another_connection_is_not_used
TEST_CASE("test_a_client_prepared_for_another_connection_is_not_used", "[test_api_upload_warmup]") {
    // The key, the address and the read timeout each make another connection.
    const int change = GENERATE(0, 1, 2);
    CAPTURE(change);
    CloudRig rig;
    Workers workers;
    rig.options.upload_clients = workers.factory();
    rig.stub_splits({});
    rig.apply([](Json& settings) {
        save_server(settings, "custom", "https://speech.example.com/v1", "CUSTOM_KEY", "m");
        save_selection(settings, "custom");
    });
    rig.credentials.set("CUSTOM_KEY", "sk-first");
    auto& backend = rig.make("api", false);
    backend.warm();
    REQUIRE(workers.size() == 1);
    if (change == 0) {
        rig.credentials.set("CUSTOM_KEY", "sk-other");
    } else if (change == 1) {
        rig.apply([](Json& settings) {
            save_server(settings, "custom", "https://other.example.com/v1", "CUSTOM_KEY", "m");
        });
    } else {
        // A loopback address is a server the user runs: the long read timeout.
        rig.apply([](Json& settings) {
            save_server(settings, "custom", "http://127.0.0.1:9000/v1", "CUSTOM_KEY", "m");
        });
    }
    backend.decode(file_job(rig.chunk_files(1).front()));
    CHECK(workers.at(0)->closed());
    CHECK(workers.at(0)->ops() == std::vector<std::string>{"prepare"});
    REQUIRE(workers.size() == 2);
    CHECK(workers.at(1)->ops() == std::vector<std::string>{"transcribe"});
    const CloudConnection& used = workers.at(1)->connection;
    CHECK(used.key == (change == 0 ? "sk-other" : "sk-first"));
    CHECK(used.base_url == (change == 1   ? "https://other.example.com/v1"
                            : change == 2 ? "http://127.0.0.1:9000/v1"
                                          : "https://speech.example.com/v1"));
    CHECK(used.timeout_s == (change == 2 ? kSelfHostedTimeoutS : kHostedTimeoutS));
}

// from tests/test_api_upload_warmup.py::test_preparing_again_closes_the_client_prepared_before
TEST_CASE("test_preparing_again_closes_the_client_prepared_before", "[test_api_upload_warmup]") {
    CloudRig rig;
    Workers workers;
    rig.options.upload_clients = workers.factory();
    auto& backend = rig.make("whisper-1");
    backend.warm();
    rig.credentials.set("OPENAI_API_KEY", "sk-new");
    backend.warm();
    REQUIRE(workers.size() == 2);
    CHECK(workers.at(0)->closed());
    CHECK_FALSE(workers.at(1)->closed());
}

// ---- the backend and the recording's start ----

// from tests/test_api_upload_warmup.py::test_the_api_backend_readies_its_client_and_never_raises
TEST_CASE("test_the_api_backend_readies_its_client_and_never_raises", "[test_api_upload_warmup]") {
    CloudRig rig;
    std::atomic<int> built{0};
    std::vector<std::shared_ptr<ScriptedWorker>> readied;
    // The first client cannot start; the backend logs it and carries on.
    rig.options.upload_clients =
        [&](const CloudConnection& connection) -> std::shared_ptr<UploadClient> {
        if (++built == 1) {
            throw std::runtime_error("cannot start a process");
        }
        readied.push_back(std::make_shared<ScriptedWorker>(connection, Script{}));
        return readied.back();
    };
    auto& backend = rig.make("whisper-1");
    CHECK(backend.prepares_for_transcription());
    CHECK_NOTHROW(backend.prepare_for_transcription());
    backend.prepare_for_transcription();
    REQUIRE(readied.size() == 1);
    CHECK(readied[0]->ops() == std::vector<std::string>{"prepare"});
    // A warm while that readied client lives starts nothing more.
    backend.prepare_for_transcription();
    CHECK(built == 2);

    rig.credentials.remove("OPENAI_API_KEY");
    CHECK_NOTHROW(backend.prepare_for_transcription());
    CHECK(built == 2);
    CHECK(readied[0]->closed());
    CHECK_FALSE(backend.is_available());
}

// from
// tests/test_api_upload_warmup.py::test_only_an_engine_that_asks_is_prepared_when_a_recording_starts
TEST_CASE("test_only_an_engine_that_asks_is_prepared_when_a_recording_starts",
          "[test_api_upload_warmup]") {
    // Partly: the backends' side. The dictation runtime's call at recording start is the
    // dictation package's (recorded pending in PARITY.json).
    struct Plain final : Backend {
        std::string_view id() const override { return "plain"; }
        Capabilities capabilities() const override { return {}; }
        Readiness load(const CancelToken&) override { return {ReadyState::Ready, "", ""}; }
        DecodeResult decode(const DecodeJob&) override { return {}; }
        void release() override {}
    } plain;
    CHECK_FALSE(plain.prepares_for_transcription());
    CHECK_FALSE(plain.supports_recognition());
    CHECK_NOTHROW(plain.prepare_for_transcription());
    CloudRig rig;
    CHECK(rig.make().prepares_for_transcription());
}
