#pragma once

#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "remote/client_history_source.h"

namespace fv::remote::testing {

// A ClientHistorySource with scripted clients and answers: query returns the answer set for
// (device, operation), throws ClientUnavailable with the code set by fail(), and otherwise
// ClientUnavailable("unavailable") as for a client that isn't connected. Queries are recorded.
// Affinity: thread-safe.
class FakeClientHistorySource final : public ClientHistorySource {
public:
    struct Query {
        std::string device_id;
        std::string operation;
        Json params;
    };

    void set_clients(std::vector<ClientHistoryStatus> clients) {
        std::lock_guard lock(mutex_);
        clients_ = std::move(clients);
    }
    void answer(std::string device_id, std::string operation, Json result) {
        std::lock_guard lock(mutex_);
        answers_[{std::move(device_id), std::move(operation)}] = std::move(result);
    }
    void fail(std::string device_id, std::string code) {
        std::lock_guard lock(mutex_);
        failures_[std::move(device_id)] = std::move(code);
    }
    std::vector<Query> queries() const {
        std::lock_guard lock(mutex_);
        return queries_;
    }

    std::vector<ClientHistoryStatus> clients() override {
        std::lock_guard lock(mutex_);
        return clients_;
    }
    Json query(std::string_view device_id, std::string_view operation,
               const Json& params) override {
        std::lock_guard lock(mutex_);
        queries_.push_back(Query{std::string(device_id), std::string(operation), params});
        if (const auto failed = failures_.find(std::string(device_id)); failed != failures_.end()) {
            throw ClientUnavailable(failed->second);
        }
        const auto found = answers_.find({std::string(device_id), std::string(operation)});
        if (found == answers_.end()) {
            throw ClientUnavailable("unavailable");
        }
        return found->second;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ClientHistoryStatus> clients_;
    std::map<std::pair<std::string, std::string>, Json> answers_;
    std::map<std::string, std::string> failures_;
    std::vector<Query> queries_;
};

} // namespace fv::remote::testing
