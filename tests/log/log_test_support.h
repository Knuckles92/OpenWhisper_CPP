#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/json.h"
#include "base/log.h"

namespace fv::logtest {

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

std::string read_file(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, std::string_view bytes);

// The lines of a text file without their line endings ("\n" or "\r\n").
std::vector<std::string> read_lines(const std::filesystem::path& path);

// Runs a program without a window and waits for it; its exit code, or nullopt on timeout (the
// process is then killed). On Windows the code is the process's DWORD exit code.
std::optional<std::uint32_t> run_child(const std::filesystem::path& program,
                                       const std::vector<std::string>& args,
                                       std::chrono::milliseconds timeout);

// A golden-vector value with {"$float": "nan" | "inf" | "-inf"} turned back into floats.
Json untag(const Json& value);

// Records every log record while it lives, then puts the facade back.
class CaptureSink final : public log::Sink {
public:
    struct Entry {
        log::Level level;
        std::string logger;
        std::string message;
    };
    void write(const log::Record& record) override;
    std::vector<Entry> entries() const;
    std::string text() const; // every message, one per line

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

class CapturedLog {
public:
    CapturedLog();
    ~CapturedLog();
    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;
    CaptureSink& sink() { return *sink_; }

private:
    std::shared_ptr<CaptureSink> sink_;
    log::Level previous_;
};

} // namespace fv::logtest
