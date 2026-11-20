#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "base/json.h"
#include "focus/snapshot.h"
#include "hotkeys/grammar.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"

// Shared helpers of the cleanup-prompt, profile, transform, style and copy tests: the JSON forms
// the golden case files use (tools/parity/cases/services.cleanup_prompts.py, ...).
namespace fv::prompt_test {

// Settings with AI cleanup on, plus `values`.
inline Json on(const Json& values = Json::object()) {
    Json settings = Json::object();
    settings["transcript_cleanup_enabled"] = true;
    for (const auto& [key, value] : values.items()) {
        settings[key] = value;
    }
    return settings;
}

inline focus::AppIdentity identity(std::string app_id, std::string name,
                                   std::string title_hint = "", bool is_self = false) {
    focus::AppIdentity out;
    out.app_id = std::move(app_id);
    out.name = std::move(name);
    out.title_hint = std::move(title_hint);
    out.is_self = is_self;
    return out;
}

inline focus::FocusSnapshot snapshot(std::optional<focus::AppIdentity> app) {
    focus::FocusSnapshot out;
    out.identity = std::move(app);
    return out;
}

// The case files' snapshot: {"identity": {AppIdentity fields} or null}, or null for none.
inline std::optional<focus::FocusSnapshot> snapshot_from(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    focus::FocusSnapshot out;
    const Json& app = fields.at("identity");
    if (!app.is_null()) {
        focus::AppIdentity made;
        made.app_id = app.at("app_id").get<std::string>();
        made.name = app.at("name").get<std::string>();
        if (!app.at("pid").is_null()) {
            made.pid = app.at("pid").get<std::uint32_t>();
        }
        made.window = app.at("window").get<std::string>();
        made.title_hint = app.at("title_hint").get<std::string>();
        made.is_self = app.at("is_self").get<bool>();
        made.platform = app.at("platform").get<std::string>();
        out.identity = std::move(made);
    }
    return out;
}

// Aggregates in full, so GCC's -Wmissing-field-initializers stays quiet.
inline text::CleanupProfile profile(std::string id, std::string name, std::string instructions,
                                    std::string hotkey = "", bool use_learned_rules = true) {
    return {std::move(id), std::move(name), std::move(instructions), std::move(hotkey),
            use_learned_rules};
}

inline text::CleanupProfile profile_from(const Json& fields) {
    return {fields.at("id").get<std::string>(), fields.at("name").get<std::string>(),
            fields.at("instructions").get<std::string>(), fields.at("hotkey").get<std::string>(),
            fields.at("use_learned_rules").get<bool>()};
}

inline text::Transform transform_from(const Json& fields) {
    return {fields.at("id").get<std::string>(), fields.at("name").get<std::string>(),
            fields.at("instruction").get<std::string>(), fields.at("hotkey").get<std::string>()};
}

// What the golden vectors were recorded with: OpenWhisper on Windows.
inline text::ShortcutContext windows_context() {
    text::ShortcutContext context;
    context.vocabulary = hotkeys::Vocabulary::Windows;
    context.platform = "win32";
    return context;
}

// A fresh folder, removed with everything in it at the end of the test.
class TempDir {
public:
    TempDir() {
        std::random_device seed;
        path_ = std::filesystem::temp_directory_path() /
                ("fv-text-test-" + std::to_string(std::mt19937_64(seed())()));
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace fv::prompt_test
