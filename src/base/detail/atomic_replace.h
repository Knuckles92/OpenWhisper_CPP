#pragma once

#include <filesystem>
#include <string_view>

// The platform half of write_file_atomic: one implementation in win/, one in posix/.
namespace fv::detail {

// Creates `temp` exclusively; false when it already exists. Throws filesystem_error otherwise.
bool create_temp_file(const std::filesystem::path& temp, std::string_view bytes, bool fsync);

// Moves `temp` over `target`, replacing it. Throws filesystem_error.
void replace_file(const std::filesystem::path& temp, const std::filesystem::path& target);

} // namespace fv::detail
