#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The import table of a Windows executable or DLL, read from its bytes (no loader involved), so
// that an install folder can be checked for the libraries its programs need.
namespace fv::cli {

// The DLL names `image` imports through its import directory, in table order (delay-loaded
// imports are not listed: the program starts without them). An image without an import
// directory gives an empty list; bytes that are not a PE32 or PE32+ image, or whose import table
// points outside them, give nullopt.
std::optional<std::vector<std::string>> pe_imports(std::span<const std::byte> image);

// pe_imports of the file's bytes; nullopt also when the file cannot be read.
std::optional<std::vector<std::string>> pe_imports(const std::filesystem::path& file);

} // namespace fv::cli
