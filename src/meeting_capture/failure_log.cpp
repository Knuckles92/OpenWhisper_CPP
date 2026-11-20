#include "meeting_capture/failure_log.h"

#include <format>
#include <typeinfo>

#if defined(__GNUG__)
#include <cstdlib>
#include <cxxabi.h>
#endif

namespace fv::meeting {

namespace {

std::string_view file_basename(std::string_view file) {
    const std::size_t slash = file.find_last_of("/\\");
    return slash == std::string_view::npos ? file : file.substr(slash + 1);
}

// The unqualified function name out of std::source_location's spelling, which MSVC and GCC give
// with the return type, the calling convention, the scopes and the parameters.
std::string_view function_basename(std::string_view signature) {
    std::size_t open = signature.find('(');
    if (open == std::string_view::npos) {
        return signature;
    }
    std::string_view head = signature.substr(0, open);
    while (!head.empty() && head.back() == ' ') {
        head.remove_suffix(1);
    }
    if (head.ends_with("operator")) {
        // operator()(...): the first parenthesis pair is the operator's name.
        return "operator()";
    }
    const std::size_t scope = head.rfind("::");
    if (scope != std::string_view::npos) {
        head = head.substr(scope + 2);
    }
    const std::size_t space = head.rfind(' ');
    if (space != std::string_view::npos) {
        head = head.substr(space + 1);
    }
    return head;
}

// Calls fn with the std::exception `error` holds (null for none or another type), inside the
// handler: rethrow_exception may throw a copy that lives only as long as the catch block.
template <class Fn> void with_exception(const std::exception_ptr& error, Fn&& fn) {
    if (!error) {
        fn(nullptr);
        return;
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& exception) {
        fn(&exception);
    } catch (...) {
        fn(nullptr);
    }
}

} // namespace

std::string exception_type_name(const std::exception& error) {
    const char* raw = typeid(error).name();
#if defined(__GNUG__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        std::string name(demangled);
        std::free(demangled);
        return name;
    }
    return raw;
#else
    std::string_view name(raw);
    for (std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.remove_prefix(prefix.size());
        }
    }
    return std::string(name);
#endif
}

std::string where_it_failed(const std::exception* error, const std::source_location& where) {
    if (error == nullptr) {
        return "unknown error";
    }
    return std::format("{} at {}:{} in {}", exception_type_name(*error),
                       file_basename(where.file_name()), where.line(),
                       function_basename(where.function_name()));
}

std::string where_it_failed(std::exception_ptr error, const std::source_location& where) {
    std::string text;
    with_exception(
        error, [&](const std::exception* exception) { text = where_it_failed(exception, where); });
    return text;
}

void FailureLog::failed(std::string_view key, std::string_view message, bool type_only,
                        std::source_location where) {
    bool first = false;
    {
        const std::lock_guard lock(mutex_);
        first = failing_.emplace(key).second;
    }
    const log::Level level = first ? log::Level::Error : log::Level::Debug;
    if (!log::enabled(level, logger_.name())) {
        return;
    }
    std::string text(message);
    if (first) {
        text += "; repeats are logged at debug until it recovers";
    }
    with_exception(std::current_exception(), [&](const std::exception* error) {
        if (type_only) {
            text += std::format(" ({})", where_it_failed(error, where));
            log::detail::dispatch(level, logger_.name(), text, where, nullptr);
            return;
        }
        log::detail::dispatch(level, logger_.name(), text, where, error);
    });
}

void FailureLog::recovered(std::initializer_list<std::string_view> keys) {
    const std::lock_guard lock(mutex_);
    if (failing_.empty()) {
        return;
    }
    for (std::string_view key : keys) {
        if (const auto found = failing_.find(key); found != failing_.end()) {
            failing_.erase(found);
        }
    }
}

} // namespace fv::meeting
