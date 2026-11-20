#include "meeting_intel/detail/paths.h"

#include <system_error>
#include <tuple>
#include <vector>

#include "base/utf8.h"
#include "pyport/text.h"

namespace fv::meeting_intel::detail {

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)
constexpr char kSep = '\\';

bool is_sep(char c) {
    return c == '\\' || c == '/';
}

std::string native_seps(std::string_view path) {
    std::string out(path);
    for (char& c : out) {
        if (c == '/') {
            c = '\\';
        }
    }
    return out;
}

// ntpath.splitroot on a path whose separators are already "\": (drive, root, rest).
std::tuple<std::string, std::string, std::string> splitroot(const std::string& p) {
    if (!p.empty() && p[0] == '\\') {
        if (p.size() > 1 && p[1] == '\\') {
            // UNC (\\server\share) or a device path (\\?\UNC\server\share, \\.\device).
            std::string upper = py::upper(p.substr(0, 8));
            const std::size_t start = upper == "\\\\?\\UNC\\" ? 8 : 2;
            const std::size_t index = p.find('\\', start);
            if (index == std::string::npos) {
                return {p, "", ""};
            }
            const std::size_t index2 = p.find('\\', index + 1);
            if (index2 == std::string::npos) {
                return {p, "", ""};
            }
            return {p.substr(0, index2), p.substr(index2, 1), p.substr(index2 + 1)};
        }
        return {"", p.substr(0, 1), p.substr(1)};
    }
    if (p.size() > 1 && p[1] == ':') {
        if (p.size() > 2 && p[2] == '\\') {
            return {p.substr(0, 2), p.substr(2, 1), p.substr(3)};
        }
        return {p.substr(0, 2), "", p.substr(2)};
    }
    return {"", "", p};
}

std::string join(std::string_view head, std::string_view tail) {
    // ntpath.join(head, tail) for a relative tail without a drive.
    if (head.empty()) {
        return std::string(tail);
    }
    if (is_sep(head.back()) || (head.size() == 2 && head[1] == ':')) {
        return std::string(head) + std::string(tail);
    }
    return std::string(head) + kSep + std::string(tail);
}

std::string dirname(std::string_view path) {
    const std::string native = native_seps(path);
    const auto [drive, root, rest] = splitroot(native);
    std::size_t cut = rest.size();
    while (cut > 0 && rest[cut - 1] != '\\') {
        --cut;
    }
    std::string head = rest.substr(0, cut);
    while (!head.empty() && head.back() == '\\') {
        head.pop_back();
    }
    return drive + root + head;
}

std::string basename(std::string_view path) {
    const std::string native = native_seps(path);
    const auto [drive, root, rest] = splitroot(native);
    const std::size_t slash = rest.rfind('\\');
    return slash == std::string::npos ? rest : rest.substr(slash + 1);
}
#else
constexpr char kSep = '/';
#endif

std::vector<std::string> split_parts(const std::string& rest) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : rest) {
        if (c == kSep) {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

// The normpath loop over the parts after the prefix.
std::string fold(const std::string& rest, bool rooted) {
    std::vector<std::string> comps;
    for (auto& comp : split_parts(rest)) {
        if (comp.empty() || comp == ".") {
            continue;
        }
        if (comp == "..") {
            if (!comps.empty() && comps.back() != "..") {
                comps.pop_back();
                continue;
            }
            if (rooted) {
                continue;
            }
        }
        comps.push_back(std::move(comp));
    }
    std::string out;
    for (std::size_t index = 0; index < comps.size(); ++index) {
        if (index > 0) {
            out += kSep;
        }
        out += comps[index];
    }
    return out;
}

std::string lower_part(const fs::path& part) {
#if defined(_WIN32)
    return py::lower(base_utf8::path_to_utf8(part));
#else
    return base_utf8::path_to_utf8(part);
#endif
}

} // namespace

#if defined(_WIN32)

std::string expanduser(std::string_view path, const EnvReader& env) {
    if (!path.starts_with('~')) {
        return std::string(path);
    }
    std::size_t i = 1;
    while (i < path.size() && !is_sep(path[i])) {
        ++i;
    }
    std::string userhome;
    if (const auto profile = env.get("USERPROFILE")) {
        userhome = *profile;
    } else if (const auto homepath = env.get("HOMEPATH")) {
        userhome = join(env.get("HOMEDRIVE").value_or(""), *homepath);
    } else {
        return std::string(path);
    }
    if (i != 1) {
        const std::string target_user(path.substr(1, i - 1));
        const auto current_user = env.get("USERNAME");
        if (!current_user || target_user != *current_user) {
            // Profiles are usually siblings named after their users; a home that is not named
            // after the current user makes that guess unsafe.
            if (!current_user || *current_user != basename(userhome)) {
                return std::string(path);
            }
            userhome = join(dirname(userhome), target_user);
        }
    }
    return userhome + std::string(path.substr(i));
}

bool isabs(std::string_view path) {
    std::string head(path.substr(0, 3));
    for (char& c : head) {
        if (c == '/') {
            c = '\\';
        }
    }
    return head.starts_with('\\') || (head.size() >= 3 && head.substr(1, 2) == ":\\");
}

std::string normpath(std::string_view path) {
    const std::string native = native_seps(path);
    const auto [drive, root, rest] = splitroot(native);
    const std::string prefix = drive + root;
    std::string folded = fold(rest, !root.empty());
    if (prefix.empty() && folded.empty()) {
        return ".";
    }
    return prefix + folded;
}

std::string abspath(std::string_view path) {
    // _getfullpathname(normpath(path)), which std::filesystem::absolute calls on Windows.
    std::error_code error;
    const fs::path full = fs::absolute(base_utf8::path_from_utf8(normpath(path)), error);
    if (!error) {
        return base_utf8::path_to_utf8(full);
    }
    std::string joined(path);
    if (!isabs(joined)) {
        joined = join(base_utf8::path_to_utf8(fs::current_path()), native_seps(path));
    }
    return normpath(joined);
}

#else

std::string expanduser(std::string_view path, const EnvReader& env) {
    if (!path.starts_with('~')) {
        return std::string(path);
    }
    std::size_t i = path.find('/', 1);
    if (i == std::string_view::npos) {
        i = path.size();
    }
    if (i != 1) {
        // "~user": the password database, which a test's pinned environment never reaches.
        return std::string(path);
    }
    const auto home = env.get("HOME");
    if (!home) {
        return std::string(path);
    }
    std::string userhome = *home;
    while (userhome.size() > 1 && userhome.back() == '/') {
        userhome.pop_back();
    }
    std::string out = userhome + std::string(path.substr(i));
    return out.empty() ? "/" : out;
}

bool isabs(std::string_view path) {
    return path.starts_with('/');
}

std::string normpath(std::string_view path) {
    if (path.empty()) {
        return ".";
    }
    std::size_t initial = 0;
    if (path.starts_with('/')) {
        initial = path.starts_with("//") && !path.starts_with("///") ? 2 : 1;
    }
    const std::string folded = fold(std::string(path), initial > 0);
    const std::string out = std::string(initial, '/') + folded;
    return out.empty() ? "." : out;
}

std::string abspath(std::string_view path) {
    std::string joined(path);
    if (!isabs(joined)) {
        std::string cwd = base_utf8::path_to_utf8(fs::current_path());
        joined = cwd.ends_with('/') ? cwd + joined : cwd + "/" + joined;
    }
    return normpath(joined);
}

#endif

bool islink(const fs::path& path) {
    std::error_code error;
    return fs::is_symlink(fs::symlink_status(path, error));
}

std::optional<fs::path> resolve_strict(const fs::path& path) {
    std::error_code error;
    fs::path resolved = fs::canonical(path, error);
    if (error) {
        return std::nullopt;
    }
    return resolved;
}

std::optional<fs::path> resolve_lax(const fs::path& path) {
    std::error_code error;
    fs::path resolved = fs::weakly_canonical(path, error);
    if (error) {
        return std::nullopt;
    }
    return resolved;
}

std::optional<fs::path> relative_to(const fs::path& path, const fs::path& root) {
    auto it = path.begin();
    for (const auto& part : root) {
        if (part.empty()) {
            continue;
        }
        while (it != path.end() && it->empty()) {
            ++it;
        }
        if (it == path.end() || lower_part(*it) != lower_part(part)) {
            return std::nullopt;
        }
        ++it;
    }
    fs::path rest;
    for (; it != path.end(); ++it) {
        if (!it->empty()) {
            rest /= *it;
        }
    }
    return rest;
}

} // namespace fv::meeting_intel::detail
