#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace fv::update_native {

// Holds every component of an absolute directory path open without delete sharing.
// This prevents an ancestor from being renamed or replaced while the guard is alive.
// posix/ opens each component without following a link and works relative to those descriptors,
// so a swapped ancestor cannot redirect an operation; it cannot stop another process from
// renaming or deleting an entry (POSIX has no sharing modes), which only the Windows product
// relies on.
class GuardedDirectory {
public:
    class FilePin {
    public:
        ~FilePin();
        FilePin(FilePin&&) noexcept;
        FilePin& operator=(FilePin&&) noexcept;
        FilePin(const FilePin&) = delete;
        FilePin& operator=(const FilePin&) = delete;

    private:
        struct Impl;
        explicit FilePin(std::unique_ptr<Impl>);
        std::unique_ptr<Impl> impl_;
        friend class GuardedDirectory;
    };
    explicit GuardedDirectory(const std::filesystem::path& directory);
    ~GuardedDirectory();
    GuardedDirectory(GuardedDirectory&&) noexcept;
    GuardedDirectory& operator=(GuardedDirectory&&) noexcept;
    GuardedDirectory(const GuardedDirectory&) = delete;
    GuardedDirectory& operator=(const GuardedDirectory&) = delete;

    // Both arguments are single child names. The source must be a non-reparse directory;
    // the destination must not exist. No path from a journal should be passed here.
    void rename_child_exclusive(const std::filesystem::path& source,
                                const std::filesystem::path& destination) const;
    std::string read_file(const std::filesystem::path& name, std::size_t limit) const;
    void write_file_atomic(const std::filesystem::path& name, std::string_view bytes) const;
    // Returns null for a missing leaf. The pin prevents deletion and replacement, and also
    // prevents writes unless allow_writes is set for an SQLite output being populated. The leaf
    // must be an ordinary single-link file.
    std::unique_ptr<FilePin> pin_file(const std::filesystem::path& name,
                                      bool allow_writes = false) const;
    // Copy from the already pinned source handle and atomically replace this directory's leaf.
    void copy_file_atomic_from(const FilePin& source,
                               const std::filesystem::path& destination) const;
    // Remove only an ordinary single-link file, without following a reparse point.
    void remove_file(const std::filesystem::path& name,
                     std::string_view expected_sha256 = {}) const;
    // Refuses reparse points and nonempty directories.
    void remove_empty_directory(const std::filesystem::path& name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fv::update_native
