#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/json.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "process/launcher.h"

// The Claude Code channel against an app on real sockets, as
// tests/test_automation_channel_e2e.py runs it: FakeApp serves the two routes the channel uses
// the way the live API describes them, and ChannelProcess runs fiorivoce-cli --channel (or
// fiorivoce --channel) on pipes, as Claude Code starts it.
namespace fv::clitest {

inline const std::string kChannelToken = "t0ken-for-the-channel-tests-0123456789abcdef";
inline constexpr std::chrono::milliseconds kWait{20000};

// One request the fake app received; header names in lower case.
struct SeenRequest {
    std::string method;
    std::string path;
    net::Query query;
    std::map<std::string, std::string> headers;
    Json body;
};

// OpenWhisper's live API in miniature, on 127.0.0.1: GET /v1/events streams channel messages
// (retry: 2000 first, Last-Event-ID replay, a ": ping" every 0.2 s), POST /v1/notify answers 202.
// Affinity: thread-safe.
class FakeApp {
public:
    explicit FakeApp(std::string token = kChannelToken);
    ~FakeApp();
    FakeApp(const FakeApp&) = delete;
    FakeApp& operator=(const FakeApp&) = delete;

    // Listens (on the port it had before, after a stop).
    FakeApp& start();
    // Closes every stream and the listener, as the app does when it quits.
    void stop();
    std::uint16_t port() const { return port_; }
    std::string url() const;

    // A channel.message (or `type`) event to every open stream and the replay buffer; its seq.
    std::int64_t publish(const Json& data, const std::string& type = "channel.message");
    // events.gap as the live API sends it: no id line, seq 0.
    void send_gap(std::int64_t missed_from_seq, const std::string& reason = "evicted");

    std::vector<SeenRequest> stream_requests() const;
    std::vector<SeenRequest> notifications() const;
    std::size_t open_streams() const;
    // Waits until `predicate` holds; false after `timeout`.
    bool wait_for(const std::function<bool()>& predicate,
                  std::chrono::milliseconds timeout = kWait) const;

    std::atomic<int> events_status{200};
    std::atomic<int> notify_status{202};
    void set_notify_message(std::string message);

private:
    void serve(const net::HttpRequest& request, std::shared_ptr<net::Responder> responder);
    void ping_loop();

    const std::string token_;
    std::uint16_t port_ = 0;
    net::Reactor reactor_{2};
    std::unique_ptr<net::HttpServer> server_;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<SeenRequest> stream_requests_;
    std::vector<SeenRequest> notifications_;
    std::vector<std::pair<std::int64_t, std::string>> buffer_;
    std::vector<std::shared_ptr<net::EventStream>> streams_;
    std::int64_t seq_ = 0;
    std::string notify_message_ = "Refused.";
    bool stopping_ = false;
    std::thread pinger_;
};

// This process's environment for a child, without the channel's overrides and log level (new
// and legacy names), with `values` set and the offscreen Qt platform, so that nothing a child
// starts can show a window.
std::map<std::string, std::string> child_env(const std::map<std::string, std::string>& values);

// fiorivoce-cli --channel (or another program) on pipes, speaking JSON-RPC over them.
class ChannelProcess {
public:
    ChannelProcess(const std::filesystem::path& program, std::vector<std::string> args,
                   std::map<std::string, std::string> env);
    ~ChannelProcess();
    ChannelProcess(const ChannelProcess&) = delete;
    ChannelProcess& operator=(const ChannelProcess&) = delete;

    void send(const Json& message);
    // A request and its answer (by id; notifications that arrive meanwhile are kept).
    Json request(const std::string& method, std::optional<Json> params = std::nullopt,
                 std::optional<Json> id = std::nullopt, std::chrono::milliseconds timeout = kWait);
    Json notification(const std::string& method, std::chrono::milliseconds timeout = kWait);
    // initialize on revision 2025-11-25 as Claude Code sends it, then notifications/initialized.
    Json handshake();
    // Closes standard input and waits for the exit code (nullopt: it did not end in time).
    std::optional<int> close_stdin(std::chrono::milliseconds timeout = kWait);
    std::string stderr_text() const;
    std::vector<std::string> raw_lines() const;
    std::uint32_t pid() const { return process_->pid(); }
    std::optional<int> exit_code() const { return process_->exit_code(); }

private:
    Json take(const std::function<bool(const Json&)>& predicate, std::chrono::milliseconds timeout);

    std::unique_ptr<Process> process_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string out_buffer_;
    std::deque<Json> lines_;
    std::vector<Json> unclaimed_;
    std::vector<std::string> raw_lines_;
    bool out_closed_ = false;
    std::string err_;
    int next_id_ = 100;
};

// fiorivoce.exe beside fiorivoce-cli (the forwarding tests); empty where the build has no app.
std::filesystem::path app_executable();
#ifdef __linux__
std::vector<std::string> mapped_modules(std::uint32_t pid);
#endif

// A port nothing listens on: bound once, then closed.
std::uint16_t closed_port();

} // namespace fv::clitest
