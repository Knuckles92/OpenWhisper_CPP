#include "base/detail/home.h"

#include <filesystem>

#include "base/utf8.h"

namespace fv::detail {

namespace {

bool is_nt_sep(char c) {
    return c == '\\' || c == '/';
}

bool has_nt_drive(std::string_view path) {
    return path.size() >= 2 && path[1] == ':';
}

// ntpath.join of two parts, for HOMEDRIVE and HOMEPATH.
std::string nt_join(std::string_view head, std::string_view tail) {
    if (has_nt_drive(tail)) {
        return std::string(tail);
    }
    if (!tail.empty() && is_nt_sep(tail.front())) {
        return std::string(has_nt_drive(head) ? head.substr(0, 2) : std::string_view()) +
               std::string(tail);
    }
    if (head.empty() || is_nt_sep(head.back()) || (head.size() == 2 && has_nt_drive(head))) {
        return std::string(head) + std::string(tail);
    }
    return std::string(head) + "\\" + std::string(tail);
}

// ntpath.split, as basename and dirname see it.
std::pair<std::string, std::string> nt_split(std::string_view path) {
    const std::size_t drive = has_nt_drive(path) ? 2 : 0;
    std::size_t i = path.size();
    while (i > drive && !is_nt_sep(path[i - 1])) {
        --i;
    }
    std::string_view head = path.substr(0, i);
    const std::string_view tail = path.substr(i);
    std::size_t keep = head.size();
    while (keep > drive && is_nt_sep(head[keep - 1])) {
        --keep;
    }
    if (keep > drive) {
        head = head.substr(0, keep);
    }
    return {std::string(head), std::string(tail)};
}

} // namespace

std::string expanduser_nt(std::string_view path, const EnvReader& env) {
    if (!path.starts_with('~')) {
        return std::string(path);
    }
    std::size_t i = 1;
    while (i < path.size() && !is_nt_sep(path[i])) {
        ++i;
    }
    std::string userhome;
    if (auto profile = env.get("USERPROFILE")) {
        userhome = *profile;
    } else if (auto homepath = env.get("HOMEPATH")) {
        userhome = nt_join(env.get("HOMEDRIVE").value_or(""), *homepath);
    } else {
        return std::string(path);
    }
    if (i != 1) {
        const std::string_view target_user = path.substr(1, i - 1);
        const auto current_user = env.get("USERNAME");
        if (!current_user || target_user != *current_user) {
            auto [parent, name] = nt_split(userhome);
            if (!current_user || *current_user != name) {
                return std::string(path);
            }
            userhome = nt_join(parent, target_user);
        }
    }
    return userhome + std::string(path.substr(i));
}

std::string expanduser_posix(std::string_view path, const EnvReader& env) {
    if (!path.starts_with('~')) {
        return std::string(path);
    }
    std::size_t i = path.find('/', 1);
    if (i == std::string_view::npos) {
        i = path.size();
    }
    // The password database belongs to the process, not to an environment handed in: pinned
    // without HOME, "~" stays as written instead of becoming the real user's home.
    const auto from_passwd = [&env](std::optional<std::string_view> user) {
        return env.is_process() ? passwd_home(user) : std::nullopt;
    };
    std::string userhome;
    if (i == 1) {
        if (auto home = env.get("HOME")) {
            userhome = *home;
        } else if (auto entry = from_passwd(std::nullopt)) {
            userhome = *entry;
        } else {
            return std::string(path);
        }
    } else if (auto entry = from_passwd(path.substr(1, i - 1))) {
        userhome = *entry;
    } else {
        return std::string(path);
    }
    while (!userhome.empty() && userhome.back() == '/') {
        userhome.pop_back();
    }
    std::string result = userhome + std::string(path.substr(i));
    return result.empty() ? std::string("/") : result;
}

std::string expanduser(std::string_view path, const EnvReader& env) {
#if defined(_WIN32)
    return expanduser_nt(path, env);
#else
    return expanduser_posix(path, env);
#endif
}

std::string abspath(std::string_view path) {
    namespace fs = std::filesystem;
    fs::path result = fs::absolute(path_from_utf8(path)).lexically_normal();
    // lexically_normal keeps a trailing separator as an empty last element; Python drops it.
    while (!result.empty() && !result.has_filename() && result != result.root_path()) {
        result = result.parent_path();
    }
    result.make_preferred();
    return path_to_utf8(result);
}

} // namespace fv::detail
