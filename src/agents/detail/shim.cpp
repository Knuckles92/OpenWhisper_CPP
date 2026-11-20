#include "agents/detail/shim.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <system_error>

#include "agents/detail/pyvalue.h"
#include "base/utf8.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::agents::detail {

namespace {

#ifdef _WIN32
constexpr bool kWindows = true;
constexpr char kPathSeparator = ';';
#else
constexpr bool kWindows = false;
constexpr char kPathSeparator = ':';
#endif

const py::Regex& shim_program() {
    static const py::Regex regex(R"re(SET\s+"_prog=([^"]*)")re", py::IGNORECASE);
    return regex;
}

const py::Regex& shim_script() {
    static const py::Regex regex(R"re("%_prog%"\s+"%dp0%\\([^"%]+)"\s+%\*)re", py::IGNORECASE);
    return regex;
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool lexists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::symlink_status(path, error));
}

// os.path.normcase.
std::string normcase(const std::filesystem::path& path) {
    auto text = path_to_utf8(path);
    if constexpr (kWindows) {
        std::replace(text.begin(), text.end(), '/', '\\');
        return py::lower(text);
    }
    return text;
}

// os.path.abspath: absolute and normalized, with the platform's separators.
std::filesystem::path abspath(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    return absolute.lexically_normal().make_preferred();
}

// os.path.dirname of an absolute, normalized path.
std::filesystem::path dirname(const std::filesystem::path& path) {
    return path.has_relative_path() ? path.parent_path() : path;
}

// The text of a shim as open(..., encoding="utf-8", errors="replace").read(16_384) gives it: at
// most that many characters, after universal newlines.
std::optional<std::string> read_shim(const std::filesystem::path& shim) {
    std::ifstream in(shim, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    // A character is at most four bytes, and a "\r\n" pair reads as one.
    std::string bytes(SHIM_READ_MAX * 8, '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (in.bad()) {
        return std::nullopt;
    }
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    return py::slice(text_mode(bytes), 0, static_cast<std::int64_t>(SHIM_READ_MAX));
}

// shutil.which("node.exe") over `path_env`, without the search of this process's working folder
// (it is not the user's).
std::filesystem::path which_node(const std::optional<std::string>& path_env) {
    if (!path_env || path_env->empty()) {
        return {};
    }
    std::set<std::string> seen;
    std::size_t start = 0;
    for (;;) {
        const auto end = path_env->find(kPathSeparator, start);
        const auto dir = path_env->substr(start, end == std::string::npos ? end : end - start);
        if (!dir.empty() && seen.insert(normcase(path_from_utf8(dir))).second) {
            const auto candidate = path_from_utf8(dir) / "node.exe";
            std::error_code error;
            const auto status = std::filesystem::status(candidate, error);
            if (!error && std::filesystem::exists(status) &&
                !std::filesystem::is_directory(status)) {
                return candidate;
            }
        }
        if (end == std::string::npos) {
            return {};
        }
        start = end + 1;
    }
}

// glob.glob(os.path.join(base, *pattern)) for a pattern of literal names and "*": "*" matches
// every name not starting with ".", a literal name one that exists.
void glob_into(const std::filesystem::path& base, const std::vector<std::string_view>& parts,
               std::size_t index, std::vector<std::filesystem::path>& out) {
    if (index == parts.size()) {
        out.push_back(base);
        return;
    }
    const bool last = index + 1 == parts.size();
    if (parts[index] != "*") {
        const auto next = base / path_from_utf8(parts[index]);
        std::error_code error;
        if (last ? lexists(next) : std::filesystem::is_directory(next, error)) {
            glob_into(next, parts, index + 1, out);
        }
        return;
    }
    std::vector<std::filesystem::path> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(base, error), end; !error && it != end;
         it.increment(error)) {
        const auto name = path_to_utf8(it->path().filename());
        if (name.empty() || name.front() == '.') {
            continue;
        }
        std::error_code is_dir_error;
        if (!last && !it->is_directory(is_dir_error)) {
            continue;
        }
        names.push_back(it->path());
    }
    for (const auto& name : names) {
        glob_into(name, parts, index + 1, out);
    }
}

} // namespace

std::optional<ShimTarget> parse_npm_shim(std::string_view source) {
    const auto programs = shim_program().findall(source);
    const auto match = shim_script().search(source);
    if (!match || programs.empty()) {
        return std::nullopt;
    }
    for (auto program : programs) {
        std::replace(program.begin(), program.end(), '\\', '/');
        const auto slash = program.rfind('/');
        const auto base =
            py::lower(slash == std::string::npos ? program : program.substr(slash + 1));
        if (base != "node" && base != "node.exe") {
            return std::nullopt; // not npm's shim of a Node script
        }
    }
    return ShimTarget{programs, match->group(1)};
}

std::filesystem::path package_exe(const std::filesystem::path& script, std::string_view command,
                                  const std::filesystem::path& npm_folder) {
    auto root = dirname(script);
    while (!is_file(root / "package.json")) {
        const auto parent = dirname(root);
        if (parent == root || normcase(parent) == normcase(npm_folder)) {
            return {}; // the package root is below the shim's folder
        }
        root = parent;
    }
    const auto name = std::string(command) + ".exe";
    const std::array<std::vector<std::string_view>, 4> patterns{{
        {"node_modules", "*", "bin", name},
        {"node_modules", "*", "*", "bin", name},
        {"node_modules", "*", "vendor", "*", "bin", name},
        {"node_modules", "*", "*", "vendor", "*", "bin", name},
    }};
    for (const auto& pattern : patterns) {
        std::vector<std::filesystem::path> found;
        glob_into(root, pattern, 0, found);
        std::sort(found.begin(), found.end(),
                  [](const auto& a, const auto& b) { return path_to_utf8(a) < path_to_utf8(b); });
        for (const auto& path : found) {
            if (is_file(path)) {
                return path;
            }
        }
    }
    return {};
}

std::optional<std::vector<std::string>>
npm_shim_command(const std::filesystem::path& shim, const std::optional<std::string>& path_env) {
    const auto source = read_shim(shim);
    if (!source) {
        return std::nullopt;
    }
    const auto target = parse_npm_shim(*source);
    if (!target) {
        return std::nullopt;
    }
    const auto folder = dirname(abspath(shim));
    auto script = folder;
    for (const auto& part : py::split_on(target->script, "\\")) {
        script /= path_from_utf8(part);
    }
    script = script.lexically_normal().make_preferred();
    if (!is_file(script)) {
        return std::nullopt;
    }
    for (const auto& node : {folder / "node.exe", which_node(path_env)}) {
        const auto text = path_to_utf8(node);
        if (py::lower(text).ends_with(".exe") && is_file(node)) {
            return std::vector<std::string>{text, path_to_utf8(script)};
        }
    }
    const auto native = package_exe(script, path_to_utf8(shim.filename().stem()), folder);
    if (native.empty()) {
        return std::nullopt;
    }
    return std::vector<std::string>{path_to_utf8(native)};
}

} // namespace fv::agents::detail
