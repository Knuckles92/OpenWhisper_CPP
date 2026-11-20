#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "focus/catalog.h"
#include "text/snippets.h"

// The Qt-free copy the Personalize pages compute (ui_qt/dialogs/settings_styles.py,
// settings_dictionary.py, settings_commands.py, settings_snippets.py,
// settings_cleanup_rules.py, ui_qt/widgets/command_settings.py and snippets_panel.py). What
// those modules look up through other services (the cleanup provider, the speech engine, the
// platform) is passed in already resolved. `settings` is a settings mapping; anything else reads
// as {}. Every function is thread-safe and pure over its arguments.
namespace fv::text {

// The AI cleanup provider as the copy names it: profile_display_name (empty when unknown) and
// whether the profile runs on this computer.
struct ProviderInfo {
    std::string name;
    bool local = false;
};

// settings_dictionary._engine_support: what the dictation engine in use does with dictionary
// words. `backend` is the selected_model value; `support` is recognition::engine_support's
// answer, "" (with every field) when only a paired host that isn't connected could say.
struct EngineInfo {
    std::string backend;
    std::string name;
    std::string destination;
    std::string support;

    friend bool operator==(const EngineInfo&, const EngineInfo&) = default;
};

// settings_styles.py.
namespace styles_page {

// CATEGORY_LABELS, in app_styles::CATEGORIES order.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 4> CATEGORY_LABELS{{
    {"email", "Email"},
    {"work", "Work messages"},
    {"personal", "Personal messages"},
    {"other", "Other"},
}};
// TONES: (tone, label), in order.
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 3> TONE_LABELS{{
    {"formal", "Formal"},
    {"casual", "Casual"},
    {"very_casual", "Very casual"},
}};

bool cleanup_on(const Json& settings);
// Whether dictations read text near the cursor with these settings on this computer
// (`text_reading_supported` is focus_context.text_reading_supported()).
bool reads_text_near_cursor(const Json& settings, bool text_reading_supported);
// Why styles cannot apply now, as a short sentence, or "".
std::string styles_blocker(const Json& settings);
// "Off", "Needs AI cleanup", "On · All formal" or "On · <n> casual".
std::string rail_value(const Json& settings);
// The tone each kind of app gets, in one line for Home, which has no tone controls.
std::string home_copy(const Json& settings);
// A few of the apps `category` covers, the user's own choices first.
std::string apps_summary(std::string_view category,
                         const std::vector<focus::catalog::Override>& overrides);
// Where text near the cursor goes, in the user's own setup. The product name comes from
// identity(); `provider.name` "" reads "your provider".
std::string privacy_note(const Json& settings, bool text_reading_supported,
                         const ProviderInfo& provider);
// The "Never read from" apps: the stored names that are text, stripped, without blanks or
// repeats (ignoring case).
std::vector<std::string> excluded(const Json& settings);

} // namespace styles_page

// settings_dictionary.py.
namespace dictionary_page {

// The selected API speech endpoint, as speech_endpoints.selected_endpoint gives it.
struct Endpoint {
    std::string name;
    bool local = false;
};
// The connected remote backend, when it is remote and available.
struct RemoteBackend {
    std::string support;
    std::string name;
    std::string host_name;
};

// _engine_support for the engine in `settings`. `endpoint` is read for "api", `remote` for
// "remote"; without a name every field is "".
EngineInfo engine_info(const Json& settings, const std::optional<Endpoint>& endpoint = {},
                       const std::optional<RemoteBackend>& remote = {});

// Whether the engine in use is known to ignore dictionary hints.
bool cannot_steer(const EngineInfo& engine);
// Whether only Sounds like spellings fix words now: AI cleanup is off and the words don't reach
// the speech model, because the engine can't take them or steering is off.
bool sounds_like_only(const Json& settings, const EngineInfo& engine);
// What the dictionary does with the dictation engine in use.
std::string engine_line(const Json& settings, const EngineInfo& engine);
// dictionary.summary.
std::string rail_value(const Json& settings);
// The Home Dictionary row's line: the count, and what fixes words now.
std::string home_detail(const Json& settings, const EngineInfo& engine);

} // namespace dictionary_page

// settings_commands.py and ui_qt/widgets/command_settings.py.
namespace commands_page {

// The saved Command Mode shortcut, "" when none or not text.
std::string command_hotkey(const Json& settings);
// "<Shortcut set|No shortcut> · <n> transform(s)".
std::string rail_value(const Json& settings);
// How the Command Mode shortcut is used in the current recording mode.
std::string command_mode_hint(const Json& settings);

} // namespace commands_page

// settings_snippets.py and ui_qt/widgets/snippets_panel.py.
namespace snippets_page {

// "<n> snippet(s)", "" with none.
std::string count(const Json& settings);
// "Off", the count, or "None yet".
std::string rail_value(const Json& settings);
// The editor's preview line for the draft (trigger stripped, text stripped of line breaks).
std::string preview(const Snippet& draft);

} // namespace snippets_page

// settings_cleanup_rules.py.
namespace cleanup_rules_page {

// "No rules", "1 rule", "<n> rules".
std::string rule_count_label(std::int64_t rule_count);

} // namespace cleanup_rules_page

} // namespace fv::text
