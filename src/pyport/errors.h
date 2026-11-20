#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace fv::py {

// The Python exception a pyport function stands in for. type_name() is the Python class name
// ("ValueError", "OverflowError", ...), which golden vectors record as "raises".
class PyError : public std::runtime_error {
public:
    PyError(std::string_view type_name, const std::string& message)
        : std::runtime_error(message), type_name_(type_name) {}
    std::string_view type_name() const noexcept { return type_name_; }

private:
    std::string_view type_name_; // always a string literal
};

class ValueError : public PyError {
public:
    explicit ValueError(const std::string& message) : PyError("ValueError", message) {}

protected:
    ValueError(std::string_view type_name, const std::string& message)
        : PyError(type_name, message) {}
};

class OverflowError : public PyError {
public:
    explicit OverflowError(const std::string& message) : PyError("OverflowError", message) {}
};

class RecursionError : public PyError {
public:
    explicit RecursionError(const std::string& message) : PyError("RecursionError", message) {}
};

class IndexError : public PyError {
public:
    explicit IndexError(const std::string& message) : PyError("IndexError", message) {}
};

class TypeError : public PyError {
public:
    explicit TypeError(const std::string& message) : PyError("TypeError", message) {}
};

} // namespace fv::py
