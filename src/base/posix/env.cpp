#include "base/env.h"

#include <cstdlib>

namespace fv {

namespace {

class ProcessEnv final : public EnvReader {
public:
    std::optional<std::string> get(std::string_view name) const override {
        const std::string key(name);
        if (key.empty() || key.find('\0') != std::string::npos) {
            return std::nullopt;
        }
        const char* value = std::getenv(key.c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    }

    bool is_process() const override { return true; }
};

} // namespace

const EnvReader& system_env() {
    static const ProcessEnv env;
    return env;
}

} // namespace fv
