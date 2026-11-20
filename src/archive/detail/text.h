#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fv::archive::detail {

#ifdef _WIN32
inline constexpr bool kWindows = true;
#else
inline constexpr bool kWindows = false;
#endif

// Python's repr() of a str, for names and paths in error texts: single quotes unless the text
// holds one and no double quote, backslashes and control characters escaped. Characters above
// ASCII are kept as they are (Python escapes the few it does not consider printable).
std::string python_repr(std::string_view text);

// `text` split at every `separator`, empty parts kept (str.split).
std::vector<std::string> split(std::string_view text, char separator);
std::string join(const std::vector<std::string>& parts, char separator);

// os.path.normpath of a name, with "/" as the separator in the result: "." and empty parts
// dropped, ".." folded. On Windows "\" separates as well.
std::string normpath(std::string_view name);

// os.path.realpath: links resolved where the path exists, the rest lexical. Windows normalizes
// first, as ntpath.realpath does.
std::filesystem::path realpath(const std::filesystem::path& path);

// os.path.commonpath([path, root]) == root, compared without regard to case on Windows.
bool is_within(const std::filesystem::path& path, const std::filesystem::path& root);

} // namespace fv::archive::detail
