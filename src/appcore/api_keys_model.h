#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "credentials/store.h"

// The words and lists of Settings → App → API keys (ui_qt/dialogs/settings_api_keys.py): which
// credentials the page lists, where the active key comes from, what each key is used for, and
// the rail value. The page (M2.19) reads the credential state off the GUI thread and renders
// these texts.
// Affinity: thread-safe (pure functions).
namespace fv::appcore {

// The variables with copy of their own.
inline constexpr std::string_view kOpenAiKeyEnv = "OPENAI_API_KEY";
inline constexpr std::string_view kOpenRouterKeyEnv = "OPENROUTER_API_KEY";
inline constexpr std::string_view kTypeSafeKeyEnv = "TYPESAFE_API_KEY";

// What an owner sends its key for: a text-model profile (cleanup and meeting intelligence), a
// speech endpoint (API transcription), or TypeSafe's decision service.
inline constexpr std::string_view kUseText = "text";
inline constexpr std::string_view kUseSpeech = "speech";
inline constexpr std::string_view kUseDecisions = "decisions";

// One sender of a credential variable: `owner` is its user-facing name ("OpenAI", "Groq").
struct CredentialOwner {
    std::string env_name;
    std::string owner;
    std::string use;
};

// One row of the credential combo: the variable and "<owners joined ' / '> · <ENV_NAME>".
struct CredentialEntry {
    std::string env_name;
    std::string label;
    bool operator==(const CredentialEntry&) const = default;
};

// key_entries: one row per distinct variable, in first-seen order, each listing its distinct
// owners. Python lists the text profiles, then the speech endpoints, then a fixed
// "TypeSafe · TYPESAFE_API_KEY" row; callers pass the owners in that order (text owners arrive
// with M4, TypeSafe's row with M8).
std::vector<CredentialEntry> key_entries(std::span<const CredentialOwner> owners);

// rail_value: "None needed" with no rows, else "N of M set" counting the rows whose source is
// not None.
std::string api_keys_rail_value(std::span<const CredentialEntry> entries,
                                const std::function<CredentialSource(std::string_view)>& source);

// _render_status: the status line and its tone ("success" or "warning"). `key` is the resolved
// value (masked here), `shadowed` whether the environment or .env also holds the variable while
// a key is saved (environment_shadowed). An empty `env_name` means no row is selected.
struct KeyStatus {
    std::string text;
    std::string tone;
    // check-green.svg for success, info-warning.svg otherwise.
    std::string icon;
};
KeyStatus status_text(CredentialSource source, std::string_view backend,
                      const std::optional<std::string>& key, std::string_view env_name,
                      bool shadowed = false);

// _render_uses: what the selected key is used for; "" hides the caption. Variables with copy of
// their own (OpenAI, OpenRouter, TypeSafe) say so; any other names the text owners and the
// speech endpoints that send it.
std::string uses_caption(std::string_view env_name, std::span<const CredentialOwner> owners);

// store_copy: where saved keys go, or why they cannot be saved here. `mac` adds the Keychain
// advice ("darwin" in Python).
std::string store_caption(const CredentialStatus& status, bool mac = false);
// store_detail: "Details: <reason>" for the caption's tooltip; "" when the store works.
std::string store_detail(const CredentialStatus& status);

} // namespace fv::appcore
