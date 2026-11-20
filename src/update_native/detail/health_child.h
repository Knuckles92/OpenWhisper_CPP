#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fv::update_native::detail {
// The one process a health check starts. It owns the handle it was given at creation and never
// looks a process up by number, so a recycled PID cannot be waited on or ended by mistake.
class HealthChild {
public:
    virtual ~HealthChild() = default;
    // Starts `executable` in `directory`. The arguments are fixed switches and validated hex
    // tokens: they are passed as written, without quoting. Throws std::system_error.
    static std::unique_ptr<HealthChild> launch(const std::filesystem::path& executable,
                                               const std::vector<std::string>& arguments,
                                               const std::filesystem::path& directory);
    // Waits up to `timeout` (zero only looks). True once the process has exited.
    virtual bool wait(std::chrono::milliseconds timeout) = 0;
    // Ends a process that is still running and waits until it is gone. Throws when it stays.
    virtual void terminate() = 0;
};
} // namespace fv::update_native::detail
