#include "base/detail/home.h"

#include <pwd.h>
#include <unistd.h>

#include <vector>

namespace fv::detail {

std::optional<std::string> passwd_home(std::optional<std::string_view> user) {
    long suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(suggested > 0 ? static_cast<std::size_t>(suggested) : 16384);
    passwd entry{};
    passwd* found = nullptr;
    int error = 0;
    if (user) {
        const std::string name(*user);
        error = getpwnam_r(name.c_str(), &entry, buffer.data(), buffer.size(), &found);
    } else {
        error = getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found);
    }
    if (error != 0 || found == nullptr || found->pw_dir == nullptr) {
        return std::nullopt;
    }
    return std::string(found->pw_dir);
}

} // namespace fv::detail
