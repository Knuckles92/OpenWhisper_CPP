#include "backup/sanitize.h"

#include <array>
#include <string_view>
#include <utility>

#include "backup/backup.h"
#include "backup/detail/action_rows.h"
#include "backup/detail/fs.h"
#include "backup/detail/url.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::backup {

namespace {

// Settings whose text is the user's own content, where a URL is something they typed to paste
// later rather than an endpoint that may hold a secret.
constexpr std::string_view kUrlScrubExempt = "dictation_snippets";
// Saved hooks that run or send something; restored switched off.
constexpr std::array<std::string_view, 3> kAutomationLists = {
    "automation_webhooks", "automation_programs", "automation_filters"};
constexpr std::array<std::string_view, 9> kSecretKeyWords = {
    "password", "secret",        "private_key", "api_key", "credential",
    "token",    "authorization", "header",      "cookie"};

bool secret_key(std::string_view key) {
    const std::string lowered = py::lower(key);
    for (const auto word : kSecretKeyWords) {
        if (lowered.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

Json scrub_text(const std::string& value) {
    detail::UrlParts parts;
    try {
        parts = detail::urlsplit(value);
    } catch (const detail::UrlError&) {
        const std::string lowered = py::lower(value);
        if (lowered.starts_with("http://") || lowered.starts_with("https://")) {
            return "";
        }
        return value;
    }
    if ((parts.scheme == "http" || parts.scheme == "https") && !parts.netloc.empty()) {
        // Endpoint URLs can carry credentials in userinfo or query.
        const auto at = parts.netloc.rfind('@');
        detail::UrlParts kept;
        kept.scheme = parts.scheme;
        kept.netloc = at == std::string::npos ? parts.netloc : parts.netloc.substr(at + 1);
        kept.path = parts.path;
        return detail::urlunsplit(kept);
    }
    return value;
}

// Action kinds that run a program, call a URL or start an agent.
bool active_kind(std::string_view kind) {
    return kind == detail::kWebhook || kind == detail::kProgram || kind == detail::kAgent;
}

} // namespace

Json scrub(const Json& value, bool keep_urls) {
    if (value.is_object()) {
        Json cleaned = Json::object();
        for (const auto& [key, item] : value.items()) {
            if (!secret_key(key)) {
                cleaned[key] = scrub(item, keep_urls || key == kUrlScrubExempt);
            }
        }
        return cleaned;
    }
    if (value.is_array()) {
        Json cleaned = Json::array();
        for (const auto& item : value) {
            cleaned.push_back(scrub(item, keep_urls));
        }
        return cleaned;
    }
    if (value.is_string() && !keep_urls) {
        return scrub_text(value.get_ref<const std::string&>());
    }
    return value;
}

Json sanitize_actions(const Json& raw) {
    Json rows = Json::array();
    for (const auto& action : detail::load_action_rows(raw)) {
        Json row = scrub(action.to_json());
        if (action.kind == detail::kOpenLink && action.params.contains("template")) {
            row["params"]["template"] = action.params.at("template");
        }
        if (active_kind(action.kind)) {
            row["enabled"] = false;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Json sanitize_settings(const Json& settings) {
    // Keyring values are not in this JSON. Clear metadata that would point to missing tokens or
    // certificates, and disable services on the restored host.
    Json cleaned = settings;
    for (const char* key : {"remote_engine_client", "remote_host_devices",
                            "meeting_context_folder_path", "fv_cleanup_agent_consent"}) {
        cleaned.erase(key);
    }
    const std::pair<const char*, Json> forced[] = {
        {"remote_host_enabled", false},
        {"remote_host_model_management", false},
        {"remote_host_tailscale_trust", false},
        {"remote_host_keep_records", false},
        {"remote_host_manage_mcp", false},
        {"remote_client_history", false},
        {"remote_records_location", "local"},
        {"mcp_enabled", false},
        {"mcp_tailscale_enabled", false},
        {"meeting_server_bind", "localhost"},
        {"meeting_context_folder_enabled", false},
        {"meeting_context_folder_path", ""},
        // Automation that can act for the user comes back off: a restored backup never starts
        // programs, posts to URLs or lets agents record.
        {"api_run_actions", false},
        {"api_control_recording", false},
        {"api_text_filters", false},
        {"api_agent_prompts", false},
        {"mcp_events_enabled", false},
    };
    for (const auto& [key, value] : forced) {
        cleaned[key] = value;
    }
    for (const auto list : kAutomationLists) {
        const std::string key(list);
        if (cleaned.contains(key) && cleaned[key].is_array()) {
            for (auto& item : cleaned[key]) {
                if (item.is_object()) {
                    item["enabled"] = false;
                }
            }
        }
    }
    Json actions;
    const bool had_actions = cleaned.contains("actions");
    if (had_actions) {
        actions = cleaned["actions"];
        cleaned.erase("actions");
    }
    cleaned = scrub(cleaned);
    // dict.pop("actions", None) cannot tell a stored null from no key: neither comes back.
    if (had_actions && !actions.is_null()) {
        cleaned["actions"] = sanitize_actions(actions);
    }
    return cleaned;
}

Json read_settings(const std::filesystem::path& path) {
    if (!detail::exists(path)) {
        return Json::object();
    }
    if (detail::is_link(path) || !detail::is_file(path)) {
        throw BackupError("Settings path is unsafe");
    }
    Json settings;
    try {
        const std::string bytes = detail::read_file(path);
        if (!detail::is_valid_utf8(bytes)) {
            throw BackupError("Settings JSON cannot be read");
        }
        settings = py::loads(bytes);
    } catch (const std::filesystem::filesystem_error&) {
        throw BackupError("Settings JSON cannot be read");
    } catch (const py::ValueError&) {
        throw BackupError("Settings JSON cannot be read");
    }
    if (!settings.is_object()) {
        throw BackupError("Settings JSON must be an object");
    }
    return settings;
}

} // namespace fv::backup
