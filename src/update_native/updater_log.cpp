#include "update_native/updater_log.h"

#include <chrono>
#include <exception>
#include <system_error>

#include "base/identity.h"
#include "base/paths.h"
#include "log/rotating_file_sink.h"
#include "log/writer.h"

namespace fv::update_native {

using namespace std::chrono_literals;

struct UpdaterLog::Parts {
    std::unique_ptr<log::LogWriter> writer;
    std::shared_ptr<log::RotatingFileSink> sink;
    log::Level previous_level = log::Level::Info;
};

std::filesystem::path updater_log_path(const EnvReader& env) {
    try {
        return Paths::resolve(identity(), env).local_app_dir() / "updater.log";
    } catch (const std::exception&) {
        return {};
    }
}

UpdaterLog::UpdaterLog(const std::filesystem::path& file) {
    if (file.empty()) {
        return;
    }
    auto parts = std::make_unique<Parts>();
    try {
        std::error_code ignored;
        std::filesystem::create_directories(file.parent_path(), ignored);
        parts->writer = std::make_unique<log::LogWriter>();
        parts->sink = std::make_shared<log::RotatingFileSink>(
            file, *parts->writer, kUpdaterLogMaxBytes, kUpdaterLogBackupCount);
    } catch (const std::exception&) {
        // setup_updater_logging returns on OSError: the update goes on without a log.
        return;
    }
    parts->previous_level = log::level();
    log::set_level(log::Level::Info);
    log::add_sink(parts->sink);
    parts_ = std::move(parts);
}

UpdaterLog::~UpdaterLog() {
    if (!parts_) {
        return;
    }
    parts_->sink->flush(1s);
    log::remove_sink(parts_->sink);
    log::set_level(parts_->previous_level);
    parts_->writer->stop(1s);
}

bool UpdaterLog::active() const {
    return parts_ != nullptr;
}

} // namespace fv::update_native
