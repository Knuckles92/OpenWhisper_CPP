#include "base/paths.h"

#include <optional>
#include <string>

#include "base/detail/home.h"
#include "base/utf8.h"

namespace fv {

namespace {

constexpr std::string_view kEnvFileName = ".env"; // config.py ENV_FILE_NAME

// config.py:121-136 local_app_dir.
std::filesystem::path per_user_dir(const AppIdentity& app, const EnvReader& env) {
    std::string base;
#if defined(_WIN32)
    if (auto local = env.get("LOCALAPPDATA"); local && !local->empty()) {
        base = *local;
    } else {
        base = detail::expanduser("~", env);
    }
#elif defined(__APPLE__)
    base = detail::expanduser("~/Library/Application Support", env);
#else
    // os.environ.get with a default: a set but empty XDG_DATA_HOME is used as is.
    if (auto xdg = env.get("XDG_DATA_HOME")) {
        base = *xdg;
    } else {
        base = detail::expanduser("~/.local/share", env);
    }
#endif
    return path_from_utf8(base) / path_from_utf8(app.data_dir_name);
}

// The override of config.py:184-186: `os.path.abspath(os.path.expanduser(override))` when the
// variable is set and not empty.
std::optional<std::filesystem::path> data_dir_override(const AppIdentity& app,
                                                       const EnvReader& env) {
    const auto value = env.get(std::string(app.env_prefix) + "DATA_DIR");
    if (!value || value->empty()) {
        return std::nullopt;
    }
    return path_from_utf8(detail::abspath(detail::expanduser(*value, env)));
}

} // namespace

Paths Paths::resolve(const AppIdentity& app, const EnvReader& env) {
    Paths paths;
    paths.local_app_dir_ = per_user_dir(app, env);
    paths.data_root_ = data_dir_override(app, env).value_or(paths.local_app_dir_);
    std::filesystem::create_directories(paths.data_root_);

    const AppIdentity& legacy = legacy_identity();
    if (auto chosen = data_dir_override(legacy, env)) {
        paths.openwhisper_roots_.push_back(std::move(*chosen));
    }
    auto legacy_per_user = per_user_dir(legacy, env);
    if (paths.openwhisper_roots_.empty() || paths.openwhisper_roots_.front() != legacy_per_user) {
        paths.openwhisper_roots_.push_back(std::move(legacy_per_user));
    }
    return paths;
}

const std::filesystem::path& Paths::data_root() const {
    return data_root_;
}

std::filesystem::path Paths::local_app_dir() const {
    return local_app_dir_;
}

std::filesystem::path Paths::components_root() const {
    return local_app_dir_ / "components";
}

std::filesystem::path Paths::speech_models_root() const {
    return local_app_dir_ / "speech-models";
}

std::filesystem::path Paths::user_file(std::string_view name) const {
    return data_root_ / path_from_utf8(name);
}

std::filesystem::path Paths::env_file() const {
    return user_file(kEnvFileName);
}

std::vector<std::filesystem::path> Paths::openwhisper_roots() const {
    return openwhisper_roots_;
}

std::filesystem::path expand_user(const std::filesystem::path& path, const EnvReader& env) {
    return path_from_utf8(detail::expanduser(path_to_utf8(path), env));
}

} // namespace fv
