#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "process/launcher.h"

namespace fv {

// What a FakeProcess does once started.
struct FakeProcessScript {
    std::string stdout_bytes; // handed to the stdout sink at once, then its end
    std::string stderr_bytes;
    std::optional<int> exit_code = 0;      // nullopt: runs until killed
    std::chrono::milliseconds runs_for{0}; // after the start, before it exits
    std::string start_error;               // non-empty: start() fails with it
    int killed_exit_code = 1;
};

// What a FakeProcess was told, readable after the Process itself is gone.
struct FakeProcessRecord {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::string stdin_data;
    bool stdin_closed = false;
    bool killed = false;
    bool disowned = false;
    std::optional<std::chrono::milliseconds> released;
    int kill_calls = 0;
};

// A scripted child: output delivered on the caller's thread when a sink is installed, an exit
// after `runs_for`, or never until kill_tree.
// Affinity: thread-safe.
class FakeProcess final : public Process {
public:
    FakeProcess(std::uint32_t pid, FakeProcessScript script,
                std::shared_ptr<FakeProcessRecord> record)
        : pid_(pid), script_(std::move(script)), record_(std::move(record)),
          started_(std::chrono::steady_clock::now()) {}

    std::uint32_t pid() const override { return pid_; }

    void write_stdin(std::string_view data) override {
        std::lock_guard lock(record_->mutex);
        if (!record_->stdin_closed) {
            record_->stdin_data.append(data);
        }
    }

    void close_stdin() override {
        std::lock_guard lock(record_->mutex);
        record_->stdin_closed = true;
    }

    void on_line(LineSink stdout_sink, LineSink stderr_sink) override {
        deliver_lines(script_.stdout_bytes, stdout_sink);
        deliver_lines(script_.stderr_bytes, stderr_sink);
    }

    void on_bytes(ByteSink stdout_sink, ByteSink stderr_sink) override {
        for (auto [bytes, sink] : {std::pair{&script_.stdout_bytes, &stdout_sink},
                                   std::pair{&script_.stderr_bytes, &stderr_sink}}) {
            if (*sink) {
                if (!bytes->empty()) {
                    (*sink)(std::span<const char>(bytes->data(), bytes->size()));
                }
                (*sink)(std::span<const char>());
            }
        }
    }

    std::optional<int> wait(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(record_->mutex);
        // now() + milliseconds::max() would overflow the clock's nanoseconds: a wait longer than
        // a day lasts until the exit or a kill.
        constexpr std::chrono::milliseconds longest = std::chrono::hours(24);
        const bool forever = timeout >= longest;
        const auto until = std::chrono::steady_clock::now() + (forever ? longest : timeout);
        for (;;) {
            if (auto code = exit_code_locked()) {
                return code;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!forever && now >= until) {
                return std::nullopt;
            }
            auto next = forever ? now + longest : until;
            if (script_.exit_code) {
                next = std::min(next, started_ + script_.runs_for);
            }
            record_->changed.wait_until(lock, next);
        }
    }

    std::optional<int> exit_code() const override {
        std::lock_guard lock(record_->mutex);
        return exit_code_locked();
    }

    void kill_tree() override {
        {
            std::lock_guard lock(record_->mutex);
            ++record_->kill_calls;
            if (!exit_code_locked()) {
                record_->killed = true;
            }
        }
        record_->changed.notify_all();
    }

    void disown() override {
        std::lock_guard lock(record_->mutex);
        record_->disowned = true;
    }

    void release_pipes(std::chrono::milliseconds grace) override {
        std::lock_guard lock(record_->mutex);
        record_->released = grace;
    }

private:
    std::optional<int> exit_code_locked() const {
        if (record_->killed) {
            return script_.killed_exit_code;
        }
        if (script_.exit_code && std::chrono::steady_clock::now() >= started_ + script_.runs_for) {
            return script_.exit_code;
        }
        return std::nullopt;
    }

    static void deliver_lines(const std::string& bytes, const LineSink& sink) {
        if (!sink) {
            return;
        }
        std::size_t start = 0;
        while (start < bytes.size()) {
            auto end = bytes.find('\n', start);
            std::string_view line(bytes.data() + start,
                                  (end == std::string::npos ? bytes.size() : end) - start);
            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }
            sink(line);
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    std::uint32_t pid_;
    FakeProcessScript script_;
    std::shared_ptr<FakeProcessRecord> record_;
    std::chrono::steady_clock::time_point started_;
};

// Starts FakeProcesses from queued scripts (an exit code 0 with no output when none is queued)
// and records every spec it was given.
// Affinity: thread-safe.
class FakeProcessLauncher final : public ProcessLauncher {
public:
    void push(FakeProcessScript script) {
        std::lock_guard lock(mutex_);
        scripts_.push_back(std::move(script));
    }

    std::expected<std::unique_ptr<Process>, std::string> start(const ProcessSpec& spec) override {
        std::lock_guard lock(mutex_);
        specs_.push_back(spec);
        FakeProcessScript script;
        if (!scripts_.empty()) {
            script = std::move(scripts_.front());
            scripts_.pop_front();
        }
        if (!script.start_error.empty()) {
            return std::unexpected(script.start_error);
        }
        auto record = std::make_shared<FakeProcessRecord>();
        records_.push_back(record);
        return std::make_unique<FakeProcess>(next_pid_++, std::move(script), std::move(record));
    }

    std::vector<ProcessSpec> specs() const {
        std::lock_guard lock(mutex_);
        return specs_;
    }
    std::vector<std::shared_ptr<FakeProcessRecord>> records() const {
        std::lock_guard lock(mutex_);
        return records_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<FakeProcessScript> scripts_;
    std::vector<ProcessSpec> specs_;
    std::vector<std::shared_ptr<FakeProcessRecord>> records_;
    std::uint32_t next_pid_ = 4242;
};

} // namespace fv
