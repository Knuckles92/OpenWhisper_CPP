#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fv {

// The process environment, injected so tests can pin it. Names and values are UTF-8.
// Affinity: thread-safe.
class EnvReader {
public:
    virtual ~EnvReader() = default;
    virtual std::optional<std::string> get(std::string_view name) const = 0;
    // True for the process's own environment. A lookup that leaves the environment (the
    // password database, for "~" without HOME) is made only then: an environment a test pins
    // without HOME must not resolve to the real user's folders.
    virtual bool is_process() const { return false; }
};

// The real process environment, as Python's os.environ.get: nullopt when unset, and a value set
// to "" (possible outside Windows) is "". Windows reads the C runtime's wide environment, so
// names match without regard to case there. Tests use testing/map_env_reader.h instead.
const EnvReader& system_env();

} // namespace fv
