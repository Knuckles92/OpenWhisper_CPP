#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "base/env.h"
#include "base/identity.h"

namespace fv::log {

enum class Level { Debug, Info, Warning, Error, Critical };

// Every view is valid only during Sink::write.
struct Record {
    Level level;
    std::string_view logger;
    std::string_view message;
    std::string_view exception_type; // "std::runtime_error"; empty without an exception
    std::uint32_t line = 0;          // of the log call (the failure journal's `line`)
    std::chrono::system_clock::time_point time{};
    const std::exception* exception = nullptr; // for mapping C++ types to Python's categories
    std::string_view file;                     // of the log call, as the compiler spells it
};

// Implemented in fv_log: rotating file, failure journal, console, metrics.
// Affinity: called synchronously on the thread that logs, in registration order, never under a
// facade lock. A sink that does I/O queues the record and writes it on its own thread ("fv-log").
class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const Record&) = 0;
    // Writes out what is queued, waiting at most the budget.
    virtual void flush(std::chrono::milliseconds /*budget*/) {}
};

void add_sink(std::shared_ptr<Sink>);
void remove_sink(const std::shared_ptr<Sink>&);

// The root level (Python's basicConfig level); INFO until set.
void set_level(Level);
Level level();
// A level for one dotted logger and its children ("httpx" covers "httpx.client"), as Python's
// logging.getLogger(name).setLevel; nullopt removes it.
void set_logger_level(std::string_view logger, std::optional<Level>);
bool enabled(Level, std::string_view logger);
// <env_prefix>LOG_LEVEL as Python's getattr(logging, value.upper(), logging.INFO): DEBUG, INFO,
// WARNING, WARN, ERROR, CRITICAL, FATAL; NOTSET logs everything (Debug); anything else is INFO.
Level level_from_env(const EnvReader&, const AppIdentity&);
// Flushes every sink, together within the budget.
void flush(std::chrono::milliseconds budget);

// A format string that also records where the log call is. Built implicitly from the string
// literal, so calls read logger.info("Loaded {} models", count).
template <class... A> struct FormatAt {
    template <class S>
        requires std::convertible_to<const S&, std::string_view>
    consteval FormatAt(const S& text, std::source_location where = std::source_location::current())
        : format(text), where(where) {}

    std::format_string<A...> format;
    std::source_location where;
};

namespace detail {
void dispatch(Level, std::string_view logger, std::string_view message,
              const std::source_location& where, const std::exception* error);
} // namespace detail

// Dictated text, selections, prompts, keys and URLs with queries never reach a log call.
// Affinity: thread-safe; callers never block on stalled storage.
class Logger { // name = Python's dotted logger ("services.recorder")
public:
    explicit constexpr Logger(std::string_view name) : name_(name) {}
    constexpr std::string_view name() const { return name_; }

    template <class... A> void debug(FormatAt<std::type_identity_t<A>...>, A&&...) const;
    template <class... A> void info(FormatAt<std::type_identity_t<A>...>, A&&...) const;
    template <class... A> void warning(FormatAt<std::type_identity_t<A>...>, A&&...) const;
    template <class... A> void error(FormatAt<std::type_identity_t<A>...>, A&&...) const;
    template <class... A> void critical(FormatAt<std::type_identity_t<A>...>, A&&...) const;
    // An ERROR record carrying the exception's type (the failure journal's category). The message
    // is logged as given: the exception's what() may hold user text and is not added.
    void error_with(const std::exception&, std::string_view message,
                    std::source_location where = std::source_location::current()) const;

private:
    template <class... A>
    void write(Level level, const FormatAt<std::type_identity_t<A>...>& format, A&&... args) const {
        if (!enabled(level, name_)) {
            return;
        }
        detail::dispatch(level, name_, std::format(format.format, std::forward<A>(args)...),
                         format.where, nullptr);
    }

    std::string_view name_;
};

template <class... A>
void Logger::debug(FormatAt<std::type_identity_t<A>...> format, A&&... args) const {
    write<A...>(Level::Debug, format, std::forward<A>(args)...);
}
template <class... A>
void Logger::info(FormatAt<std::type_identity_t<A>...> format, A&&... args) const {
    write<A...>(Level::Info, format, std::forward<A>(args)...);
}
template <class... A>
void Logger::warning(FormatAt<std::type_identity_t<A>...> format, A&&... args) const {
    write<A...>(Level::Warning, format, std::forward<A>(args)...);
}
template <class... A>
void Logger::error(FormatAt<std::type_identity_t<A>...> format, A&&... args) const {
    write<A...>(Level::Error, format, std::forward<A>(args)...);
}
template <class... A>
void Logger::critical(FormatAt<std::type_identity_t<A>...> format, A&&... args) const {
    write<A...>(Level::Critical, format, std::forward<A>(args)...);
}

} // namespace fv::log
