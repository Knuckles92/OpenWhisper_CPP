#pragma once

#include <filesystem>

namespace fv::update_native::detail {
bool unsafe_node(const std::filesystem::path& path);
// Flush file contents or a directory entry before advancing a transaction.
void flush_file(const std::filesystem::path& path);
void flush_directory(const std::filesystem::path& path);
void replace_file(const std::filesystem::path& source, const std::filesystem::path& destination);
// Atomic within a filesystem. Fails if a competing writer already published the destination.
void publish_new(const std::filesystem::path& source, const std::filesystem::path& destination);
} // namespace fv::update_native::detail
