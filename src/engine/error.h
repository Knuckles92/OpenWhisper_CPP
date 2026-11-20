#pragma once

#include <optional>
#include <stdexcept>
#include <string>

namespace fv {

// Anything the engine layer cannot do: a missing runtime or model, unreadable
// audio, or an error reported by the speech runtime. what() is a sentence fit
// to show the user.
class EngineError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;

    enum class Retry { None, Connection, Capacity };
    EngineError(const std::string& message, Retry kind, std::optional<double> after = {})
        : std::runtime_error(message), retry(kind), retry_after_s(after) {}
    // Retained by value when the speech lane returns a failed job to its submitter.
    Retry retry = Retry::None;
    std::optional<double> retry_after_s;
};

} // namespace fv
