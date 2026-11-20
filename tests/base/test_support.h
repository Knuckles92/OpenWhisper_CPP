#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fv::test {

// A fresh folder under the system's temporary folder, removed with what it holds.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// The working directory, restored on destruction.
class ScopedCwd {
public:
    explicit ScopedCwd(const std::filesystem::path& to);
    ~ScopedCwd();
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;

private:
    std::filesystem::path previous_;
};

// A child process with pipes on its standard input and output and no console window, as the
// reader process of test_backup_startup.py.
class ChildProcess {
public:
    ChildProcess(const std::filesystem::path& program, const std::vector<std::string>& args);
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // One line of the child's output without its line ending; nullopt at end of output or when
    // the time is up.
    std::optional<std::string> read_line(std::chrono::milliseconds timeout);
    void close_stdin();
    // The exit code; nullopt when the time is up.
    std::optional<int> wait(std::chrono::milliseconds timeout);
    void kill();

private:
    std::intptr_t process_ = -1; // HANDLE on Windows, pid elsewhere
    std::intptr_t stdin_ = -1;
    std::intptr_t stdout_ = -1;
    std::string buffer_;
    std::optional<int> exit_code_;
};

} // namespace fv::test
