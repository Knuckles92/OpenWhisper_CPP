#include "log/rotating_file_sink.h"

#include <cerrno>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <system_error>

#include "log/detail/files.h"

namespace fv::log {

namespace {

std::size_t code_points(std::string_view utf8) {
    std::size_t count = 0;
    for (const char c : utf8) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::filesystem::path numbered(const std::filesystem::path& base, int index) {
    auto name = base;
    name += "." + std::to_string(index);
    return name;
}

} // namespace

struct RotatingFileSink::State {
    std::filesystem::path file;
    std::uintmax_t max_bytes = 0;
    int backup_count = 0;
    std::mutex mutex; // the writer runs one job at a time; this guards against a second writer
    std::optional<std::ofstream> stream;
    std::uintmax_t size = 0;

    void open() {
        errno = 0;
        std::ofstream out(file, std::ios::binary | std::ios::app);
        if (!out) {
            // The C runtime under std::ofstream leaves the reason in errno.
            const int code = errno != 0 ? errno : EIO;
            throw std::filesystem::filesystem_error("Cannot open the log file", file,
                                                    std::error_code(code, std::generic_category()));
        }
        std::error_code error;
        const auto existing = std::filesystem::file_size(file, error);
        size = error ? 0 : existing;
        stream = std::move(out);
    }

    // RotatingFileHandler.doRollover.
    void roll_over() {
        stream.reset();
        if (backup_count > 0) {
            for (int i = backup_count - 1; i > 0; --i) {
                const auto source = numbered(file, i);
                const auto destination = numbered(file, i + 1);
                if (std::filesystem::exists(source)) {
                    if (std::filesystem::exists(destination)) {
                        std::filesystem::remove(destination);
                    }
                    std::filesystem::rename(source, destination);
                }
            }
            const auto first = numbered(file, 1);
            if (std::filesystem::exists(first)) {
                std::filesystem::remove(first);
            }
            if (std::filesystem::exists(file)) {
                std::filesystem::rename(file, first);
            }
        }
        open();
    }

    void write_line(const std::string& line) {
        std::lock_guard lock(mutex);
        try {
            if (!stream) {
                open();
            }
            if (max_bytes > 0 && size > 0 && size + code_points(line) + 1 >= max_bytes) {
                roll_over();
            }
            const std::string bytes = detail::text_mode_bytes(line + "\n");
            stream->write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream->flush();
            size += bytes.size();
        } catch (const std::exception&) {
            // logging's handleError: the record is lost, and the next one reopens the file.
            stream.reset();
        }
    }
};

std::string_view level_name(Level level) {
    switch (level) {
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warning:
        return "WARNING";
    case Level::Error:
        return "ERROR";
    case Level::Critical:
        return "CRITICAL";
    }
    return "INFO";
}

std::string format_record(const Record& record, const std::chrono::time_zone* zone) {
    using namespace std::chrono;
    if (!zone) {
        zone = current_zone();
    }
    const auto local = zone->to_local(floor<milliseconds>(record.time));
    const auto seconds = floor<std::chrono::seconds>(local);
    const auto millis = (local - seconds).count();
    return std::format("{:%Y-%m-%d %H:%M:%S},{:03} - {} - {} - {}", seconds, millis,
                       level_name(record.level), record.logger, record.message);
}

RotatingFileSink::RotatingFileSink(std::filesystem::path file, LogWriter& writer,
                                   std::uintmax_t max_bytes, int backup_count,
                                   const std::chrono::time_zone* zone)
    : state_(std::make_shared<State>()), queue_(writer.queue()),
      zone_(zone ? zone : std::chrono::current_zone()) {
    state_->file = std::filesystem::absolute(file);
    state_->max_bytes = max_bytes;
    state_->backup_count = backup_count;
    std::lock_guard lock(state_->mutex);
    state_->open();
}

RotatingFileSink::~RotatingFileSink() = default;

void RotatingFileSink::write(const Record& record) {
    queue_.post([state = state_, line = format_record(record, zone_)] { state->write_line(line); });
}

void RotatingFileSink::flush(std::chrono::milliseconds budget) {
    queue_.flush(budget);
}

const std::filesystem::path& RotatingFileSink::file() const {
    return state_->file;
}

} // namespace fv::log
