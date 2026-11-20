#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "http/detail/os_text.h"
#include "http/proxy.h"

extern char** environ;

namespace fv::http::detail {

std::vector<std::pair<std::string, std::string>> environment_entries() {
    std::vector<std::pair<std::string, std::string>> entries;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string line(*entry);
        const auto equals = line.find('=');
        if (equals != std::string::npos) {
            entries.emplace_back(line.substr(0, equals), line.substr(equals + 1));
        }
    }
    return entries;
}

// urllib reads no registry outside Windows.
std::optional<RegistryProxy> read_registry_proxy() {
    return std::nullopt;
}

std::string socket_error_text(long code) {
    return "[Errno " + std::to_string(code) + "] " + std::strerror(static_cast<int>(code));
}

std::string dns_failure_text() {
    return "[Errno -2] Name or service not known";
}

} // namespace fv::http::detail
