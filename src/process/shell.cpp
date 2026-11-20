#include "process/launcher.h"

#include <exception>
#include <typeinfo>

#include "base/log.h"
#include "base/utf8.h"
#include "process/detail/launch_internal.h"
#include "process/launch.h"

namespace fv {

namespace {

constexpr log::Logger logger("services.actions.launch");

class PlatformShell final : public Shell {
public:
    bool open_url(std::string_view url) override {
        try {
            open_uri(url);
            return true;
        } catch (const std::exception& error) {
            logger.info("A link couldn't be opened ({})", typeid(error).name());
            return false;
        }
    }

    bool reveal(const std::filesystem::path& path) override {
        try {
            return launch_backend()->reveal(path);
        } catch (const std::exception& error) {
            logger.info("A file couldn't be shown in its folder ({})", typeid(error).name());
            return false;
        }
    }

    bool launch_via_explorer(const std::filesystem::path& path, std::string_view args) override {
        try {
            const auto backend = launch_backend();
            if (!backend->supports_launching()) {
                return false;
            }
            const auto file = path_to_utf8(path);
            if (process_detail::ask_explorer(file, args)) {
                return true;
            }
            // Only without Explorer's desktop: a NoAnswer is never retried, since Explorer may
            // still start it.
            return backend->start_here(file, args);
        } catch (const std::exception& error) {
            logger.info("A program couldn't be started through Explorer ({})",
                        typeid(error).name());
            return false;
        }
    }
};

} // namespace

std::shared_ptr<Shell> make_shell() {
    return std::make_shared<PlatformShell>();
}

} // namespace fv
