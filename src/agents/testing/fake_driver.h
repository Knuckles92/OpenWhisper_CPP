#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agents/drivers.h"

namespace fv::agents {

// A driver that plays each pass by calling the pass's tools the way an agent would, then returns
// a set outcome (the FakeDriver and ReadingDriver of tests/test_installed_agent_core.py). It
// records every request, the personas it was started with, and whether it was closed.
// Affinity: thread-safe.
class FakeDriver final : public Driver {
public:
    struct Options {
        std::string name = "Fake agent";
        std::string tool_prefix;
        // Claude Code's: it can be fenced to a folder; supports_read_folder says whether this
        // install can.
        bool reads_folders = false;
        bool supports_read_folder = false;
        // (tool, arguments) the pass calls through tracked_handler, in order.
        std::vector<std::pair<std::string, Json>> calls;
        PassOutcome outcome{.ok = true, .usage = Json{{"total_tokens", 10}}};
    };

    FakeDriver() = default;
    explicit FakeDriver(Options options) : options_(std::move(options)) {}

    std::string_view name() const override { return options_.name; }
    void start(LoopbackMcpServer&, std::map<std::string, std::string> personas = {},
               const Json& tools = Json::array()) override {
        std::lock_guard lock(mutex_);
        personas_ = std::move(personas);
        start_tools_ = tools;
        started_ = true;
    }
    bool healthy() const override {
        std::lock_guard lock(mutex_);
        return !closed_;
    }
    void close() override {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    PassOutcome run_pass(const PassRequest& request) override {
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        request.emit("start");
        const auto handler = request.tracked_handler();
        for (const auto& [tool, args] : options_.calls) {
            handler(tool, args, Json::object());
        }
        request.emit("settled");
        return options_.outcome;
    }
    std::string tool_prefix() const override { return options_.tool_prefix; }
    bool reads_folders() const override { return options_.reads_folders; }
    bool supports_read_folder() const override { return options_.supports_read_folder; }

    std::vector<PassRequest> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    // nullopt until start().
    std::optional<std::map<std::string, std::string>> personas() const {
        std::lock_guard lock(mutex_);
        if (!started_) {
            return std::nullopt;
        }
        return personas_;
    }
    Json start_tools() const {
        std::lock_guard lock(mutex_);
        return start_tools_;
    }
    bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    Options options_;
    mutable std::mutex mutex_;
    std::vector<PassRequest> requests_;
    std::map<std::string, std::string> personas_;
    Json start_tools_ = Json::array();
    bool started_ = false;
    bool closed_ = false;
};

} // namespace fv::agents
