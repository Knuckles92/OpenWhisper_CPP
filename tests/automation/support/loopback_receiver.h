#pragma once

// A webhook and filter receiver on 127.0.0.1 for the hook engine tests (the port of
// tests/test_automation_hooks_webhooks.py Receiver), on net::HttpServer: it answers POSTs per
// path from a script, records what arrived, and can hold or delay an answer. Nothing here leaves
// the loopback interface.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "base/json.h"
#include "net/http_server.h"
#include "net/reactor.h"

namespace fv::automation::test {

// Released by the test; an answer that holds on it waits for it (at most 10 s).
class Gate {
public:
    void set() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        opened_.notify_all();
    }
    // True once set; false when `timeout` passed first.
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return opened_.wait_for(lock, timeout, [&] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable opened_;
    bool open_ = false;
};

struct ReceivedRequest {
    std::string path;
    std::map<std::string, std::string> headers; // names in lower case, the first of each
    std::string body;
    std::chrono::steady_clock::time_point at; // when it arrived, before any hold or delay

    Json json() const { return Json::parse(body); }
    std::string header(const std::string& name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? std::string() : found->second;
    }
};

struct Answer {
    int status = 200;
    net::Headers headers;
    std::string body;
    // Waited before answering (ended early when the receiver closes).
    std::chrono::milliseconds delay{0};
    std::shared_ptr<Gate> hold;
    // Builds the body from the request, instead of `body`.
    std::function<std::string(const ReceivedRequest&)> reply;
};

class Receiver {
public:
    Receiver() : reactor_(2), server_(net::HttpServer::create(reactor_)) {
        server_->set_fallback([this](const net::HttpRequest& request, bool,
                                     std::shared_ptr<net::Responder> responder) {
            arrived(request, std::move(responder));
        });
        const auto endpoint = server_->listen({.address = "127.0.0.1", .port = 0});
        if (!endpoint) {
            throw std::runtime_error("the receiver could not listen: " + endpoint.error());
        }
        port_ = endpoint->port;
    }
    ~Receiver() { close(); }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    std::uint16_t port() const { return port_; }
    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    // The first request to `path` gets answers[0], the second answers[1], and every later one
    // the last answer.
    void answer(const std::string& path, std::vector<Answer> answers) {
        std::lock_guard lock(mutex_);
        answers_[path] = std::move(answers);
    }

    // Answered requests, in the order they were answered.
    std::vector<ReceivedRequest> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    std::vector<ReceivedRequest> at(std::string_view path) const {
        std::lock_guard lock(mutex_);
        std::vector<ReceivedRequest> found;
        for (const auto& request : requests_) {
            if (request.path == path) {
                found.push_back(request);
            }
        }
        return found;
    }
    // Paths of requests as they arrive, before any delay or hold.
    std::vector<std::string> arrivals() const {
        std::lock_guard lock(mutex_);
        return arrivals_;
    }
    bool arrived_at(std::string_view path) const {
        const auto paths = arrivals();
        return std::ranges::find(paths, path) != paths.end();
    }

    // At least `count` answered requests to `path` (any path when empty); throws when they do
    // not come in time.
    std::vector<ReceivedRequest>
    wait_for(std::size_t count, std::string_view path = {},
             std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
        std::unique_lock lock(mutex_);
        std::vector<ReceivedRequest> got;
        const bool done = changed_.wait_for(lock, timeout, [&] {
            got.clear();
            for (const auto& request : requests_) {
                if (path.empty() || request.path == path) {
                    got.push_back(request);
                }
            }
            return got.size() >= count;
        });
        if (!done) {
            throw std::runtime_error("expected " + std::to_string(count) + " requests at " +
                                     std::string(path) + ", got " + std::to_string(got.size()));
        }
        return got;
    }

    void close() {
        std::list<std::thread> workers;
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            closing_ = true;
            workers.swap(workers_);
        }
        changed_.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
        server_->stop(std::chrono::milliseconds(200));
        reactor_.stop(std::chrono::milliseconds(200));
    }

private:
    Answer next_answer(const std::string& path) {
        std::lock_guard lock(mutex_);
        auto found = answers_.find(path);
        if (found == answers_.end() || found->second.empty()) {
            return Answer{};
        }
        auto& queue = found->second;
        if (queue.size() > 1) {
            Answer first = std::move(queue.front());
            queue.erase(queue.begin());
            return first;
        }
        return queue.front();
    }

    // Runs on the reactor: records the arrival and answers from a thread of its own, so a held
    // or delayed answer never blocks the server.
    void arrived(const net::HttpRequest& request, std::shared_ptr<net::Responder> responder) {
        ReceivedRequest received;
        received.path = request.path;
        for (const auto& [name, value] : request.headers) {
            std::string lower = name;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            received.headers.emplace(std::move(lower), value);
        }
        received.body = request.body;
        received.at = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        arrivals_.push_back(received.path);
        changed_.notify_all();
        if (closing_) {
            responder->send(503, {}, "");
            return;
        }
        workers_.emplace_back(
            [this, received = std::move(received), responder = std::move(responder)]() mutable {
                serve(std::move(received), std::move(responder));
            });
    }

    void serve(ReceivedRequest received, std::shared_ptr<net::Responder> responder) {
        Answer answer = next_answer(received.path);
        if (answer.hold) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!answer.hold->wait(std::chrono::milliseconds(50)) && !closing() &&
                   std::chrono::steady_clock::now() < until) {
            }
        }
        if (answer.delay.count() > 0) {
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, answer.delay, [&] { return closing_; });
        }
        const std::string body = answer.reply ? answer.reply(received) : answer.body;
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(received);
        }
        changed_.notify_all();
        responder->send(answer.status, answer.headers, body);
    }

    bool closing() const {
        std::lock_guard lock(mutex_);
        return closing_;
    }

    net::Reactor reactor_;
    std::unique_ptr<net::HttpServer> server_;
    std::uint16_t port_ = 0;

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::map<std::string, std::vector<Answer>> answers_;
    std::vector<ReceivedRequest> requests_;
    std::vector<std::string> arrivals_;
    std::list<std::thread> workers_;
    bool closing_ = false;
    bool closed_ = false;
};

} // namespace fv::automation::test
