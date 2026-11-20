#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "base/log.h"

namespace fv::credentials_test {

// A fresh folder under the system temporary folder, removed on destruction.
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

void write_bytes(const std::filesystem::path& file, std::string_view bytes);

// Every log record while it lives, at every level (as pytest's caplog at DEBUG).
class LogCapture {
public:
    LogCapture();
    ~LogCapture();
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    // "LEVEL logger message" lines, one per record.
    std::string text() const;
    bool contains(std::string_view needle) const {
        return text().find(needle) != std::string::npos;
    }

private:
    class Sink;
    std::shared_ptr<Sink> sink_;
    log::Level previous_;
};

} // namespace fv::credentials_test
