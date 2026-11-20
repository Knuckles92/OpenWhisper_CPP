#include "agents/detail/long_path.h"

#include <system_error>

namespace fv::agents::detail {

std::filesystem::path long_real_path(const std::filesystem::path& folder) {
    // No short names here: the real path is the long one.
    std::error_code error;
    auto real = std::filesystem::weakly_canonical(std::filesystem::absolute(folder, error), error);
    return error ? folder : real;
}

} // namespace fv::agents::detail
