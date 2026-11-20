#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "credentials/store.h"

namespace fv {

// A process-local backend for tests and for callers that must not touch the OS store. Always
// available, under the name it was given ("in-memory store" by default). Tests derive from it
// to make reads fail or count them, as OpenWhisper's tests subclass MemoryCredentialBackend.
// Affinity: thread-safe.
class MemoryCredentialBackend : public CredentialBackend {
public:
    explicit MemoryCredentialBackend(std::string name = "in-memory store");

    std::optional<std::string> get(std::string_view service, std::string_view name) override;
    void set(std::string_view service, std::string_view name, std::string_view value) override;
    bool remove(std::string_view service, std::string_view name) override;
    CredentialStatus probe() override;

    const std::string& name() const { return name_; }

private:
    std::string name_;
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, std::string> values_;
};

} // namespace fv
