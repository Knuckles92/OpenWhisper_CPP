#include "process/detail/launch_internal.h"
#include "process/win/explorer_open.h"

namespace fv::process_detail {

namespace {

class WindowsLaunchBackend final : public LaunchBackend {
public:
    AssocAnswer assoc_query(std::string_view scheme, AssocString what) override {
        // Windows reads some packaged apps' protocols (ms-actioncenter:) only through COM;
        // without it, AssocQueryStringW says they have no handler.
        ComScope com;
        return process_win::assoc_query(scheme, what);
    }

    bool explorer_open(std::string_view file, std::string_view args) override {
        return process_win::explorer_open(file, args, process_win::desktop_application,
                                          process_win::allow_foreground);
    }

    bool start_here(std::string_view file, std::string_view args) override {
        ComScope com;
        return process_win::shell_execute_here(file, args);
    }

    bool start_console(const ProcessSpec& spec) override {
        // No job and no pipes: the console window is the user's, and nothing waits for it.
        auto started = make_process_launcher()->start(spec);
        return started.has_value();
    }

    bool reveal(const std::filesystem::path& path) override {
        ComScope com;
        return process_win::reveal_in_folder(path);
    }
};

} // namespace

std::shared_ptr<LaunchBackend> platform_launch_backend() {
    return std::make_shared<WindowsLaunchBackend>();
}

} // namespace fv::process_detail
