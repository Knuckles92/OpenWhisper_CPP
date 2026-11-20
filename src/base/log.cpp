#include "base/log.h"

#include "base/detail/pytext.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <typeinfo>
#include <vector>

#if defined(__GNUG__)
#include <cstdlib>
#include <cxxabi.h>
#endif

namespace fv::log {

namespace {

using SinkList = std::vector<std::shared_ptr<Sink>>;

struct Registry {
    std::mutex mutex;
    std::shared_ptr<const SinkList> sinks = std::make_shared<SinkList>();
    std::vector<std::pair<std::string, Level>> logger_levels;
    std::atomic<int> root_level{static_cast<int>(Level::Info)};
    std::atomic<bool> has_logger_levels{false};
};

Registry& registry() {
    static Registry instance;
    return instance;
}

std::shared_ptr<const SinkList> sinks_snapshot() {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    return r.sinks;
}

bool covers(std::string_view configured, std::string_view logger) {
    return logger == configured ||
           (logger.size() > configured.size() && logger.starts_with(configured) &&
            logger[configured.size()] == '.');
}

Level effective_level(std::string_view logger) {
    auto& r = registry();
    if (r.has_logger_levels.load()) {
        std::lock_guard lock(r.mutex);
        const std::pair<std::string, Level>* best = nullptr;
        for (const auto& entry : r.logger_levels) {
            if (covers(entry.first, logger) && (!best || entry.first.size() > best->first.size())) {
                best = &entry;
            }
        }
        if (best) {
            return best->second;
        }
    }
    return static_cast<Level>(r.root_level.load());
}

// The exception's class as C++ spells it, without MSVC's "class " or "struct ".
std::string type_name(const std::exception& error) {
    const char* raw = typeid(error).name();
#if defined(__GNUG__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
    std::string name = status == 0 && demangled ? demangled : raw;
    std::free(demangled);
    return name;
#else
    std::string_view name = raw;
    for (std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.remove_prefix(prefix.size());
        }
    }
    return std::string(name);
#endif
}

void deliver(const Record& record) {
    const auto sinks = sinks_snapshot();
    for (const auto& sink : *sinks) {
        try {
            sink->write(record);
        } catch (...) {
            // A failing sink must not take the caller down or stop the other sinks.
        }
    }
}

} // namespace

void add_sink(std::shared_ptr<Sink> sink) {
    if (!sink) {
        return;
    }
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    auto next = std::make_shared<SinkList>(*r.sinks);
    next->push_back(std::move(sink));
    r.sinks = std::move(next);
}

void remove_sink(const std::shared_ptr<Sink>& sink) {
    auto& r = registry();
    std::shared_ptr<const SinkList> previous;
    {
        std::lock_guard lock(r.mutex);
        auto next = std::make_shared<SinkList>(*r.sinks);
        std::erase(*next, sink);
        previous = std::exchange(r.sinks, std::move(next));
    }
}

void set_level(Level level) {
    registry().root_level.store(static_cast<int>(level));
}

Level level() {
    return static_cast<Level>(registry().root_level.load());
}

void set_logger_level(std::string_view logger, std::optional<Level> level) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    std::erase_if(r.logger_levels, [&](const auto& entry) { return entry.first == logger; });
    if (level) {
        r.logger_levels.emplace_back(std::string(logger), *level);
    }
    r.has_logger_levels.store(!r.logger_levels.empty());
}

bool enabled(Level level, std::string_view logger) {
    return static_cast<int>(level) >= static_cast<int>(effective_level(logger));
}

Level level_from_env(const EnvReader& env, const AppIdentity& app) {
    const auto value = env.get(std::string(app.env_prefix) + "LOG_LEVEL");
    const std::string name = ::fv::detail::ascii_upper(value.value_or("INFO"));
    if (name == "DEBUG" || name == "NOTSET") {
        return Level::Debug;
    }
    if (name == "WARNING" || name == "WARN") {
        return Level::Warning;
    }
    if (name == "ERROR") {
        return Level::Error;
    }
    if (name == "CRITICAL" || name == "FATAL") {
        return Level::Critical;
    }
    return Level::Info;
}

void flush(std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    const auto sinks = sinks_snapshot();
    for (const auto& sink : *sinks) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        try {
            sink->flush(std::max(left, std::chrono::milliseconds(0)));
        } catch (...) {
        }
    }
}

namespace detail {

void dispatch(Level level, std::string_view logger, std::string_view message,
              const std::source_location& where, const std::exception* error) {
    std::string exception_type;
    if (error) {
        exception_type = type_name(*error);
    }
    const Record record{
        .level = level,
        .logger = logger,
        .message = message,
        .exception_type = exception_type,
        .line = where.line(),
        .time = std::chrono::system_clock::now(),
        .exception = error,
        .file = where.file_name(),
    };
    deliver(record);
}

} // namespace detail

void Logger::error_with(const std::exception& error, std::string_view message,
                        std::source_location where) const {
    if (!enabled(Level::Error, name_)) {
        return;
    }
    detail::dispatch(Level::Error, name_, message, where, &error);
}

} // namespace fv::log
