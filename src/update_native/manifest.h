#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "update/apply_error.h"

namespace fv::update_native {

inline constexpr std::uint64_t max_unpacked_bytes = 2ULL << 30;
inline constexpr std::size_t max_members = 50'000;
inline constexpr std::size_t max_manifest_bytes = 4U << 20;
inline constexpr std::string_view updater_name = "fiorivoce-updater.exe";

// Declared in fv_update so the app side can tell them apart without knowing this module.
using UpdateError = update::UpdateApplyError;
using RequiresSetup = update::UpdateRequiresSetup;
using Canceled = update::UpdateCanceled;

struct FileRecord {
    std::uint64_t size;
    std::string sha256;
};
using FileTable = std::map<std::string, FileRecord>;

// Validate and return a relative UTF-8 name in Windows release layout.
std::string member_name(std::string_view raw, bool directory = false);
void check_topology(std::string_view name, bool directory);
FileTable validate_manifest(const Json& manifest, std::string_view expected_version = {},
                            std::string_view updater_version = {});

// The tree is the staged FioriVoce distribution. Its existing manifest and completion sentinel
// do not become release payload. Building the manifest never changes the tree.
Json build_manifest(const std::filesystem::path& tree, std::string_view version);
void verify_tree(const std::filesystem::path& tree, const Json& manifest);

} // namespace fv::update_native
