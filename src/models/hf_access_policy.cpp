#include "models/hf_access_policy.h"

#include <algorithm>
#include <stdexcept>

#include "base/log.h"
#include "pyport/text.h"
#include "settings/keys.h"

namespace fv::models {

namespace {

constexpr log::Logger kLog("services.settings");

constexpr std::string_view kOfflineEnv = "HF_HUB_OFFLINE";
constexpr std::array<std::string_view, 4> kOfflineTruthy{"1", "on", "true", "yes"};

bool is_policy(std::string_view value) {
    return std::ranges::find(hf_access_policy::kAll, value) != hf_access_policy::kAll.end();
}

// Python's truthiness of a saved value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    if (value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

} // namespace

std::string load_hf_access_policy(SettingsStore& store) {
    const auto settings = store.snapshot();
    const std::string_view policy_key = keys::hf_access_policy.key();
    const std::string_view legacy_key = keys::hf_hub_offline.key();
    // Raw, not typed: an invalid saved policy is rewritten below.
    const auto policy = settings->find(policy_key);
    const bool has_policy = policy != settings->end();
    if (has_policy && policy->is_string() && is_policy(policy->get_ref<const std::string&>())) {
        return policy->get<std::string>();
    }
    const auto legacy = settings->find(legacy_key);
    const bool has_legacy = legacy != settings->end();
    const std::string migrated(has_legacy && truthy(*legacy) ? hf_access_policy::kNever
                                                             : hf_access_policy::kAsk);
    if (has_legacy || (has_policy && !policy->is_null())) {
        try {
            store.update(Json{{std::string(policy_key), migrated}}, {legacy_key});
            kLog.info("Migrated HuggingFace access policy to '{}'", migrated);
        } catch (const std::exception& error) {
            kLog.warning("Failed to persist HF policy migration: {}", error.what());
        }
    }
    return migrated;
}

void save_hf_access_policy(SettingsStore& store, std::string_view policy) {
    if (!is_policy(policy)) {
        throw std::invalid_argument("Invalid HF access policy '" + std::string(policy) +
                                    "'. Valid values: ['ask', 'always', 'never']");
    }
    store.update(Json{{std::string(keys::hf_access_policy.key()), std::string(policy)}},
                 {keys::hf_hub_offline.key()});
    kLog.info("HuggingFace access policy saved: {}", policy);
}

bool is_hf_hub_offline_env_set(const EnvReader& env) {
    const std::string value = py::lower(py::strip(env.get(kOfflineEnv).value_or(std::string())));
    return std::ranges::find(kOfflineTruthy, value) != kOfflineTruthy.end();
}

} // namespace fv::models
