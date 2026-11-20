#include "backup/detail/fs.h"

#include <system_error>

#include "base/ids.h"
#include "base/utf8.h"
#include "pyport/text.h"

namespace fv::backup::detail {

namespace {

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

// pathlib compares Windows paths by str(path).lower().
std::string fold(std::string_view text) {
    return kWindows ? py::lower(text) : std::string(text);
}

struct PureParts {
    std::string anchor; // drive + root
    std::vector<std::string> tail;
};

PureParts pure_parts(const std::filesystem::path& path) {
    PureParts parts;
    parts.anchor = path_to_utf8(path.root_name()) + path_to_utf8(path.root_directory());
    if constexpr (kWindows) {
        for (char& c : parts.anchor) {
            if (c == '/') {
                c = '\\';
            }
        }
    }
    for (const auto& element : path.relative_path()) {
        std::string text = path_to_utf8(element);
        if (!text.empty() && text != ".") {
            parts.tail.push_back(std::move(text));
        }
    }
    return parts;
}

std::filesystem::path home() {
    if constexpr (kWindows) {
        if (auto profile = env_utf8("USERPROFILE")) {
            return path_from_utf8(*profile);
        }
        auto drive = env_utf8("HOMEDRIVE");
        auto rest = env_utf8("HOMEPATH");
        if (rest) {
            return path_from_utf8(drive.value_or("") + *rest);
        }
        return {};
    } else {
        if (auto value = env_utf8("HOME")) {
            return path_from_utf8(*value);
        }
        return {};
    }
}

// os.path.expanduser of "~" and "~/rest"; "~user" is left alone.
std::filesystem::path expand_user(const std::filesystem::path& path) {
    const std::string text = path_to_utf8(path);
    if (text.empty() || text[0] != '~') {
        return path;
    }
    if (text.size() > 1 && text[1] != '/' && !(kWindows && text[1] == '\\')) {
        return path;
    }
    const std::filesystem::path base = home();
    if (base.empty()) {
        return path;
    }
    return text.size() <= 2 ? base : base / path_from_utf8(text.substr(2));
}

// Path.absolute(), without expanduser.
std::filesystem::path absolute_only(const std::filesystem::path& path) {
    if (path.is_absolute()) {
        return path;
    }
    if (path.has_root_name()) {
        // A drive-relative path: there is a working directory on each drive.
        return std::filesystem::absolute(path.root_name()) / path.relative_path();
    }
    return std::filesystem::current_path() / path;
}

} // namespace

std::filesystem::path absolute_path(const std::filesystem::path& raw) {
    return absolute_only(expand_user(raw));
}

std::optional<std::vector<std::string>> relative_parts(const std::filesystem::path& path,
                                                       const std::filesystem::path& parent) {
    const PureParts child = pure_parts(absolute_only(path));
    const PureParts base = pure_parts(absolute_only(parent));
    if (fold(child.anchor) != fold(base.anchor) || base.tail.size() > child.tail.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < base.tail.size(); ++i) {
        if (fold(base.tail[i]) != fold(child.tail[i])) {
            return std::nullopt;
        }
    }
    return std::vector<std::string>(
        child.tail.begin() + static_cast<std::ptrdiff_t>(base.tail.size()), child.tail.end());
}

bool inside(const std::filesystem::path& path, const std::filesystem::path& parent) {
    return relative_parts(path, parent).has_value();
}

bool is_absolute_text(std::string_view text) {
    return path_from_utf8(text).is_absolute();
}

std::filesystem::path join_parts(const std::filesystem::path& root, std::string_view name) {
    std::filesystem::path path = root;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = name.find('/', start);
        path /= path_from_utf8(name.substr(start, slash - start));
        if (slash == std::string_view::npos) {
            return path;
        }
        start = slash + 1;
    }
}

std::string name_of(const std::filesystem::path& path) {
    const PureParts parts = pure_parts(path);
    return parts.tail.empty() ? std::string() : parts.tail.back();
}

std::string suffix(const std::filesystem::path& path) {
    const std::string name = name_of(path);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) {
        return "";
    }
    return name.substr(dot);
}

bool exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool is_dir(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

TempDir::TempDir(const std::filesystem::path& parent, std::string_view prefix) {
    const std::filesystem::path base =
        parent.empty() ? std::filesystem::temp_directory_path() : parent;
    for (int attempt = 0;; ++attempt) {
        std::filesystem::path candidate =
            base / path_from_utf8(std::string(prefix) + random_hex(8));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            path_ = std::move(candidate);
            return;
        }
        if (error || attempt >= 100) {
            throw std::filesystem::filesystem_error(
                "Cannot create a temporary folder", candidate,
                error ? error : std::make_error_code(std::errc::file_exists));
        }
    }
}

TempDir::~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

std::uint64_t free_bytes(const std::filesystem::path& directory) {
    return std::filesystem::space(directory).free;
}

std::string read_file(const std::filesystem::path& path) {
    InputFile file(path);
    std::string bytes;
    std::vector<std::byte> buffer(1 << 16);
    while (const std::size_t got = file.read(buffer)) {
        bytes.append(reinterpret_cast<const char*>(buffer.data()), got);
    }
    return bytes;
}

bool is_valid_utf8(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t code = 0;
        char32_t minimum = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1F;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0F;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + length > bytes.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(bytes[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += length;
    }
    return true;
}

} // namespace fv::backup::detail
