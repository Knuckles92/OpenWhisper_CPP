#include "process/launcher.h"

namespace fv {

namespace {

// Programs are started on Windows only; the POSIX launcher of OpenWhisper (new sessions,
// process-group signals) has no milestone.
class UnsupportedLauncher final : public ProcessLauncher {
public:
    std::expected<std::unique_ptr<Process>, std::string> start(const ProcessSpec&) override {
        return std::unexpected(std::string("Starting programs is not supported on this platform."));
    }
};

} // namespace

std::shared_ptr<ProcessLauncher> make_process_launcher() {
    return std::make_shared<UnsupportedLauncher>();
}

bool can_start_programs() {
    return false;
}

} // namespace fv
