#pragma once

#include <filesystem>
#include <string_view>

#include "base/cancel.h"
#include "base/json.h"

namespace fv::update_native {

// Streams the contents against the bounded embedded manifest and verifies every SHA-256.
Json verify_archive(const std::filesystem::path& archive, std::string_view expected_version = {},
                    const CancelToken& cancel = {});
// Builds a verified candidate in a new directory. Existing destinations are never modified.
Json extract_archive(const std::filesystem::path& archive, const std::filesystem::path& destination,
                     std::string_view expected_version, const CancelToken& cancel = {});

// Writes the embedded manifest and the managed staged files, verifying before publishing.
void pack_archive(const std::filesystem::path& tree, std::string_view version,
                  const std::filesystem::path& output);
void write_manifest(const std::filesystem::path& tree, std::string_view version);

} // namespace fv::update_native
