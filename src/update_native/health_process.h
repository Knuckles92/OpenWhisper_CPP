#pragma once
#include "base/cancel.h"
#include "update_native/transaction_storage.h"
#include <chrono>
#include <memory>
#include <string_view>

namespace fv::update_native {
// Owns only the exact process created by this object. Never opens or terminates a PID by number.
class HealthProcess {
public:
    HealthProcess();
    ~HealthProcess();
    HealthProcess(const HealthProcess&) = delete;
    HealthProcess& operator=(const HealthProcess&) = delete;
    bool launch_and_wait(TransactionStorage&, std::string_view token,
                         std::string_view expected_sha256, std::chrono::milliseconds timeout,
                         const CancelToken&);
    // Called only after a failed health check; waits until the launched process has exited.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fv::update_native
