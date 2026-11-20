#include "process/process_env.h"

#include <cstdlib>
#include <stdexcept>

extern char** environ;

namespace fv {

namespace {

// os.environ on POSIX: names and values as the process holds them.
class PosixEnv final : public MutableEnv {
public:
    std::optional<std::string> get(std::string_view name) const override {
        if (name.empty() || name.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    }

    void set(std::string_view name, std::string_view value) override {
        check_env_assignment(name, value, false);
        if (::setenv(std::string(name).c_str(), std::string(value).c_str(), 1) != 0) {
            throw std::invalid_argument("illegal environment variable name");
        }
    }

    void unset(std::string_view name) override {
        if (name.empty() || name.find('\0') != std::string_view::npos) {
            return;
        }
        ::unsetenv(std::string(name).c_str());
    }

    std::vector<std::pair<std::string, std::string>> items() const override {
        std::vector<std::pair<std::string, std::string>> out;
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            const std::string_view text(*entry);
            const auto equals = text.find('=');
            if (equals == std::string_view::npos) {
                continue;
            }
            out.emplace_back(std::string(text.substr(0, equals)),
                             std::string(text.substr(equals + 1)));
        }
        return out;
    }
};

} // namespace

MutableEnv& system_mutable_env() {
    static auto* env = new PosixEnv();
    return *env;
}

} // namespace fv
