#include "test_support.h"

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <system_error>

namespace fv::credentials_test {

TempDir::TempDir() {
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            std::format("fv-credentials-{}-{}", stamp, counter.fetch_add(1));
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

void write_bytes(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

class LogCapture::Sink final : public log::Sink {
public:
    void write(const log::Record& record) override {
        std::lock_guard lock(mutex_);
        text_ += std::format("{} {} {}\n", static_cast<int>(record.level), record.logger,
                             record.message);
    }

    std::string text() const {
        std::lock_guard lock(mutex_);
        return text_;
    }

private:
    mutable std::mutex mutex_;
    std::string text_;
};

LogCapture::LogCapture() : sink_(std::make_shared<Sink>()), previous_(log::level()) {
    log::set_level(log::Level::Debug);
    log::add_sink(sink_);
}

LogCapture::~LogCapture() {
    log::remove_sink(sink_);
    log::set_level(previous_);
}

std::string LogCapture::text() const {
    return sink_->text();
}

} // namespace fv::credentials_test
