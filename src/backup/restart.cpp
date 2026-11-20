#include "backup/restart.h"

#include "base/utf8.h"

namespace fv::backup {

std::vector<std::string> restart_command(const std::filesystem::path& exe) {
    return {path_to_utf8(exe)};
}

std::expected<void, std::string> spawn_restart(const std::filesystem::path& exe,
                                               ProcessStarter& starter) {
    const std::vector<std::string> command = restart_command(exe);
    return starter.start(path_from_utf8(command.front()),
                         std::span<const std::string>(command).subspan(1));
}

} // namespace fv::backup
