#pragma once

#include <exception>
#include <initializer_list>
#include <mutex>
#include <set>
#include <source_location>
#include <string>
#include <string_view>

#include "base/log.h"

// meeting/capture_session.py where_it_failed and FailureLog: report a failure once, then log its
// repeats at debug until it recovers, for handlers on paths that run every second, every chunk or
// every block.
namespace fv::meeting {

// The exception's type and where it was handled, without its message:
// "std::runtime_error at engine.cpp:120 in on_chunk". For failures in code that handles
// transcript text, prompts or keys, whose messages could quote them. C++ keeps no stack of the
// throw, so the place is the handler's call site. "unknown error" without an exception.
std::string where_it_failed(const std::exception* error,
                            const std::source_location& where = std::source_location::current());
// The exception being handled (std::current_exception()), as above.
std::string where_it_failed(std::exception_ptr error,
                            const std::source_location& where = std::source_location::current());

// The type name of an exception, without MSVC's "class " and "struct " prefixes.
std::string exception_type_name(const std::exception& error);

// Affinity: thread-safe.
class FailureLog {
public:
    explicit FailureLog(log::Logger logger) : logger_(logger) {}

    // Logs the exception being handled; call only from a catch block. The first failure under
    // `key` is an ERROR record carrying the exception's type, with "; repeats are logged at debug
    // until it recovers" appended; later ones are DEBUG until recovered(key). type_only appends
    // " (<where_it_failed>)" and never the exception's message.
    void failed(std::string_view key, std::string_view message, bool type_only = false,
                std::source_location where = std::source_location::current());
    // Lets the next failure under any of the keys be reported again.
    void recovered(std::initializer_list<std::string_view> keys);
    void recovered(std::string_view key) { recovered({key}); }

private:
    log::Logger logger_;
    std::mutex mutex_;
    std::set<std::string, std::less<>> failing_;
};

} // namespace fv::meeting
