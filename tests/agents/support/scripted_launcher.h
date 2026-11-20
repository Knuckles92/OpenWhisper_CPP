#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "base/utf8.h"
#include "process/launcher.h"

// A launcher whose children are scripted by command line: the agent tests' stand-in for the
// agents' --version, auth status and login status, and for an ACP agent on stdio. Nothing it
// starts is a real process.
namespace fv::agents_test {

// Holds a scripted reply back until opened.
class Gate {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }
    bool is_open() const {
        std::lock_guard lock(mutex_);
        return open_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

struct Reply {
    std::optional<int> code = 0; // nullopt: runs until killed
    std::string out;
    std::string err;
    std::shared_ptr<Gate> gate; // when set, nothing happens until it is opened (or a kill)
};

// A child that, once its output is asked for, waits for its gate, writes its output, then
// exits; or runs until killed.
// Affinity: thread-safe.
class ScriptedProcess final : public Process {
public:
    ScriptedProcess(std::uint32_t pid, Reply reply) : pid_(pid), reply_(std::move(reply)) {}
    ~ScriptedProcess() override {
        kill_tree();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::uint32_t pid() const override { return pid_; }
    void write_stdin(std::string_view) override {}
    void close_stdin() override {}
    void on_line(LineSink out, LineSink err) override {
        on_bytes(splitter(std::move(out)), splitter(std::move(err)));
    }
    void on_bytes(ByteSink out, ByteSink err) override {
        worker_ = std::thread([this, out = std::move(out), err = std::move(err)] {
            while (!killed_.load() && reply_.gate && !reply_.gate->is_open()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!killed_.load()) {
                deliver(out, reply_.out);
                deliver(err, reply_.err);
                if (reply_.code) {
                    finish(*reply_.code);
                }
            }
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return exit_.has_value(); });
            lock.unlock();
            end(out);
            end(err);
        });
    }
    std::optional<int> wait(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, timeout, [&] { return exit_.has_value(); });
        return exit_;
    }
    std::optional<int> exit_code() const override {
        std::lock_guard lock(mutex_);
        return exit_;
    }
    void kill_tree() override {
        killed_.store(true);
        finish(1);
    }
    void disown() override {}
    void release_pipes(std::chrono::milliseconds) override {}

private:
    static ByteSink splitter(LineSink sink) {
        auto buffer = std::make_shared<std::string>();
        return [buffer, sink = std::move(sink)](std::span<const char> bytes) {
            if (!sink) {
                return;
            }
            if (bytes.empty()) {
                if (!buffer->empty()) {
                    sink(*buffer);
                }
                return;
            }
            buffer->append(bytes.data(), bytes.size());
            for (auto end = buffer->find('\n'); end != std::string::npos;
                 end = buffer->find('\n')) {
                sink(std::string_view(*buffer).substr(0, end));
                buffer->erase(0, end + 1);
            }
        };
    }
    static void deliver(const ByteSink& sink, const std::string& bytes) {
        if (sink && !bytes.empty()) {
            sink(std::span<const char>(bytes.data(), bytes.size()));
        }
    }
    static void end(const ByteSink& sink) {
        if (sink) {
            sink(std::span<const char>());
        }
    }
    void finish(int code) {
        {
            std::lock_guard lock(mutex_);
            if (!exit_) {
                exit_ = code;
            }
        }
        changed_.notify_all();
    }

    std::uint32_t pid_;
    Reply reply_;
    std::atomic<bool> killed_{false};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<int> exit_;
    std::thread worker_;
};

// Answers a start by the program's full path and its arguments; an unscripted command fails to
// start ("The program wasn't found."). Records every spec.
// Affinity: thread-safe.
class ScriptedLauncher final : public ProcessLauncher {
public:
    using Factory = std::function<std::unique_ptr<Process>(const ProcessSpec&)>;

    void reply(const std::filesystem::path& program, const std::vector<std::string>& args,
               Reply reply) {
        std::lock_guard lock(mutex_);
        replies_[key(program, args)] = std::move(reply);
    }
    // Starts what `factory` makes for a spec it doesn't return null for (ACP agents).
    void set_factory(Factory factory) {
        std::lock_guard lock(mutex_);
        factory_ = std::move(factory);
    }

    std::expected<std::unique_ptr<Process>, std::string> start(const ProcessSpec& spec) override {
        Factory factory;
        std::optional<Reply> reply;
        {
            std::lock_guard lock(mutex_);
            specs_.push_back(spec);
            ++counts_[key(spec.program, spec.args)];
            factory = factory_;
            if (const auto it = replies_.find(key(spec.program, spec.args)); it != replies_.end()) {
                reply = it->second;
            }
        }
        if (factory) {
            if (auto process = factory(spec)) {
                return process;
            }
        }
        if (!reply) {
            return std::unexpected(std::string("The program wasn't found."));
        }
        return std::make_unique<ScriptedProcess>(next_pid_++, std::move(*reply));
    }

    int count(const std::filesystem::path& program, const std::vector<std::string>& args) const {
        std::lock_guard lock(mutex_);
        const auto it = counts_.find(key(program, args));
        return it == counts_.end() ? 0 : it->second;
    }
    std::vector<ProcessSpec> specs() const {
        std::lock_guard lock(mutex_);
        return specs_;
    }

    static std::string key(const std::filesystem::path& program,
                           const std::vector<std::string>& args) {
        auto text = path_to_utf8(std::filesystem::absolute(program).lexically_normal());
        std::transform(text.begin(), text.end(), text.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
        for (const auto& arg : args) {
            text += '\x1f';
            text += arg;
        }
        return text;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, Reply> replies_;
    std::map<std::string, int> counts_;
    std::vector<ProcessSpec> specs_;
    Factory factory_;
    std::atomic<std::uint32_t> next_pid_{7000};
};

} // namespace fv::agents_test
