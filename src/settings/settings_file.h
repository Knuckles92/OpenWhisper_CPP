#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fv {

// What tells one state of the settings file from another (SettingsManager._file_signature): the
// absolute path, and for a file that exists its volume, file index, size and two times. A path
// changed, an edit in place and an atomic replace from outside all change it.
struct FileSignature {
    std::filesystem::path path; // absolute
    bool exists = false;
    std::uint64_t volume = 0; // st_dev: the volume serial number on Windows
    std::uint64_t index = 0;  // st_ino: the file index on Windows
    std::uint64_t size = 0;
    std::int64_t modified = 0; // last write, in the platform's ticks (100 ns FILETIME on Windows)
    std::int64_t changed = 0;  // st_ctime_ns: creation on Windows, status change elsewhere

    bool operator==(const FileSignature&) const = default;
};

// A settings file that could not be read, with the Python exception OpenWhisper raised for it
// ("JSONDecodeError", "UnicodeDecodeError", "ValueError", "RecursionError", "PermissionError",
// ...). what() is that exception's message. A strict read (update, mutate) throws it.
class SettingsFileError : public std::runtime_error {
public:
    SettingsFileError(std::string python_type, const std::string& message)
        : std::runtime_error(message), python_type_(std::move(python_type)) {}
    const std::string& python_type() const noexcept { return python_type_; }

private:
    std::string python_type_;
};

// The bytes behind a SettingsStore: the file on disk, or an in-memory one in tests
// (settings/testing/memory_store.h).
// Affinity: called under the store's lock only.
class SettingsFile {
public:
    virtual ~SettingsFile() = default;
    // As given, for messages (Python formats the path it was handed).
    virtual const std::filesystem::path& path() const = 0;
    // nullopt when it cannot be checked (stat failed for another reason than "missing").
    virtual std::optional<FileSignature> signature() const = 0;
    // os.path.exists: false on any error.
    virtual bool exists() const = 0;
    // The whole file; SettingsFileError (the OSError family) when it cannot be opened or read.
    virtual std::string read() const = 0;
    // Replaces the file with `bytes` atomically; on failure the old file stays and the error
    // (std::filesystem::filesystem_error on disk) propagates.
    virtual void replace(std::string_view bytes) = 0;
};

// The settings file at `path`: signature from GetFileInformationByHandle on Windows (stat
// elsewhere), atomic writes with base/atomic_file.h.
std::unique_ptr<SettingsFile> disk_settings_file(std::filesystem::path path);

} // namespace fv
