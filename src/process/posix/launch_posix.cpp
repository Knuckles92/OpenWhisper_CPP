#include "process/detail/launch_internal.h"

namespace fv::process_detail {

ComScope::ComScope() = default;
ComScope::~ComScope() = default;

namespace {

// OpenWhisper's macOS and Linux handlers (NSWorkspace, xdg-mime, open, xdg-open, terminal
// scripts) have no milestone: lookups can't tell, and nothing opens.
class UnsupportedLaunchBackend final : public LaunchBackend {
public:
    bool supports_launching() const override { return false; }

    AssocAnswer assoc_query(std::string_view, AssocString) override {
        return {0x80004001, {}}; // E_NOTIMPL: neither "no handler" nor an answer
    }
    bool explorer_open(std::string_view, std::string_view) override { return false; }
    bool start_here(std::string_view, std::string_view) override { return false; }
    bool start_console(const ProcessSpec&) override { return false; }
    bool reveal(const std::filesystem::path&) override { return false; }
};

} // namespace

std::shared_ptr<LaunchBackend> platform_launch_backend() {
    return std::make_shared<UnsupportedLaunchBackend>();
}

} // namespace fv::process_detail
