#include "backup/import_credentials.h"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>

#include "base/log.h"
#include "credentials/store.h"
#include "pyport/text.h"

namespace fv::backup {

namespace {

constexpr log::Logger logger("services.import_credentials");

// The providers' keys OpenWhisper reads by these names (services/text_llm.py,
// services/speech_endpoints.py, services/typesafe.py).
constexpr std::array<std::string_view, 7> kProviderKeys = {
    "OPENAI_API_KEY",       "OPENROUTER_API_KEY", "GROQ_API_KEY",    "OPENCODE_GO_API_KEY",
    "OPENCODE_ZEN_API_KEY", "LMSTUDIO_API_KEY",   "TYPESAFE_API_KEY"};

// Its own tokens, after its environment prefix: the remote engine's (remote_engine/settings.py
// TOKEN_CREDENTIAL), the MCP server's (agent_mcp/runtime.py CREDENTIAL_NAME), the History API's
// (agent_api/cli.py) and the channel bridge's (automation/channel_bridge.py TOKEN_ENV).
constexpr std::array<std::string_view, 4> kTokenSuffixes = {"REMOTE_ENGINE_TOKEN", "MCP_TOKEN",
                                                            "API_TOKEN", "TOKEN"};

// services/speech_endpoints.py ENDPOINTS that are configurable (not hosted), in display order:
// only these read an api_key_env saved in api_transcription_servers.
constexpr std::array<std::string_view, 5> kSpeechServers = {"speaches", "localai", "vllm",
                                                            "whisper_cpp", "custom"};

// services/text_llm.py LOCAL_SERVER_DEFAULT_URLS, in its order: each reads "<id>_api_key_env".
constexpr std::array<std::string_view, 5> kLocalTextServers = {"ollama", "lm_studio", "vllm",
                                                               "llama_cpp", "localai"};

constexpr std::string_view kActionSecret = "ACTION_SECRET_";
constexpr std::string_view kWebhookSecret = "WEBHOOK_SECRET_";

void add(std::vector<std::string>& names, std::string name) {
    if (!name.empty() && std::ranges::find(names, name) == names.end()) {
        names.push_back(std::move(name));
    }
}

const Json* list_of(const Json& settings, std::string_view key) {
    if (!settings.is_object()) {
        return nullptr;
    }
    const auto it = settings.find(std::string(key));
    return it != settings.end() && it->is_array() ? &*it : nullptr;
}

// The "id" of a saved row, when it is a string.
std::optional<std::string> row_id(const Json& row) {
    if (!row.is_object()) {
        return std::nullopt;
    }
    const auto it = row.find("id");
    if (it == row.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

// text_llm.validate_api_key_env of row[key]: stripped, then ^[A-Z_][A-Z0-9_]*$; nullopt
// otherwise.
std::optional<std::string> api_key_env(const Json& row, const std::string& key = "api_key_env") {
    if (!row.is_object()) {
        return std::nullopt;
    }
    const auto it = row.find(key);
    if (it == row.end() || !it->is_string()) {
        return std::nullopt;
    }
    const std::string name = py::strip(it->get_ref<const std::string&>());
    const auto head = [](char c) {
        return (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto tail = [&](char c) {
        return head(c) || (c >= '0' && c <= '9');
    };
    if (name.empty() || !head(name.front()) || !std::all_of(name.begin() + 1, name.end(), tail)) {
        return std::nullopt;
    }
    return name;
}

} // namespace

std::vector<std::string> legacy_credential_names(const Json& legacy_settings,
                                                 const AppIdentity& from) {
    const std::string prefix(from.env_prefix);
    std::vector<std::string> names;
    for (const auto key : kProviderKeys) {
        add(names, std::string(key));
    }
    for (const auto suffix : kTokenSuffixes) {
        add(names, prefix + std::string(suffix));
    }
    if (const Json* actions = list_of(legacy_settings, "actions")) {
        for (const auto& row : *actions) {
            if (const auto id = row_id(row); id && !py::strip(*id).empty()) {
                add(names, prefix + std::string(kActionSecret) + *id);
            }
        }
    }
    if (const Json* hooks = list_of(legacy_settings, "automation_webhooks")) {
        for (const auto& row : *hooks) {
            if (const auto id = row_id(row)) {
                const std::string stripped = py::strip(*id);
                if (!stripped.empty()) {
                    add(names, prefix + std::string(kWebhookSecret) + stripped);
                }
            }
        }
    }
    if (const Json* profiles = list_of(legacy_settings, "text_llm_profiles")) {
        for (const auto& row : *profiles) {
            if (auto name = api_key_env(row)) {
                add(names, std::move(*name));
            }
        }
    }
    if (legacy_settings.is_object()) {
        const auto servers = legacy_settings.find("api_transcription_servers");
        if (servers != legacy_settings.end() && servers->is_object()) {
            for (const auto id : kSpeechServers) {
                const auto entry = servers->find(std::string(id));
                if (entry != servers->end()) {
                    if (auto name = api_key_env(*entry)) {
                        add(names, std::move(*name));
                    }
                }
            }
        }
        for (const auto id : kLocalTextServers) {
            if (auto name = api_key_env(legacy_settings, std::string(id) + "_api_key_env")) {
                add(names, std::move(*name));
            }
        }
    }
    return names;
}

std::string imported_credential_name(std::string_view name, const AppIdentity& from,
                                     const AppIdentity& to) {
    if (name.starts_with(from.env_prefix)) {
        return std::string(to.env_prefix) + std::string(name.substr(from.env_prefix.size()));
    }
    return std::string(name);
}

CopyReport copy_credentials(CredentialStore& from, CredentialStore& to, const Json& legacy_settings,
                            const AppIdentity& from_app, const AppIdentity& to_app) {
    CopyReport report;
    const auto names = legacy_credential_names(legacy_settings, from_app);
    // get() answers nullopt for an unusable store as for a missing key: asked first, so that a
    // store nothing can be read from is not reported as one that holds nothing.
    if (!from.status().available) {
        report.failed = names;
        report.unreadable = true;
        logger.warning("Could not read the saved keys to copy: the store cannot be used");
        return report;
    }
    for (const auto& name : names) {
        std::optional<std::string> value;
        try {
            value = from.get(name);
        } catch (const std::exception&) {
            report.failed.push_back(name);
            continue;
        }
        if (!value) {
            report.skipped.push_back(name);
            continue;
        }
        const std::string target = imported_credential_name(name, from_app, to_app);
        try {
            to.set(target, *value);
            report.copied.push_back(target);
        } catch (const std::exception&) {
            // The message of a refused key may describe the value; only the name is kept.
            report.failed.push_back(name);
        }
    }
    logger.info("Copied {} saved keys; {} not saved there, {} failed", report.copied.size(),
                report.skipped.size(), report.failed.size());
    return report;
}

} // namespace fv::backup
