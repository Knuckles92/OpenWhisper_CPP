#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Text crosses the core libraries as UTF-8 std::string; these meet the platform's native path
// and environment encodings, by the same rules as engine/platform.h (which base cannot include).
//
// They sit in an inline namespace so that their symbols differ from engine/platform.h's
// fv::path_from_utf8 and fv::path_to_utf8 of the same signature: a program linking fv_engine and
// fv_base would otherwise hold two definitions of one function. A file that includes both headers
// calls one of them qualified (fv::base_utf8::path_from_utf8).
namespace fv {
inline namespace base_utf8 {

std::filesystem::path path_from_utf8(std::string_view text);
std::string path_to_utf8(const std::filesystem::path& path);

// The environment variable's value, or nullopt when it is unset or empty.
std::optional<std::string> env_utf8(std::string_view name);

} // namespace base_utf8
} // namespace fv
