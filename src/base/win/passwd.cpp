#include "base/detail/home.h"

namespace fv::detail {

// Windows has no password database; ntpath.expanduser never consults one.
std::optional<std::string> passwd_home(std::optional<std::string_view>) {
    return std::nullopt;
}

} // namespace fv::detail
