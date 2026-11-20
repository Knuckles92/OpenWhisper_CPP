#include "process/process_env.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "base/identity.h"
#include "credentials/dotenv.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv {

namespace {

bool is_ascii(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

char ascii_upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

// Python's str.upper, without the allocation for ASCII names.
std::string upper_name(std::string_view name) {
    if (!is_ascii(name)) {
        return py::upper(name);
    }
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(), ascii_upper);
    return out;
}

std::vector<std::string> split_path_list(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(separator, start);
        const auto part = value.substr(start, end == std::string_view::npos ? end : end - start);
        if (!part.empty()) {
            parts.emplace_back(part);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

} // namespace

bool EnvNameLess::operator()(std::string_view a, std::string_view b) const {
    if constexpr (ProcessEnv::kWindows) {
        if (is_ascii(a) && is_ascii(b)) {
            return std::lexicographical_compare(
                a.begin(), a.end(), b.begin(), b.end(),
                [](char x, char y) { return ascii_upper(x) < ascii_upper(y); });
        }
        return upper_name(a) < upper_name(b);
    } else {
        return a < b;
    }
}

ProcessEnv::ProcessEnv(MutableEnv& env, bool windows) : env_(env), windows_(windows) {}

ProcessEnv& ProcessEnv::instance() {
    // Never destroyed: children may be started while static objects are torn down.
    static auto* env = new ProcessEnv(system_mutable_env());
    return *env;
}

std::string ProcessEnv::key(std::string_view name) const {
    return windows_ ? upper_name(name) : std::string(name);
}

void ProcessEnv::remember(std::string_view name, std::optional<std::string> original) {
    std::lock_guard lock(mutex_);
    original_.try_emplace(key(name), std::move(original));
}

void ProcessEnv::set_for_app(std::string_view name, std::string_view value, bool keep_existing) {
    auto original = env_.get(name);
    if (keep_existing && original) {
        return;
    }
    env_.set(name, value);
    remember(name, std::move(original));
}

std::vector<std::string>
ProcessEnv::load_for_app(std::span<const std::pair<std::string, std::string>> values) {
    std::vector<std::string> refused;
    for (const auto& [name, value] : values) {
        try {
            const auto current = env_.get(name);
            if (!current) {
                env_.set(name, value);
            } else if (*current != value) {
                continue;
            }
        } catch (const std::invalid_argument&) {
            refused.push_back(name);
            continue;
        }
        remember(name, std::nullopt);
    }
    return refused;
}

void ProcessEnv::adopt_dotenv(const DotEnv& dotenv) {
    for (const auto& [name, value] : dotenv.values()) {
        if (!value) {
            continue;
        }
        const auto current = env_.get(name);
        if (current && *current == *value) {
            remember(name, std::nullopt);
        }
    }
}

void ProcessEnv::prepend_for_app(std::string_view name, std::span<const std::string> entries) {
    std::vector<std::string> first;
    for (const auto& entry : entries) {
        if (!entry.empty()) {
            first.push_back(entry);
        }
    }
    if (first.empty()) {
        return;
    }
    const char separator = windows_ ? ';' : ':';
    auto original = env_.get(name);
    std::string joined;
    const auto append = [&](const std::string& part) {
        if (!joined.empty()) {
            joined += separator;
        }
        joined += part;
    };
    for (const auto& entry : first) {
        append(entry);
    }
    for (const auto& part : split_path_list(original.value_or(""), separator)) {
        if (std::find(first.begin(), first.end(), part) == first.end()) {
            append(part);
        }
    }
    env_.set(name, joined);
    remember(name, std::move(original));
}

EnvMap ProcessEnv::external_env(const std::map<std::string, std::string>& extra) const {
    EnvMap env;
    for (auto& [name, value] : env_.items()) {
        env.insert_or_assign(std::move(name), std::move(value));
    }
    std::map<std::string, std::optional<std::string>> original;
    {
        std::lock_guard lock(mutex_);
        original = original_;
    }
    for (const auto& [name, value] : original) {
        if (!value) {
            env.erase(name);
        } else {
            env.insert_or_assign(name, *value);
        }
    }
    for (const auto& [name, value] : extra) {
        env.erase(name);
        env.emplace(name, value);
    }
    return env;
}

void ProcessEnv::drop(EnvMap& env, std::span<const std::string> names) const {
    std::set<std::string> unwanted;
    for (const auto& name : names) {
        unwanted.insert(key(name));
    }
    for (auto it = env.begin(); it != env.end();) {
        if (unwanted.contains(key(it->first))) {
            it = env.erase(it);
        } else {
            ++it;
        }
    }
}

void ProcessEnv::forget() {
    std::lock_guard lock(mutex_);
    original_.clear();
}

std::vector<std::string> internal_env_names() {
    std::vector<std::string> names;
    for (const auto* id : {&identity(), &legacy_identity()}) {
        for (const std::string_view suffix : {"SIDECAR_TOKEN", "LLM_API_KEY", "LLM_BASE_URL"}) {
            names.push_back(std::string(id->env_prefix) + std::string(suffix));
        }
    }
    return names;
}

EnvMap child_env(const ProcessEnv& env, const std::map<std::string, std::string>& extra) {
    auto result = env.external_env();
    env.drop(result, internal_env_names());
    for (const auto& [name, value] : extra) {
        result.erase(name);
        result.emplace(name, value);
    }
    return result;
}

EnvMap child_env(const std::map<std::string, std::string>& extra) {
    return child_env(ProcessEnv::instance(), extra);
}

std::map<std::string, std::string> to_spec_env(const EnvMap& env) {
    return {env.begin(), env.end()};
}

void check_env_assignment(std::string_view name, std::string_view value, bool windows) {
    const bool bad_name = name.empty() || name.find('=', windows ? 1 : 0) != std::string_view::npos;
    if (bad_name) {
        throw std::invalid_argument("illegal environment variable name");
    }
    if (name.find('\0') != std::string_view::npos || value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument(windows ? "embedded null character" : "embedded null byte");
    }
    if (windows) {
        // "name=value" in UTF-16 units, as _wputenv receives it.
        std::size_t units = 1;
        for (const auto text : {name, value}) {
            for (const char32_t cp : py::to_code_points(text)) {
                units += cp > 0xFFFF ? 2 : 1;
            }
        }
        constexpr std::size_t max_env = 32767;
        if (units > max_env) {
            throw std::invalid_argument("the environment variable is longer than 32767 characters");
        }
    }
}

} // namespace fv
