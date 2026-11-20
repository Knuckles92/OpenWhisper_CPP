#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agentapi/app_control.h"
#include "agentapi/history_server.h"
#include "automation/control.h"
#include "base/log.h"
#include "base/signal.h"
#include "net/http_server.h"

// Shared by the live API tests of fv_agentapi.
namespace fv::agentapi::test {

// The socket side of an event stream: what was written, whether it ended, and a client that
// stops reading (queued) or leaves (gone).
// Affinity: thread-safe.
class FakeSse final : public net::EventStream {
public:
    bool write(std::string frame) override;
    void close() override;
    std::size_t queued_frames() const override;

    // Every frame written, in order (empty writes are not frames).
    std::vector<std::string> frames() const;
    std::string text() const;
    bool closed() const;
    void set_queued(std::size_t frames);
    void set_gone(bool gone);
    // Waits up to `timeout` for `predicate(frames())`.
    bool wait_for(const std::function<bool(const std::vector<std::string>&)>& predicate,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<std::string> frames_;
    bool closed_ = false;
    bool gone_ = false;
    std::size_t queued_ = 0;
};

// One answer of McpApp::serve: send() or start_sse().
// Affinity: thread-safe.
class CaptureResponder final : public net::Responder {
public:
    void send(int status, net::Headers headers, std::string body) override;
    void send_file(std::filesystem::path, std::string, std::optional<net::ByteRange>) override;
    std::shared_ptr<net::EventStream> start_sse(net::Headers headers) override;

    int status = 0;
    net::Headers headers;
    std::string body;
    std::shared_ptr<FakeSse> stream; // set by start_sse
    int answers = 0;
};

struct Served {
    int status = 0;
    net::Headers headers;
    std::string body; // the stream's frames so far, for an event stream
    std::shared_ptr<FakeSse> stream;

    std::optional<std::string> header(std::string_view name) const;
    Json json() const;
    // The error object of a {"error": {...}} answer.
    Json error() const;
};

// app.serve(request) through a CaptureResponder.
Served serve(McpApp& app, const net::HttpRequest& request);

// A live API request as TestClient(base_url="http://127.0.0.1", client=("127.0.0.1", 50000))
// sends it with the bearer token; a body is sent as application/json. Headers given replace
// those of the same name; an empty optional leaves one out.
net::HttpRequest
live_request(std::string method, std::string path, const std::string& token,
             std::optional<std::string> body = std::nullopt,
             std::vector<std::pair<std::string, std::optional<std::string>>> headers = {});

// FakeControl of tests/test_automation_api_routes.py: every call is recorded, and `error` (set
// with fail_with) is thrown by every call, state() and list_actions() included unless
// `reads_fail` is false.
// Affinity: thread-safe.
class RouteControl final : public automation::AppControl {
public:
    automation::LiveState state() override;
    Json list_actions() override;
    std::string run_action(std::string_view action_id, std::string_view text,
                           std::string_view source) override;
    std::string start_recording(std::optional<std::string> action_id,
                                std::string_view source) override;
    void stop_recording() override;
    void cancel() override;
    void notify(std::string_view text, std::string_view title) override;

    void fail_with(std::function<void()> thrower);
    std::vector<std::vector<std::string>> calls() const;

    automation::LiveState live{false, false, false, "", "", "", "9.9.9"};
    Json actions = Json::parse(
        R"([{"id": "a1", "name": "Ask Claude", "kind": "open_link", "enabled": true}])");
    std::string run_job = "job-run";
    std::string start_job = "job-rec";
    bool reads_fail = true;

private:
    void maybe_fail(bool read) const;

    mutable std::mutex mutex_;
    std::vector<std::vector<std::string>> calls_;
    std::function<void()> fail_;
};

// Registers `control` as the app control for its lifetime; the previous one comes back after.
class ScopedAppControl {
public:
    explicit ScopedAppControl(std::shared_ptr<automation::AppControl> control);
    ~ScopedAppControl();
    ScopedAppControl(const ScopedAppControl&) = delete;
    ScopedAppControl& operator=(const ScopedAppControl&) = delete;

private:
    std::shared_ptr<automation::AppControl> previous_;
};

// FakeController of tests/test_automation_api_app_control.py, as a ControlHost: recording,
// busy, meeting and saving are fields; starts emit their status; every call is recorded with
// the thread it ran on.
// Affinity: thread-safe.
class FakeHost final : public ControlHost {
public:
    struct Call {
        std::string name; // "run", "start", "start_action", "stop", "cancel", "notify"
        std::vector<std::string> args;
        std::thread::id thread;
    };

    automation::LiveState state() override;
    std::optional<std::string> start_recording(std::optional<std::string> action_id,
                                               std::string source) override;
    bool stop_recording() override;
    bool cancel() override;
    std::string run_action_text(std::string action_id, std::string text,
                                std::string source) override;
    void notify(std::string line) override;
    Signal<std::string>& status_updates() override { return status_update; }

    std::vector<Call> calls() const;
    std::vector<std::string> call_names() const;

    Signal<std::string> status_update;
    std::atomic<bool> recording{false};
    std::atomic<bool> busy{false};
    std::atomic<bool> meeting{false};
    // The slot's job was delivered and is only being saved: nothing is left to cancel.
    std::atomic<bool> saving{false};
    std::string refusal; // a start emits it and refuses
    std::chrono::milliseconds delay{0};
    std::function<void()> on_run; // runs inside run_action_text, before it answers
    // The jobs state() reports: the worker slot's, and the one it is working on once busy.
    struct Job {
        std::string id, mode, action_id;
    };
    std::optional<Job> job;
    std::optional<Job> active_job;
    std::string version = "9.9.9";

private:
    void record(std::string name, std::vector<std::string> args);

    mutable std::mutex mutex_;
    std::vector<Call> calls_;
};

// Captures every log record, DEBUG included, for its lifetime.
class ScopedLogCapture final {
public:
    ScopedLogCapture();
    ~ScopedLogCapture();
    ScopedLogCapture(const ScopedLogCapture&) = delete;
    ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

    // Every record's logger, message and exception type, one per line.
    std::string text() const;

private:
    class Sink;
    std::shared_ptr<Sink> sink_;
    log::Level previous_;
};

// Polls `predicate` every 10 ms for up to `timeout`.
bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

} // namespace fv::agentapi::test
