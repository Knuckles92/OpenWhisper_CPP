#include "agents/detail/opencode_cleanup.h"

#include <algorithm>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <system_error>

#include "agents/acp.h"
#include "agents/agent_actions.h"
#include "agents/agent_config.h"
#include "agents/detail/headless.h"
#include "agents/detail/pyvalue.h"
#include "base/utf8.h"
#include "pyport/text.h"

namespace fv::agents::detail {
namespace {

using Clock = std::chrono::steady_clock;

class Workspace {
public:
    explicit Workspace(const std::filesystem::path& parent) {
        std::random_device random;
        for (int attempt = 0; attempt < 20; ++attempt) {
            auto candidate = std::filesystem::absolute(
                parent / path_from_utf8("opencode-cleanup-" + std::to_string(random())));
            if (std::filesystem::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::filesystem::filesystem_error("Cleanup workspace unavailable", parent,
                                                std::make_error_code(std::errc::file_exists));
    }
    ~Workspace() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    std::filesystem::path path;
};

std::map<std::string, std::string> cleanup_env(const InstalledAgents& agents,
                                               const std::filesystem::path& config,
                                               std::string_view instructions) {
    const Json deny = Json::array({{{"action", "*"}, {"resource", "*"}, {"effect", "deny"}}});
    Json personas = Json::object();
    for (const auto* name : {"build", "plan"}) {
        personas[name] = {{"system", std::string(instructions)}, {"permissions", deny}};
    }
    // V2.2.3 maps these prefixed rules to its dedicated MCP, plugin and skill policies.
    // A bare integration wildcard would also block the signed-in provider on that version.
    const Json policies = Json::array({
        {{"action", "tool.use"}, {"resource", "*"}, {"effect", "deny"}},
        {{"action", "integration.use"}, {"resource", "mcp:*"}, {"effect", "deny"}},
        {{"action", "integration.use"}, {"resource", "plugin:*"}, {"effect", "deny"}},
        {{"action", "integration.use"}, {"resource", "skill:*"}, {"effect", "deny"}},
        {{"action", "integration.use"},
         {"resource", "plugin:opencode.provider.*"},
         {"effect", "allow"}},
    });
    const Json overlay{{"agents", std::move(personas)},
                       {"default_agent", "build"},
                       {"permissions", deny},
                       {"policies", policies},
                       {"plugins", Json::array()},
                       {"instructions", Json::array()},
                       {"snapshots", false},
                       {"update", "disable"},
                       {"compaction", {{"auto", false}}}};
    auto env = agents.agent_child_env();
    // Do not take a user-supplied runtime override. Keep API credentials and the database
    // selector, since the owner's signed-in accounts live in the data store.
    std::erase_if(env, [](const auto& entry) {
        const auto name = py::lower(entry.first);
        return name.starts_with("opencode_") && name != "opencode_api_key" &&
               name != "opencode_db" && name != "opencode_auth_content";
    });
    std::erase_if(env,
                  [](const auto& entry) { return py::lower(entry.first) == "xdg_config_home"; });
    env["XDG_CONFIG_HOME"] = path_to_utf8(config);
    env["OPENCODE_CONFIG_CONTENT"] = overlay.dump();
    env["OPENCODE_DISABLE_PROJECT_CONFIG"] = "1";
    env["OPENCODE_DISABLE_GLOBAL_CONFIG"] = "1";
    env["OPENCODE_DISABLE_FREE_MODELS"] = "1";
    env["OPENCODE_DISABLE_CLAUDE_CODE"] = "1";
    env["OPENCODE_DISABLE_CLAUDE_CODE_PROMPT"] = "1";
    env["OPENCODE_DISABLE_AUTOUPDATE"] = "1";
    env["OPENCODE_DISABLE_FILEWATCHER"] = "1";
    env["OPENCODE_DISABLE_LSP_DOWNLOAD"] = "1";
    env["OPENCODE_DISABLE_AUTOCOMPACT"] = "1";
    return env;
}

std::optional<std::string> model_in(const Json& config) {
    const auto& value = get(config, "model");
    if (value.is_null()) {
        return std::nullopt;
    }
    if (value.is_string()) {
        const auto model = py::strip(value.get<std::string>());
        if (!model.empty()) {
            return model;
        }
    } else if (value.is_object()) {
        const auto& provider = get(value, "providerID");
        const auto& model = get(value, "model");
        if (provider.is_string() && model.is_string() &&
            !provider.get_ref<const std::string&>().empty() &&
            !model.get_ref<const std::string&>().empty()) {
            return provider.get<std::string>() + "/" + model.get<std::string>();
        }
    }
    throw AcpError("Invalid configured cleanup model.");
}

struct OwnerConfiguration {
    std::string model;
    std::set<std::string, std::less<>> provider_overrides;

    void collect(const Json& config, bool use_default_model) {
        if (use_default_model && model.empty()) {
            model = model_in(config).value_or(std::string());
        }
        for (const auto* key : {"providers", "provider"}) {
            const auto& providers = get(config, key);
            if (!providers.is_object()) {
                continue;
            }
            for (const auto& [id, settings] : providers.items()) {
                if (!settings.empty()) {
                    provider_overrides.insert(id);
                }
            }
        }
    }

    bool overrides(std::string_view model_id) const {
        return provider_overrides.contains(model_id.substr(0, model_id.find('/')));
    }
};

OwnerConfiguration owner_configuration(const AgentConfig& config, bool use_default_model) {
    // Inspect the selected model and provider IDs. Never pass the owner's endpoint, plugin,
    // MCP or instruction configuration to the child.
    OwnerConfiguration result;
    if (const auto inline_config = config.env("OPENCODE_CONFIG_CONTENT");
        inline_config && !inline_config->empty()) {
        const auto parsed = Json::parse(strip_jsonc(*inline_config));
        if (!parsed.is_object()) {
            throw AcpError("Invalid OpenCode configuration.");
        }
        result.collect(parsed, use_default_model);
    }
    if (const auto file = config.env("OPENCODE_CONFIG"); file && !file->empty()) {
        const auto path = path_from_utf8(*file);
        const auto parsed = path.is_absolute() ? read_json_object(path, true) : std::nullopt;
        if (!parsed) {
            throw AcpError("Invalid OpenCode configuration.");
        }
        result.collect(*parsed, use_default_model);
    }
    const auto collect_dir = [&](const std::filesystem::path& directory) {
        for (const char* name : {"opencode.jsonc", "opencode.json", "config.json"}) {
            if (const auto parsed = read_json_object(directory / name, true)) {
                result.collect(*parsed, use_default_model);
            }
        }
    };
    if (const auto directory = config.env("OPENCODE_CONFIG_DIR");
        directory && !directory->empty()) {
        const auto path = path_from_utf8(*directory);
        if (!path.is_absolute()) {
            throw AcpError("Invalid OpenCode configuration directory.");
        }
        collect_dir(path);
    }
    collect_dir(config.opencode_config_dir());
    return result;
}

AskResult failed(const CancelToken& cancel) {
    return {.ok = false,
            .answer = {},
            .message = "OpenCode did not complete transcript cleanup.",
            .stopped = cancel.cancelled()};
}

} // namespace

AskResult ask_opencode_cleanup(InstalledAgents& agents, const InstalledAgent& agent,
                               std::string_view text, std::string_view system_prompt,
                               std::string_view model, std::string_view effort,
                               Clock::time_point deadline, const CancelToken& cancel) {
    const auto remaining = [&] {
        const double seconds = std::chrono::duration<double>(deadline - Clock::now()).count();
        if (cancel.cancelled() || seconds <= 0) {
            throw AcpError("Cleanup ended.");
        }
        return seconds;
    };
    try {
        (void)remaining();
        const Workspace workspace(agents.agent_workspace_dir().parent_path());
        const auto cwd = workspace.path / "workspace";
        const auto config = workspace.path / "config";
        std::filesystem::create_directory(cwd);
        std::filesystem::create_directory(config);
        const auto instructions = std::string(system_prompt) + std::string(kCleanupClosing);
        auto env = cleanup_env(agents, config, instructions);
        const auto configured = owner_configuration(agents.config(), model.empty());
        const auto selected = model.empty() ? std::string_view(configured.model) : model;
        const auto model_id = selected.substr(0, selected.find('#'));
        if (!selected.empty() && !valid_model(model_id)) {
            throw AcpError("Invalid configured cleanup model.");
        }
        if (configured.overrides(model_id)) {
            throw AcpError("Cleanup model requires an owner-configured provider.");
        }
        auto answer = std::make_shared<AcpAnswer>();
        AcpConnection connection(
            agents.launcher(), {path_to_utf8(agent.path), "acp"}, std::move(env), cwd,
            [answer](const std::string& method, const Json& params) {
                answer->on_notification(method, params);
            },
            refuse_acp_request, AcpDiagnostics::private_data);
        (void)remaining();
        connection.start();
        std::string session_id;
        bool can_delete = false;
        AskResult result;
        try {
            const auto info = acp_initialize(connection, remaining(), cancel);
            if (get(info, "protocolVersion") != ACP_PROTOCOL_VERSION) {
                throw AcpError("Unsupported ACP protocol version.");
            }
            can_delete =
                get(get(info, "agentCapabilities"), "sessionCapabilities").contains("delete");
            const auto session = connection.request(
                "session/new", {{"cwd", path_to_utf8(cwd)}, {"mcpServers", Json::array()}},
                remaining(), cancel);
            const auto& id = get(session, "sessionId");
            if (!id.is_string() || id.get_ref<const std::string&>().empty()) {
                throw AcpError("Cleanup session unavailable.");
            }
            session_id = id.get<std::string>();
            // A configured or explicitly selected model must still be offered. Do not silently
            // choose a different provider when an ambient custom provider was isolated.
            const auto wanted = opencode_model(session, model_id, {});
            if (!wanted) {
                throw AcpError("Cleanup model unavailable.");
            }
            if (wanted->empty() && !configured.provider_overrides.empty()) {
                const auto current = acp_config_option(session, "model");
                const auto& value = get(current, "currentValue");
                if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
                    configured.overrides(value.get_ref<const std::string&>())) {
                    throw AcpError("Cleanup default model requires an owner-configured provider.");
                }
            }
            const auto set_option = [&](const char* key, const std::string& value) {
                return connection.request(
                    "session/set_config_option",
                    {{"sessionId", session_id}, {"configId", key}, {"value", value}}, remaining(),
                    cancel);
            };
            Json options = session;
            if (!wanted->empty() &&
                get(acp_config_option(session, "model"), "currentValue") != *wanted) {
                auto changed = set_option("model", *wanted);
                if (get(changed, "configOptions").is_array() && !changed["configOptions"].empty()) {
                    options = std::move(changed);
                }
            }
            const auto efforts = acp_option_values(options, "effort");
            if (!effort.empty() &&
                std::find(efforts.begin(), efforts.end(), effort) != efforts.end()) {
                (void)set_option("effort", std::string(effort));
            }
            const auto reply = connection.request(
                "session/prompt",
                {{"sessionId", session_id},
                 {"prompt", Json::array({{{"type", "text"}, {"text", std::string(text)}}})}},
                remaining(), cancel);
            (void)remaining();
            result = opencode_result(reply, answer->text());
        } catch (const std::exception&) {
            if (!session_id.empty()) {
                connection.notify("session/cancel", {{"sessionId", session_id}});
            }
        }
        if (can_delete && !session_id.empty()) {
            try {
                // Try to remove the transient session even if the prompt was canceled.
                (void)connection.request("session/delete", {{"sessionId", session_id}}, 0.25);
            } catch (const std::exception&) {
            }
        }
        connection.close(0.1);
        return result.ok && !cancel.cancelled() ? result : failed(cancel);
    } catch (const std::exception&) {
        return failed(cancel);
    }
}

} // namespace fv::agents::detail
