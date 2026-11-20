#include "credentials/resolve.h"

#include <utility>

#include "pyport/text.h"

namespace fv {

namespace {

bool truthy(const std::optional<std::string>& value) {
    return value.has_value() && !value->empty();
}

} // namespace

std::optional<std::string> dotenv_value(const DotEnv& dotenv, std::string_view name) {
#if defined(_WIN32)
    // process_env keys names by str.upper() on Windows, and loading .env never overwrites a
    // variable already set, so the first such entry is the one the environment holds.
    const std::string wanted = py::upper(name);
    for (auto& [key, value] : dotenv.values()) {
        if (value.has_value() && py::upper(key) == wanted) {
            return value;
        }
    }
    return std::nullopt;
#else
    return dotenv.get(name);
#endif
}

Resolved resolve_credential(CredentialStore& store, const EnvReader& env, const DotEnv& dotenv,
                            std::string_view name) {
    if (name.empty()) {
        return {std::nullopt, CredentialSource::None};
    }
    if (auto stored = store.get(name); truthy(stored)) {
        return {std::move(stored), CredentialSource::Store};
    }
    auto from_env = env.get(name);
    if (truthy(from_env)) {
        const auto from_file = dotenv_value(dotenv, name);
        const auto source =
            from_file == from_env ? CredentialSource::DotEnv : CredentialSource::Environment;
        return {std::move(from_env), source};
    }
    if (from_env.has_value()) {
        // Set but empty (possible outside Windows): loading .env never overwrites a variable
        // that is set, so OpenWhisper's lookup ends with nothing here.
        return {std::nullopt, CredentialSource::None};
    }
    if (auto from_file = dotenv_value(dotenv, name); truthy(from_file)) {
        return {std::move(from_file), CredentialSource::DotEnv};
    }
    return {std::nullopt, CredentialSource::None};
}

CredentialSource credential_source(CredentialStore& store, const EnvReader& env,
                                   const DotEnv& dotenv, std::string_view name) {
    if (name.empty()) {
        return CredentialSource::None;
    }
    if (truthy(store.get(name))) {
        return CredentialSource::Store;
    }
    const auto from_env = env.get(name);
    auto from_file = dotenv_value(dotenv, name);
    if (!truthy(from_file)) {
        from_file.reset();
    }
    if (truthy(from_env)) {
        return from_file == from_env ? CredentialSource::DotEnv : CredentialSource::Environment;
    }
    if (from_file.has_value()) {
        return CredentialSource::DotEnv;
    }
    return CredentialSource::None;
}

bool environment_shadowed(CredentialStore& store, const EnvReader& env, const DotEnv& dotenv,
                          std::string_view name) {
    if (name.empty() || !truthy(store.get(name))) {
        return false;
    }
    return truthy(env.get(name)) || truthy(dotenv_value(dotenv, name));
}

std::string_view credential_source_name(CredentialSource source) {
    switch (source) {
    case CredentialSource::Store:
        return "stored";
    case CredentialSource::Environment:
        return "environment";
    case CredentialSource::DotEnv:
        return "dotenv";
    case CredentialSource::None:
        break;
    }
    return "none";
}

} // namespace fv
