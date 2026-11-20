#include "archive/safe_extract.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "archive/detail/text.h"
#include "archive/errors.h"
#include "archive/tar_gz_reader.h"
#include "archive/zip_reader.h"
#include "base/utf8.h"

namespace fv::archive {

namespace {

namespace fs = std::filesystem;
using detail::kWindows;
using detail::python_repr;

// How deep link-to-link copies go before giving up quietly; tarfile would recurse until Python's
// recursion limit.
constexpr int kMaxLinkDepth = 32;

bool contains(const std::vector<std::string>& parts, std::string_view value) {
    return std::find(parts.begin(), parts.end(), value) != parts.end();
}

class OutputFile {
public:
    explicit OutputFile(const fs::path& path)
        : path_(path), out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) {
            throw fs::filesystem_error(
                "open", path, std::error_code(errno != 0 ? errno : EIO, std::generic_category()));
        }
    }
    void write(std::span<const char> data) {
        out_.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out_) {
            throw fs::filesystem_error("write", path_, std::make_error_code(std::errc::io_error));
        }
    }
    void close() {
        out_.close();
        if (!out_) {
            throw fs::filesystem_error("close", path_, std::make_error_code(std::errc::io_error));
        }
    }

private:
    fs::path path_;
    std::ofstream out_;
};

void make_parent_directories(const fs::path& target) {
    const auto parent = target.parent_path();
    std::error_code error;
    if (!parent.empty() && !fs::exists(parent, error)) {
        fs::create_directories(parent);
    }
}

// --- zip -------------------------------------------------------------------------------------

std::size_t first_code_point_length(std::string_view text) {
    const auto lead = static_cast<unsigned char>(text.front());
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xF0) {
        return 4;
    }
    if (lead >= 0xE0) {
        return 3;
    }
    return 2;
}

// zipfile._extract_member's arcname: the member's name made relative and, on Windows, legal.
std::string zip_arcname(std::string_view filename) {
    std::string arcname(filename);
    if (kWindows && !arcname.empty()) {
        // ntpath.splitdrive: any first character followed by ":" is a drive.
        const auto first = first_code_point_length(arcname);
        if (arcname.size() > first && arcname[first] == ':') {
            arcname.erase(0, first + 1);
        }
    }
    std::vector<std::string> kept;
    for (auto& part : detail::split(arcname, '/')) {
        if (!part.empty() && part != "." && part != "..") {
            kept.push_back(std::move(part));
        }
    }
    if (kWindows) {
        std::vector<std::string> legal;
        for (auto& part : kept) {
            for (auto& c : part) {
                if (std::string_view(":<>|\"?*").find(c) != std::string_view::npos) {
                    c = '_';
                }
            }
            while (!part.empty() && (part.back() == ' ' || part.back() == '.')) {
                part.pop_back();
            }
            if (!part.empty()) {
                legal.push_back(std::move(part));
            }
        }
        kept = std::move(legal);
    }
    return detail::join(kept, '/');
}

// --- tar -------------------------------------------------------------------------------------

std::string dirname(std::string_view name) {
    const auto at = kWindows ? name.find_last_of("/\\") : name.rfind('/');
    if (at == std::string_view::npos) {
        return {};
    }
    auto head = name.substr(0, at);
    // os.path.dirname keeps a root of only separators but drops trailing ones.
    while (head.size() > 1 && (head.back() == '/' || (kWindows && head.back() == '\\'))) {
        head.remove_suffix(1);
    }
    return std::string(head);
}

bool is_absolute_name(std::string_view name) {
    if (!kWindows) {
        return name.starts_with('/');
    }
    // ntpath.isabs in Python 3.12: a root, or a drive followed by a root.
    std::string head(name.substr(0, 3));
    std::replace(head.begin(), head.end(), '/', '\\');
    return head.starts_with('\\') || (head.size() >= 3 && head[1] == ':' && head[2] == '\\');
}

std::string unsafe(const std::string& reason) {
    return "Unsafe speech runtime archive: " + reason;
}

struct Filtered {
    std::string name;
    std::optional<std::int64_t> mode;
};

// tarfile.data_filter (Python 3.12.10).
Filtered data_filter(const TarMember& member, const fs::path& dest) {
    Filtered out;
    out.name = member.name;
    const auto dest_path = detail::realpath(dest);
    if (out.name.starts_with('/') || (kWindows && out.name.starts_with('\\'))) {
        const auto first = out.name.find_first_not_of(kWindows ? "/\\" : "/");
        out.name = first == std::string::npos ? std::string() : out.name.substr(first);
    }
    if (is_absolute_name(out.name)) {
        throw ArchiveError(unsafe("member " + python_repr(member.name) + " has an absolute path"));
    }
    const auto target = detail::realpath(dest_path / path_from_utf8(out.name));
    if (!detail::is_within(target, dest_path)) {
        throw ArchiveError(unsafe(python_repr(member.name) + " would be extracted to " +
                                  python_repr(path_to_utf8(target)) +
                                  ", which is outside the destination"));
    }
    std::int64_t mode = member.mode & 0755;
    if (member.is_regular() || member.is_hardlink()) {
        if ((mode & 0100) == 0) {
            mode &= ~std::int64_t{0111};
        }
        mode |= 0600;
        out.mode = mode;
    } else if (!member.is_directory() && !member.is_symlink()) {
        throw ArchiveError(unsafe(python_repr(member.name) + " is a special file"));
    }
    if (member.is_hardlink() || member.is_symlink()) {
        if (is_absolute_name(member.linkname)) {
            throw ArchiveError(unsafe(python_repr(member.name) + " is a link to an absolute path"));
        }
        fs::path link_target = dest_path;
        if (member.is_symlink()) {
            link_target /= path_from_utf8(dirname(out.name));
        }
        link_target = detail::realpath(link_target / path_from_utf8(member.linkname));
        if (!detail::is_within(link_target, dest_path)) {
            throw ArchiveError(unsafe(python_repr(member.name) + " would link to " +
                                      python_repr(path_to_utf8(link_target)) +
                                      ", which is outside the destination"));
        }
    }
    return out;
}

// TarFile.chmod and TarFile.utime: failures are ExtractErrors, which extract() ignores.
void set_attributes(const fs::path& target, std::optional<std::int64_t> mode, double mtime) {
    std::error_code ignored;
    if (mode) {
        fs::permissions(target, static_cast<fs::perms>(*mode & 07777), fs::perm_options::replace,
                        ignored);
        if (ignored) {
            return;
        }
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::duration<double>(mtime));
    const auto when = std::chrono::clock_cast<std::chrono::file_clock>(
        std::chrono::system_clock::time_point(seconds));
    fs::last_write_time(target, when, ignored);
}

class TarExtraction {
public:
    TarExtraction(const fs::path& archive, const fs::path& target_dir,
                  std::vector<TarMember> members)
        : archive_(archive), target_dir_(target_dir), members_(std::move(members)) {}

    std::size_t size() const { return members_.size(); }
    const TarMember& member(std::size_t index) const { return members_[index]; }
    // The loop renames members in place, as the Python edits the TarInfo objects getmembers()
    // returned: link targets are looked up among the names as renamed so far.
    void update(std::size_t index, const TarMember& renamed) { members_[index] = renamed; }

    // TarFile._extract_member for a filtered member, its data at `reader`'s position.
    void extract(const TarMember& member, const Filtered& filtered, std::size_t index,
                 TarGzReader& reader) {
        std::string name = filtered.name;
        while (name.ends_with('/')) {
            name.pop_back();
        }
        const fs::path target = target_dir_ / path_from_utf8(name);
        make_parent_directories(target);
        if (member.is_regular()) {
            write_data(reader, target);
        } else if (member.is_directory()) {
            make_directory(target);
        } else {
            make_link(member, index, target, 0);
        }
        if (!member.is_symlink()) {
            set_attributes(target, filtered.mode, member.mtime);
        }
    }

private:
    static void write_data(TarGzReader& reader, const fs::path& target) {
        OutputFile out(target);
        std::array<char, 1 << 16> buffer{};
        while (true) {
            const auto got = reader.read(buffer);
            if (got == 0) {
                break;
            }
            out.write(std::span<const char>(buffer.data(), got));
        }
        out.close();
    }

    static void make_directory(const fs::path& target) {
        std::error_code error;
        if (!fs::create_directory(target, error) && error) {
            if (!fs::is_directory(target)) {
                throw fs::filesystem_error("mkdir", target, error);
            }
        }
    }

    // Writes member `index`'s data again, read from a fresh pass over the archive: tarfile
    // seeks back for it.
    void copy_member_data(std::size_t index, const fs::path& target) const {
        TarGzReader reader(archive_);
        for (std::size_t i = 0; i <= index; ++i) {
            if (!reader.next()) {
                throw BadArchive("unexpected end of data");
            }
        }
        write_data(reader, target);
    }

    // TarFile._find_link_target: a symbolic link's target relative to its folder, searched in
    // the whole archive; a hard link's among the members before it. Names are as renamed so far.
    std::optional<std::size_t> find_link_target(const TarMember& member,
                                                const std::string& current_name,
                                                std::size_t index) const {
        std::string linkname;
        std::size_t limit = members_.size();
        if (member.is_symlink()) {
            const auto folder = dirname(current_name);
            linkname = folder.empty()            ? member.linkname
                       : member.linkname.empty() ? folder
                                                 : folder + "/" + member.linkname;
        } else {
            linkname = member.linkname;
            limit = index;
        }
        const auto wanted = detail::normpath(linkname);
        for (std::size_t i = limit; i-- > 0;) {
            if (detail::normpath(members_[i].name) == wanted) {
                return i;
            }
        }
        return std::nullopt;
    }

    // What _extract_member does with the member a link names, when the link itself cannot be
    // made: its data copied, or its own link followed.
    void extract_found(std::size_t found, const fs::path& target, int depth) {
        const TarMember& source = members_[found];
        if (source.is_regular()) {
            copy_member_data(found, target);
            set_attributes(target, source.mode, source.mtime);
        } else if (source.is_directory()) {
            // The member as getmembers() read it, unfiltered: tarfile chmods to its own mode.
            make_directory(target);
            set_attributes(target, source.mode, source.mtime);
        } else if (source.is_symlink() || source.is_hardlink()) {
            make_link(source, found, target, depth + 1);
            if (!source.is_symlink()) {
                set_attributes(target, source.mode, source.mtime);
            }
        }
    }

    void fall_back(const TarMember& member, std::size_t index, const fs::path& target, int depth) {
        if (depth >= kMaxLinkDepth) {
            return;
        }
        // An unresolvable target is a non-fatal ExtractError: the link is skipped.
        if (const auto found = find_link_target(member, members_[index].name, index)) {
            extract_found(*found, target, depth);
        }
    }

    // TarFile.makelink.
    void make_link(const TarMember& member, std::size_t index, const fs::path& target, int depth) {
        try {
            if (member.is_symlink()) {
                std::error_code error;
                if (fs::exists(fs::symlink_status(target, error))) {
                    fs::remove(target);
                }
                auto link = path_from_utf8(member.linkname);
                if (kWindows) {
                    link.make_preferred();
                }
                fs::create_symlink(link, target);
                return;
            }
            const fs::path link_target = target_dir_ / path_from_utf8(member.linkname);
            std::error_code error;
            if (fs::exists(link_target, error)) {
                fs::create_hard_link(link_target, target);
                return;
            }
            const auto found = find_link_target(member, members_[index].name, index);
            if (!found) {
                throw BadArchive("linkname " + python_repr(member.linkname) + " not found");
            }
            extract_found(*found, target, depth);
        } catch (const fs::filesystem_error&) {
            fall_back(member, index, target, depth);
        }
    }

    fs::path archive_;
    fs::path target_dir_;
    std::vector<TarMember> members_;
};

} // namespace

void extract_zip(const std::filesystem::path& archive, const std::filesystem::path& target_dir,
                 const ExtractProgress& progress, const CancelToken& cancel) {
    ZipReader zip(archive);
    const auto& members = zip.entries();
    const auto total = static_cast<std::int64_t>(members.size());
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (cancel.cancelled()) {
            throw ExtractCanceled();
        }
        std::string name = members[index].name;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.starts_with('/') || contains(detail::split(name, '/'), "..")) {
            throw ArchiveError("Archive contains an unsafe path: " + name);
        }

        const auto arcname = zip_arcname(members[index].name);
        if (arcname.empty() && !members[index].is_dir) {
            throw BadArchive("Empty filename.");
        }
        const fs::path target = arcname.empty() ? target_dir : target_dir / path_from_utf8(arcname);
        make_parent_directories(target);
        if (members[index].is_dir) {
            std::error_code error;
            if (!fs::is_directory(target, error)) {
                fs::create_directory(target);
            }
        } else {
            OutputFile out(target);
            zip.read(index, [&out](std::span<const char> data) { out.write(data); });
            out.close();
        }
        if (progress) {
            progress(static_cast<std::int64_t>(index) + 1, total);
        }
    }
}

void extract_native_tar(const std::filesystem::path& archive,
                        const std::filesystem::path& target_dir, std::string_view root,
                        std::string_view folder, const ExtractProgress& progress,
                        const CancelToken& cancel) {
    // The upstream libraries use relative symlinks. The data filter rejects links outside the
    // destination, special files and unsafe permissions before extraction.
    TarExtraction extraction(archive, target_dir, list_tar_gz(archive));
    TarGzReader reader(archive);
    const auto total = static_cast<std::int64_t>(extraction.size());
    for (std::size_t index = 0; index < extraction.size(); ++index) {
        if (!reader.next()) {
            throw BadArchive("unexpected end of data");
        }
        if (cancel.cancelled()) {
            throw ExtractCanceled();
        }
        TarMember member = extraction.member(index);
        auto parts = detail::split(member.name, '/');
        if (parts.front() != root || contains(parts, "..")) {
            throw ArchiveError("Archive contains an unsafe path: " + member.name);
        }
        if (root != folder) {
            parts.front() = std::string(folder);
            member.name = detail::join(parts, '/');
            extraction.update(index, member);
            if (member.is_hardlink()) {
                // Hard links name another member by its archive path.
                auto link = detail::split(member.linkname, '/');
                if (link.front() != root || contains(link, "..")) {
                    throw ArchiveError("Archive contains an unsafe link: " + member.linkname);
                }
                link.front() = std::string(folder);
                member.linkname = detail::join(link, '/');
                extraction.update(index, member);
            }
        }
        const auto filtered = data_filter(member, target_dir);
        extraction.extract(member, filtered, index, reader);
        if (progress) {
            progress(static_cast<std::int64_t>(index) + 1, total);
        }
    }
}

} // namespace fv::archive
