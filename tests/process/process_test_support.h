#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "base/utf8.h"
#include "process/launch.h"
#include "testkit/testkit.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fv::process_test {

// fv_process_child, the program the [platform] tests start (commands in its header comment).
inline std::string child() {
    return FV_PROCESS_CHILD;
}

// argv for fv_process_child running `commands`.
inline std::vector<std::string> child_argv(std::vector<std::string> commands) {
    commands.insert(commands.begin(), child());
    return commands;
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

inline void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// tests/test_automation_hooks_programs.py wait_until: polls every 20 ms; false when the time
// ran out.
inline bool wait_until(const std::function<bool()>& predicate,
                       std::chrono::duration<double> timeout = std::chrono::seconds(15)) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

inline std::optional<std::uint32_t> read_pid(const std::filesystem::path& file) {
    if (!wait_until([&] { return std::filesystem::exists(file); })) {
        return std::nullopt;
    }
    const auto text = read_file(file);
    if (text.empty() || text == "0") {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::stoul(text));
}

#ifdef _WIN32
// tests/test_agent_process.py _alive.
inline bool alive(std::uint32_t pid) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool running = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return running;
}

// Ends a helper a failed test left behind, so no process outlives the run.
inline void kill_pid(std::uint32_t pid) {
    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (handle != nullptr) {
        TerminateProcess(handle, 1);
        CloseHandle(handle);
    }
}

// Kills `pid` when the scope ends, whatever the test did.
class PidGuard {
public:
    PidGuard() = default;
    ~PidGuard() {
        for (const auto pid : pids_) {
            if (alive(pid)) {
                kill_pid(pid);
            }
        }
    }
    PidGuard(const PidGuard&) = delete;
    PidGuard& operator=(const PidGuard&) = delete;
    void add(std::uint32_t pid) { pids_.push_back(pid); }

private:
    std::vector<std::uint32_t> pids_;
};
#endif

// A file whose contents stop changing: the acceptance check that no helper is still writing.
inline bool stops_changing(const std::filesystem::path& file,
                           std::chrono::milliseconds quiet = std::chrono::milliseconds(300)) {
    const auto before = std::filesystem::exists(file) ? read_file(file) : std::string();
    std::this_thread::sleep_for(quiet);
    const auto after = std::filesystem::exists(file) ? read_file(file) : std::string();
    return before == after;
}

// Installs a launch backend for the test's lifetime, and the real one after.
class ScopedLaunchBackend {
public:
    explicit ScopedLaunchBackend(std::shared_ptr<LaunchBackend> backend) {
        set_launch_backend(std::move(backend));
    }
    ~ScopedLaunchBackend() { set_launch_backend(nullptr); }
    ScopedLaunchBackend(const ScopedLaunchBackend&) = delete;
    ScopedLaunchBackend& operator=(const ScopedLaunchBackend&) = delete;
};

// Sets a variable of this process for the scope, and puts the previous value back.
class ScopedEnv {
public:
    ScopedEnv(std::string name, std::optional<std::string> value)
        : name_(std::move(name)), previous_(testkit::get_env(name_)) {
        testkit::set_env(name_, value);
    }
    ~ScopedEnv() { testkit::set_env(name_, previous_); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

} // namespace fv::process_test
