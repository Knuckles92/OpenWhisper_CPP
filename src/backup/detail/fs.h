#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The pathlib and os calls services/backup.py makes, with their semantics on this platform:
// lexical paths as pathlib keeps them (".." never folded, case ignored on Windows), stat that
// follows links, and the link test that also counts Windows junctions.
namespace fv::backup::detail {

// _is_link: Path.is_symlink() or Path.is_junction(), without following the link. False when the
// path cannot be examined.
bool is_link(const std::filesystem::path& path);

// Path(path).expanduser().absolute(): a leading "~" becomes the home folder, and a relative path
// is joined to the working directory (of its drive), without folding ".." or resolving links.
std::filesystem::path absolute_path(const std::filesystem::path& path);

// path.absolute().relative_to(parent.absolute()) as parts, nullopt when `path` is not below
// `parent` (lexically, as pathlib compares: case-insensitive on Windows).
std::optional<std::vector<std::string>> relative_parts(const std::filesystem::path& path,
                                                       const std::filesystem::path& parent);
// _inside.
bool inside(const std::filesystem::path& path, const std::filesystem::path& parent);

// PurePath(text).is_absolute().
bool is_absolute_text(std::string_view text);
// PurePath(text) / with the parts of a "/"-separated relative name (root.joinpath(*name.split)).
std::filesystem::path join_parts(const std::filesystem::path& root, std::string_view name);
// Path.suffix: from the last "." of the final part, "" when the dot leads or ends the name.
std::string suffix(const std::filesystem::path& path);
// Path.name as UTF-8.
std::string name_of(const std::filesystem::path& path);

// Path.exists(), Path.is_file() and Path.is_dir(): links followed, false on any error.
bool exists(const std::filesystem::path& path);
bool is_file(const std::filesystem::path& path);
bool is_dir(const std::filesystem::path& path);

// What os.stat tells apart for the "data changed" checks: st_dev, st_ino, st_size and
// st_mtime_ns. On Windows the volume serial number, the file index and the last write time in
// 100 ns ticks (GetFileInformationByHandle, as os.stat reads them).
struct FileStat {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    bool regular = false; // stat.S_ISREG(st_mode)

    bool operator==(const FileStat&) const = default;
};

// os.stat(path), following links; std::filesystem::filesystem_error (Python's OSError) when it
// fails.
FileStat stat(const std::filesystem::path& path);

// A file opened for reading as open(path, "rb") opens it.
// Affinity: one thread at a time.
class InputFile {
public:
    // std::filesystem::filesystem_error when it cannot be opened.
    explicit InputFile(const std::filesystem::path& path);
    ~InputFile();
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;

    // os.fstat of the open file.
    FileStat stat() const;
    // Up to buffer.size() bytes; 0 at the end. std::filesystem::filesystem_error on failure.
    std::size_t read(std::span<std::byte> buffer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// tempfile.TemporaryDirectory(prefix=..., dir=parent): a new folder "<prefix><random>" created
// in `parent` (the system temporary folder when empty), removed with its content on
// destruction. std::filesystem::filesystem_error when it cannot be created.
// Affinity: one thread at a time.
class TempDir {
public:
    TempDir(const std::filesystem::path& parent, std::string_view prefix);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// os.fsync of the file at `path` (opened for writing). std::filesystem::filesystem_error on
// failure.
void fsync_file(const std::filesystem::path& path);
// _fsync_directory: persists a rename in `directory` where the platform supports it; a no-op on
// Windows, and failures are ignored elsewhere.
void fsync_directory(const std::filesystem::path& directory);

// shutil.disk_usage(directory).free: the volume's free bytes.
std::uint64_t free_bytes(const std::filesystem::path& directory);

// The whole file (Path.read_bytes); std::filesystem::filesystem_error when it cannot be read.
std::string read_file(const std::filesystem::path& path);

// Strict UTF-8 as Python's "utf-8" codec decodes it (no surrogates, no overlong forms).
bool is_valid_utf8(std::string_view bytes);

} // namespace fv::backup::detail
