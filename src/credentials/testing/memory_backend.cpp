#include "credentials/testing/memory_backend.h"

namespace fv {

MemoryCredentialBackend::MemoryCredentialBackend(std::string name) : name_(std::move(name)) {}

std::optional<std::string> MemoryCredentialBackend::get(std::string_view service,
                                                        std::string_view name) {
    std::lock_guard lock(mutex_);
    const auto it = values_.find(std::pair<std::string, std::string>(service, name));
    if (it == values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void MemoryCredentialBackend::set(std::string_view service, std::string_view name,
                                  std::string_view value) {
    std::lock_guard lock(mutex_);
    values_[std::pair<std::string, std::string>(service, name)] = std::string(value);
}

bool MemoryCredentialBackend::remove(std::string_view service, std::string_view name) {
    std::lock_guard lock(mutex_);
    return values_.erase(std::pair<std::string, std::string>(service, name)) > 0;
}

CredentialStatus MemoryCredentialBackend::probe() {
    return {true, name_, ""};
}

} // namespace fv
