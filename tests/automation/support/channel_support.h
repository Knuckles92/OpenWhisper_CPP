#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "automation/channel_bridge.h"
#include "automation/channel_link.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/log.h"
#include "golden/golden.h"
#include "pyport/json.h"
#include "pyport/text.h"

// In-memory protocol streams and fakes for the Claude Code channel's tests
// (tests/test_automation_channel_protocol.py's _Pipe, _Output, _FakeLink and _FakeStream).
namespace fv::automation::channel_test {

// A blocking line reader fed by the test, like standard input.
class Pipe final : public LineReader {
public:
    void put(const Json& message) { put_line(py::dumps(message) + "\n"); }
    void put_line(std::string line) {
        std::lock_guard lock(mutex_);
        lines_.push_back(std::move(line));
        changed_.notify_all();
    }
    void close() {
        std::lock_guard lock(mutex_);
        lines_.push_back(std::nullopt);
        changed_.notify_all();
    }
    std::optional<std::string> readline() override {
        std::unique_lock lock(mutex_);
        ++reads_;
        changed_.notify_all();
        if (!changed_.wait_for(lock, std::chrono::seconds(10), [&] { return !lines_.empty(); })) {
            throw std::runtime_error("nothing to read");
        }
        std::optional<std::string> line = std::move(lines_.front());
        lines_.pop_front();
        return line;
    }
    // Waits until the server asked for line `count` (1-based), so every line before it has been
    // handled.
    bool wait_reads(std::size_t count,
                    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return reads_ >= count; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::optional<std::string>> lines_;
    std::size_t reads_ = 0;
};

// Collects what the server writes, a message per line.
class Output final : public LineWriter {
public:
    void write(std::string_view bytes) override {
        std::lock_guard lock(mutex_);
        if (broken) {
            throw std::runtime_error("broken pipe");
        }
        raw_ += bytes;
        changed_.notify_all();
    }
    void close() override {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    std::string raw() const {
        std::lock_guard lock(mutex_);
        return raw_;
    }
    bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }
    std::vector<std::string> lines() const {
        std::lock_guard lock(mutex_);
        return split(raw_);
    }
    // Waits for `count` lines and returns every line as JSON.
    std::vector<Json> messages(std::size_t count,
                               std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        const bool arrived = changed_.wait_for(lock, timeout, [&] {
            return static_cast<std::size_t>(std::count(raw_.begin(), raw_.end(), '\n')) >= count;
        });
        if (!arrived) {
            throw std::runtime_error("the server wrote fewer than " + std::to_string(count) +
                                     " lines");
        }
        std::vector<Json> out;
        for (const std::string& line : split(raw_)) {
            out.push_back(py::loads(line));
        }
        return out;
    }
    bool broken = false;

private:
    static std::vector<std::string> split(const std::string& raw) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start < raw.size()) {
            const std::size_t end = raw.find('\n', start);
            if (end == std::string::npos) {
                out.push_back(raw.substr(start));
                break;
            }
            out.push_back(raw.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string raw_;
    bool closed_ = false;
};

// An AppLink whose notify records the reply, or throws what the test set.
class FakeLink final : public AppLink {
public:
    FakeLink() : AppLink(quiet_options()) {}
    void notify(const std::string& text, const std::string& title) override {
        std::lock_guard lock(mutex_);
        if (fail) {
            fail(text);
        }
        sent_.emplace_back(text, title);
    }
    std::vector<std::pair<std::string, std::string>> sent() const {
        std::lock_guard lock(mutex_);
        return sent_;
    }
    std::function<void(const std::string& text)> fail;

    static AppLinkOptions quiet_options() {
        AppLinkOptions options;
        options.token = std::string(40, 't');
        options.read_settings = [] {
            return Json::object();
        };
        options.read_token = [] {
            return std::optional<std::string>();
        };
        return options;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, std::string>> sent_;
};

struct StreamCounts {
    std::atomic<int> started{0};
    std::atomic<int> stopped{0};
};

class FakeStream final : public ChannelStream {
public:
    explicit FakeStream(std::shared_ptr<StreamCounts> counts) : counts_(std::move(counts)) {}
    void start() override { ++counts_->started; }
    void stop() override { ++counts_->stopped; }

private:
    std::shared_ptr<StreamCounts> counts_;
};

// The server on a thread of its own, reading the pipe, as main() runs it.
class Harness {
public:
    Harness() {
        ChannelServerOptions options;
        options.version = "9.9.9";
        options.stream_factory = [this] {
            std::lock_guard lock(mutex_);
            streams_.push_back(std::make_shared<StreamCounts>());
            return std::make_unique<FakeStream>(streams_.back());
        };
        server = std::make_unique<ChannelServer>(in, out, link, std::move(options));
        thread_ = std::thread([this] { code_ = server->serve(); });
    }
    ~Harness() {
        if (thread_.joinable()) {
            finish();
        }
    }
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    int finish() {
        in.close();
        thread_.join();
        return code_;
    }
    std::vector<std::shared_ptr<StreamCounts>> streams() {
        std::lock_guard lock(mutex_);
        return streams_;
    }

    Pipe in;
    Output out;
    FakeLink link;
    std::unique_ptr<ChannelServer> server;

private:
    std::mutex mutex_;
    std::vector<std::shared_ptr<StreamCounts>> streams_;
    std::thread thread_;
    int code_ = -1;
};

// Every log record at INFO and above for its lifetime (caplog.set_level("INFO")).
class LogCapture {
public:
    struct Entry {
        log::Level level;
        std::string message;
    };

    LogCapture() : previous_(log::level()) {
        log::set_level(log::Level::Info);
        log::add_sink(sink_);
    }
    ~LogCapture() {
        log::remove_sink(sink_);
        log::set_level(previous_);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    std::vector<Entry> entries() const { return sink_->entries(); }
    std::string text() const {
        std::string out;
        for (const Entry& entry : entries()) {
            out += entry.message + "\n";
        }
        return out;
    }
    std::vector<std::string> warnings() const {
        std::vector<std::string> out;
        for (const Entry& entry : entries()) {
            if (entry.level == log::Level::Warning) {
                out.push_back(entry.message);
            }
        }
        return out;
    }

private:
    class Sink final : public log::Sink {
    public:
        void write(const log::Record& record) override {
            std::lock_guard lock(mutex_);
            entries_.push_back({record.level, std::string(record.message)});
        }
        std::vector<Entry> entries() const {
            std::lock_guard lock(mutex_);
            return entries_;
        }

    private:
        mutable std::mutex mutex_;
        std::vector<Entry> entries_;
    };

    std::shared_ptr<Sink> sink_ = std::make_shared<Sink>();
    log::Level previous_;
};

// OpenWhisper's names in a recorded string as OpenWhisper_CPP's: golden::rebrand, plus the product
// in lower case (the MCP server's name and the notices' origin).
inline std::string rebranded_text(std::string_view text) {
    const std::string legacy = py::lower(legacy_identity().product);
    const std::string current = py::lower(identity().product);
    std::string out = golden::rebrand(text);
    for (std::size_t at = out.find(legacy); at != std::string::npos;
         at = out.find(legacy, at + current.size())) {
        out.replace(at, legacy.size(), current);
    }
    return out;
}

} // namespace fv::automation::channel_test
