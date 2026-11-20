#include "base/desktop_session.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "base/detail/home.h"
#include "base/detail/pytext.h"
#include "base/platform_info.h"
#include "base/utf8.h"

namespace fv {

namespace {

bool is_linux(std::string_view platform) {
    return platform.starts_with("linux");
}

// Path.home(): expanduser("~"); Python raises when no home can be found, which leaves "~".
std::filesystem::path home(const EnvReader& env) {
    return path_from_utf8(detail::expanduser("~", env));
}

std::filesystem::path dir_from(const EnvReader& env, std::string_view name,
                               const std::filesystem::path& fallback) {
    // os.environ.get(name, fallback): a set but empty variable is used, and Path("") is ".".
    if (auto value = env.get(name)) {
        return value->empty() ? std::filesystem::path(".") : path_from_utf8(*value);
    }
    return fallback;
}

struct CacheKey {
    std::string install_path;
    std::vector<std::filesystem::path> theme_paths;
    bool operator==(const CacheKey&) const = default;
};

} // namespace

bool is_wayland_session(std::string_view platform, const EnvReader& env) {
    if (!is_linux(platform)) {
        return false;
    }
    return detail::ascii_lower(env.get("XDG_SESSION_TYPE").value_or("")) == "wayland" ||
           !env.get("WAYLAND_DISPLAY").value_or("").empty();
}

bool is_wayland_session() {
    return is_wayland_session(python_platform(), system_env());
}

bool is_native_wayland_session(std::string_view platform, const EnvReader& env) {
    if (!is_linux(platform)) {
        return false;
    }
    const std::string type = env.get("XDG_SESSION_TYPE").value_or("");
    return detail::ascii_lower(detail::py_strip(type)) == "wayland" ||
           !env.get("WAYLAND_DISPLAY").value_or("").empty();
}

bool is_native_wayland_session() {
    return is_native_wayland_session(python_platform(), system_env());
}

std::array<std::filesystem::path, 2> omarchy_theme_paths(const EnvReader& env) {
    const std::filesystem::path user_home = home(env);
    const auto state = dir_from(env, "XDG_STATE_HOME", user_home / ".local" / "state");
    const auto config = dir_from(env, "XDG_CONFIG_HOME", user_home / ".config");
    const std::filesystem::path theme =
        std::filesystem::path("omarchy") / "current" / "theme" / "colors.toml";
    return {state / theme, config / theme};
}

bool detect_omarchy(std::string_view install_path,
                    std::span<const std::filesystem::path> theme_paths,
                    const std::filesystem::path& os_release) {
    if (!install_path.empty()) {
        return true;
    }
    for (const auto& path : theme_paths) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error)) {
            return true;
        }
    }
    std::ifstream file(os_release, std::ios::binary);
    if (!file) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    for (const std::string_view line : detail::py_splitlines(text)) {
        std::string_view stripped = detail::py_strip(line);
        if (stripped == "ID=\"omarchy\"") {
            return true;
        }
        // .strip('"') removes quotes at both ends, however many.
        while (!stripped.empty() && stripped.front() == '"') {
            stripped.remove_prefix(1);
        }
        while (!stripped.empty() && stripped.back() == '"') {
            stripped.remove_suffix(1);
        }
        if (stripped == "ID=omarchy") {
            return true;
        }
    }
    return false;
}

bool use_omarchy_ui(std::string_view platform, const EnvReader& env, const AppIdentity& app) {
    const std::string override_value =
        detail::ascii_lower(env.get(std::string(app.env_prefix) + "UI").value_or("auto"));
    if (override_value == "omarchy" || override_value == "classic") {
        return override_value == "omarchy";
    }
    if (!is_linux(platform)) {
        return false;
    }
    const auto paths = omarchy_theme_paths(env);
    CacheKey key{env.get("OMARCHY_PATH").value_or(""), {paths.begin(), paths.end()}};

    // functools.lru_cache(maxsize=4): most recent first, the least recent dropped.
    static std::mutex mutex;
    static std::list<std::pair<CacheKey, bool>> cache;
    {
        std::lock_guard lock(mutex);
        const auto hit = std::find_if(cache.begin(), cache.end(),
                                      [&](const auto& entry) { return entry.first == key; });
        if (hit != cache.end()) {
            cache.splice(cache.begin(), cache, hit);
            return hit->second;
        }
    }
    const bool detected = detect_omarchy(key.install_path, key.theme_paths);
    std::lock_guard lock(mutex);
    cache.emplace_front(std::move(key), detected);
    if (cache.size() > 4) {
        cache.pop_back();
    }
    return detected;
}

bool use_omarchy_ui() {
    return use_omarchy_ui(python_platform(), system_env(), identity());
}

} // namespace fv
