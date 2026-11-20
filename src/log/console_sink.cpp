#include "log/console_sink.h"

#include <string>
#include <utility>

#include "log/detail/files.h"
#include "log/rotating_file_sink.h"

namespace fv::log {

ConsoleSink::ConsoleSink(LogWriter& writer, std::function<void(std::string_view)> out,
                         const std::chrono::time_zone* zone)
    : queue_(writer.queue()),
      out_(out ? std::move(out) : [](std::string_view text) { detail::write_stderr(text); }),
      zone_(zone ? zone : std::chrono::current_zone()) {}

void ConsoleSink::write(const Record& record) {
    queue_.post([out = out_, line = format_record(record, zone_) + "\n"] { out(line); });
}

void ConsoleSink::flush(std::chrono::milliseconds budget) {
    queue_.flush(budget);
}

} // namespace fv::log
