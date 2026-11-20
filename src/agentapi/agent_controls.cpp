#include "agentapi/agent_controls.h"

#include <cmath>
#include <system_error>
#include <utility>

#include "automation/permissions.h"
#include "base/desktop_session.h"
#include "base/log.h"
#include "history/titles.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "settings/enums.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "sql/db.h"

namespace fv::agentapi {

namespace {

constexpr log::Logger kLog{"services.agent_mcp.controls"};

using enums::EnumId;

SettingControl boolean(std::string_view key, std::string_view label, std::string_view group,
                       std::string_view effect = NEXT_OPERATION) {
    SettingControl control;
    control.key = key;
    control.label = label;
    control.group = group;
    control.effect = effect;
    return control;
}

SettingControl string_choice(std::string_view key, std::string_view label, std::string_view group,
                             EnumId choices, std::int64_t max_length,
                             std::string_view effect = NEXT_OPERATION) {
    SettingControl control = boolean(key, label, group, effect);
    control.kind = "string";
    control.choices = enums::values(choices);
    control.max_length = max_length;
    return control;
}

std::vector<SettingControl> make_controls() {
    std::vector<SettingControl> out;
    out.push_back(boolean(keys::auto_paste.key(), "Paste after transcription", "Dictation"));
    out.push_back(
        boolean(keys::restore_clipboard.key(), "Restore clipboard after paste", "Dictation"));
    out.push_back(boolean(keys::copy_clipboard.key(), "Copy to clipboard", "Dictation"));
    out.push_back(string_choice(keys::recording_trigger_mode.key(), "Recording shortcut mode",
                                "Dictation", EnumId::RecordingTriggerMode, 20, LIVE));
    out.push_back(string_choice(keys::local_asr_language.key(), "Dictation language", "Dictation",
                                EnumId::MeetingLanguage, 10));
    out.push_back(boolean(keys::streaming_enabled.key(), "Live preview", "Dictation", LIVE));
    {
        SettingControl size = boolean(keys::streaming_overlay_font_size.key(), "Preview font size",
                                      "Appearance", LIVE);
        size.kind = "integer";
        size.minimum = 10;
        size.maximum = 48;
        out.push_back(size);
    }
    out.push_back(string_choice(keys::streaming_overlay_layout.key(), "Preview layout",
                                "Appearance", EnumId::StreamingOverlayLayout, 10, LIVE));
    out.push_back(string_choice(keys::recording_overlay_look.key(), "Recording overlay look",
                                "Appearance", EnumId::RecordingOverlayLook, 10, LIVE));
    out.push_back(boolean(keys::recording_overlay_dot.key(), "Recording overlay live dot",
                          "Appearance", LIVE));
    out.push_back(
        boolean(keys::recording_overlay_text.key(), "Recording overlay label", "Appearance", LIVE));
    out.push_back(boolean(keys::recording_overlay_clock.key(), "Recording overlay clock",
                          "Appearance", LIVE));
    {
        SettingControl theme = string_choice(keys::ui_theme.key(), "Color theme", "Appearance",
                                             EnumId::UiTheme, 20, LIVE);
        theme.resettable = true;
        out.push_back(theme);
    }
    {
        SettingControl scale =
            boolean(keys::ui_font_scale.key(), "Interface text size", "Appearance", LIVE);
        scale.kind = "integer";
        scale.choices = enums::values(EnumId::UiFontScale);
        out.push_back(scale);
    }
    out.push_back(boolean(keys::minimize_tray.key(), "Minimize to tray on close", "Appearance"));
    out.push_back(
        boolean(keys::transcript_cleanup_enabled.key(), "Transcript cleanup", "Cleanup", LIVE));
    out.push_back(string_choice(keys::transcript_cleanup_level.key(), "Cleanup level", "Cleanup",
                                EnumId::TranscriptCleanupLevel, 10, LIVE));
    out.push_back(string_choice(keys::transcript_cleanup_reasoning.key(),
                                "Cleanup reasoning effort", "Cleanup",
                                EnumId::TranscriptCleanupReasoning, 20));
    out.push_back(string_choice(keys::meeting_language.key(), "Meeting language", "Meetings",
                                EnumId::MeetingLanguage, 10));
    out.push_back(boolean(keys::meeting_report_ribbon.key(), "Meeting report ribbon", "Meetings"));
    out.push_back(boolean(keys::meeting_report_brief.key(), "Meeting brief view", "Meetings"));
    out.push_back(boolean(keys::meeting_report_signal.key(), "Meeting signal view", "Meetings"));
    out.push_back(
        boolean(keys::confirm_history_entry_delete.key(), "Confirm history deletion", "History"));
    out.push_back(
        boolean(keys::confirm_meeting_delete.key(), "Confirm meeting deletion", "History"));
    out.push_back(boolean(keys::update_check_enabled.key(), "Check for app updates", "Updates"));
    out.push_back(
        boolean(keys::update_notify_enabled.key(), "Notify about app updates", "Updates"));
    return out;
}

bool contains_key(const Json& object, std::string_view key) {
    return object.is_object() && object.contains(std::string(key));
}

// saved.get(key) is True
bool is_true(const Json& saved, std::string_view key) {
    if (!contains_key(saved, key)) {
        return false;
    }
    const Json& value = saved.at(std::string(key));
    return value.is_boolean() && value.get<bool>();
}

Json validated_changes(const Json& changes) {
    if (!changes.is_object() || changes.empty() || changes.size() > SETTING_CONTROLS().size()) {
        throw ControlError("invalid_parameters: Supply a nonempty object of supported settings.");
    }
    Json validated = Json::object();
    for (const auto& [key, value] : changes.items()) {
        const SettingControl* control = setting_control(key);
        if (control == nullptr) {
            throw ControlError("unsupported_setting: This setting is not available to agents.");
        }
        validated[key] = control->validate(value);
    }
    return validated;
}

void save_changes(Json& saved, const Json& validated) {
    if (!saved.is_object()) {
        saved = Json::object();
    }
    for (const auto& [key, value] : validated.items()) {
        if (value.is_null()) {
            saved.erase(key);
        } else {
            saved[key] = value;
        }
    }
    if (validated.contains(std::string(keys::streaming_enabled.key()))) {
        for (const std::string_view key : keys::legacy_streaming) {
            saved.erase(std::string(key));
        }
    }
}

std::string text_of(const sql::Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return {};
}

Json json_of(const sql::Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return nullptr;
}

} // namespace

Json SettingControl::validate(const Json& value) const {
    if (value.is_null() && resettable) {
        return nullptr;
    }
    bool valid = false;
    if (kind == "boolean") {
        valid = value.is_boolean();
    } else if (kind == "integer") {
        valid = value.is_number_integer();
    } else if (kind == "number") {
        valid = value.is_number_integer() ||
                (value.is_number_float() && std::isfinite(value.get<double>()));
    } else if (kind == "string") {
        valid =
            value.is_string() &&
            static_cast<std::int64_t>(py::len(value.get_ref<const std::string&>())) <= max_length;
    }
    if (!valid) {
        throw ControlError("invalid_value: Invalid value for " + std::string(key) + ".");
    }
    if (!choices.empty()) {
        bool found = false;
        for (const Json& choice : choices) {
            found = found || py::py_equal(choice, value);
        }
        if (!found) {
            throw ControlError("invalid_value: Choose a supported value for " + std::string(key) +
                               ".");
        }
    }
    if (minimum && value.get<double>() < static_cast<double>(*minimum)) {
        throw ControlError("invalid_value: " + std::string(key) + " is below its minimum.");
    }
    if (maximum && value.get<double>() > static_cast<double>(*maximum)) {
        throw ControlError("invalid_value: " + std::string(key) + " is above its maximum.");
    }
    return value;
}

Json SettingControl::resolve(const Json& saved) const {
    if (key == keys::ui_theme.key()) {
        if (!contains_key(saved, key) && use_omarchy_ui()) {
            return "omarchy";
        }
        return keys::ui_theme.read(saved);
    }
    const Json fallback = keys::defaults_table().at(std::string(key));
    try {
        return validate(contains_key(saved, key) ? saved.at(std::string(key)) : fallback);
    } catch (const ControlError&) {
        return fallback;
    }
}

Json SettingControl::describe() const {
    Json out = Json::object();
    out["key"] = std::string(key);
    out["label"] = std::string(label);
    out["group"] = std::string(group);
    out["type"] = std::string(kind);
    out["effect"] = std::string(effect);
    if (!choices.empty()) {
        out["choices"] = choices;
    }
    if (resettable) {
        out["resettable"] = true;
    }
    if (minimum && *minimum != 0) {
        out["minimum"] = *minimum;
    }
    if (maximum && *maximum != 0) {
        out["maximum"] = *maximum;
    }
    if (max_length != 0) {
        out["max_length"] = max_length;
    }
    return out;
}

const std::vector<SettingControl>& SETTING_CONTROLS() {
    static const std::vector<SettingControl> controls = make_controls();
    return controls;
}

const SettingControl* setting_control(std::string_view key) {
    for (const SettingControl& control : SETTING_CONTROLS()) {
        if (control.key == key) {
            return &control;
        }
    }
    return nullptr;
}

std::set<std::string> writable_settings(const Json& saved) {
    const Json granted = keys::mcp_writable_settings.read(saved);
    std::set<std::string> out;
    for (const SettingControl& control : SETTING_CONTROLS()) {
        if (is_true(granted, control.key)) {
            out.emplace(control.key);
        }
    }
    return out;
}

AgentControls::AgentControls(AgentControlsOptions options) : options_(std::move(options)) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(options_.database, error);
    if (!error) {
        options_.database = std::filesystem::weakly_canonical(absolute, error);
        if (error) {
            options_.database = absolute;
        }
    }
}

Json AgentControls::saved() const {
    if (options_.settings == nullptr) {
        return Json::object();
    }
    return *options_.settings->snapshot();
}

void AgentControls::require(const Json& saved, std::string_view permission) const {
    if (options_.enabled && !options_.enabled()) {
        throw ControlError("disabled: MCP access is turned off.");
    }
    if (options_.settings == nullptr || !is_true(saved, permission)) {
        throw ControlError("permission_denied: Enable this action in Settings > API & MCP.");
    }
}

void AgentControls::changed(std::string_view kind, const Json& changes) const {
    if (!options_.on_change) {
        return;
    }
    try {
        options_.on_change(std::string(kind), changes);
    } catch (const std::exception& error) {
        // The change is already committed. A closed window must not make an agent retry a
        // successful write or expose source data.
        kLog.warning("Could not refresh the desktop after an MCP change ({}).",
                     typeid(error).name());
    }
}

Json AgentControls::capabilities() const {
    const Json saved = this->saved();
    const bool access = keys::mcp_settings_access.read(saved);
    Json out = Json::object();
    out["retitle_transcriptions"] = keys::mcp_retitle_transcriptions.read(saved);
    out["retitle_meetings"] = keys::mcp_retitle_meetings.read(saved);
    out["settings_access"] = access;
    Json writable = Json::array();
    if (access) {
        for (const std::string& key : writable_settings(saved)) {
            writable.push_back(key);
        }
    }
    out["writable_settings"] = std::move(writable);
    const auto permissions = automation::permissions::describe(saved);
    for (const auto& [name, granted] : permissions.items()) {
        out[name] = options_.allow_live_tools && granted.is_boolean() && granted.get<bool>();
    }
    out["mcp_events"] = options_.allow_live_tools && keys::mcp_events_enabled.read(saved);
    return out;
}

Json AgentControls::get_settings() const {
    const Json saved = this->saved();
    require(saved, keys::mcp_settings_access.key());
    const std::set<std::string> granted = writable_settings(saved);
    Json list = Json::array();
    for (const SettingControl& control : SETTING_CONTROLS()) {
        Json item = control.describe();
        item["value"] = control.resolve(saved);
        item["writable"] = granted.contains(std::string(control.key));
        if (control.resettable) {
            item["inherited"] = !contains_key(saved, control.key);
        }
        list.push_back(std::move(item));
    }
    Json out = Json::object();
    out["settings"] = std::move(list);
    return out;
}

Json AgentControls::update_settings(const Json& changes) {
    const Json validated = validated_changes(changes);
    if (options_.settings == nullptr) {
        throw ControlError("permission_denied: Settings access is not enabled.");
    }
    Json result;
    options_.settings->mutate([&](Json& saved) {
        // Permission checking and saving share the settings lock.
        require(saved, keys::mcp_settings_access.key());
        const std::set<std::string> granted = writable_settings(saved);
        for (const auto& [key, value] : validated.items()) {
            if (!granted.contains(key)) {
                throw ControlError(
                    "permission_denied: Enable each setting in Settings > API & MCP.");
            }
        }
        Json previous = Json::object();
        Json restore = Json::object();
        for (const auto& [key, value] : validated.items()) {
            previous[key] = setting_control(key)->resolve(saved);
        }
        for (const auto& [key, value] : validated.items()) {
            const SettingControl* control = setting_control(key);
            restore[key] =
                control->resettable && !contains_key(saved, key) ? Json(nullptr) : previous[key];
        }
        save_changes(saved, validated);
        Json updated = Json::object();
        for (const auto& [key, value] : validated.items()) {
            updated[key] = setting_control(key)->resolve(saved);
        }
        result = Json::object();
        result["updated"] = std::move(updated);
        result["previous"] = std::move(previous);
        result["restore"] = std::move(restore);
    });
    changed("settings", result.at("updated"));
    return result;
}

Json AgentControls::retitle(std::string_view kind, std::string_view record_id,
                            std::string_view title) {
    std::string normalized;
    try {
        normalized = normalize_title(title);
    } catch (const py::ValueError& error) {
        throw ControlError(std::string("invalid_title: ") + error.what());
    }
    const bool meeting = kind == "meeting";
    const Json saved = this->saved();
    require(saved,
            meeting ? keys::mcp_retitle_meetings.key() : keys::mcp_retitle_transcriptions.key());
    const std::string table = meeting ? "meeting_sessions" : "transcription_history";
    Json previous;
    {
        // ReadWrite cannot create a missing database or run startup migrations.
        sql::Connection connection =
            sql::Connection::open(options_.database, sql::OpenMode::ReadWrite);
        sql::Transaction transaction = connection.begin(sql::TxMode::Immediate);
        sql::Statement select =
            connection.prepare("SELECT title" + std::string(meeting ? ", status, state_json" : "") +
                               " FROM " + table + " WHERE id = ? AND origin_device_id IS NULL");
        select.bind(1, std::string(record_id));
        if (!select.step()) {
            throw ControlError("not_found: Local record not found.");
        }
        previous = json_of(select.column(0));
        Json state = Json::object();
        if (meeting) {
            const std::string status = text_of(select.column(1));
            if (status != "ended" && status != "failed") {
                throw ControlError(
                    "record_busy: Finish the meeting before changing its history title.");
            }
            const sql::Value raw = select.column(2);
            try {
                const std::string text = text_of(raw);
                if (!std::holds_alternative<std::nullptr_t>(raw) && !text.empty()) {
                    state = py::loads(text);
                }
                if (!state.is_object()) {
                    throw py::ValueError("not an object");
                }
            } catch (const py::PyError&) {
                throw ControlError("snapshot_unavailable: Saved meeting state cannot be updated.");
            }
        }
        select.reset();
        if (meeting && options_.meeting_renamer) {
            // The owning dashboard must update its cached state as well. Release the database
            // write lock before dispatching there.
            transaction.rollback();
            options_.meeting_renamer(std::string(record_id), normalized);
        } else {
            if (meeting) {
                state["title"] = normalized;
                py::DumpOptions dump;
                dump.ensure_ascii = false;
                sql::Statement update = connection.prepare(
                    "UPDATE meeting_sessions SET title = ?, state_json = ? WHERE id = ?");
                update.bind(1, normalized);
                update.bind(2, py::dumps(state, dump));
                update.bind(3, std::string(record_id));
                update.step();
            } else {
                sql::Statement update =
                    connection.prepare("UPDATE transcription_history SET title = ? WHERE id = ?");
                update.bind(1, normalized);
                update.bind(2, std::string(record_id));
                update.step();
            }
            transaction.commit();
        }
    }
    Json changes = Json::object();
    changes["id"] = std::string(record_id);
    changes["title"] = normalized;
    changed(kind, changes);
    Json out = Json::object();
    out["id"] = std::string(record_id);
    out["title"] = normalized;
    out["previous_title"] = std::move(previous);
    return out;
}

} // namespace fv::agentapi
