#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Which app has focus when a dictation starts, and the text near its caret. Knowing the app is
// native and local. Text from other apps is opt-in, is never logged or stored in history, and
// only ever reaches the AI cleanup provider the user configured. A slow, missing or failing read
// means "no context", never an error or a delayed dictation.
namespace fv::focus {

// A longer selection reads as unknown rather than cut short, so a rewrite never replaces a
// whole selection with a rewrite of its start. Code points.
inline constexpr std::size_t MAX_SELECTION_CHARS = 8000;
// What a read keeps around the caret (config.CONTEXT_BEFORE_CHARS, CONTEXT_AFTER_CHARS), in
// code points.
inline constexpr std::size_t CONTEXT_BEFORE_CHARS = 1000;
inline constexpr std::size_t CONTEXT_AFTER_CHARS = 200;
// config.CONTEXT_CAPTURE_DEADLINE_S: every capture resolves by then with whatever is known.
inline constexpr std::chrono::milliseconds CONTEXT_CAPTURE_DEADLINE{400};

// TextContext::source of a selection refused because the app is excluded.
inline constexpr std::string_view EXCLUDED_SOURCE = "excluded";

// Affinity: value type.
struct AppIdentity {
    // Windows executable name (lowercase), macOS bundle id or Linux window class.
    std::string app_id;
    std::string name;
    // nullopt where the platform does not say (Python's None).
    std::optional<std::uint32_t> pid;
    // The native window handle as Python's hex() writes it ("0x1a2b"), or "".
    std::string window;
    // Site or product derived from the window title; never the raw title.
    std::string title_hint;
    // One of our own windows.
    bool is_self = false;
    std::string platform;

    bool operator==(const AppIdentity&) const = default;
};

// Affinity: value type.
struct TextContext {
    std::string before;
    std::string selected;
    std::string after;
    // before/after really are the text around the caret.
    bool caret_known = false;
    // selected was read, so "" means nothing is selected rather than "unknown".
    bool selection_known = false;
    std::string source;
    // Reading was refused (an excluded app, or a password field), so the text must not be
    // fetched another way either, such as by a copy.
    bool blocked = false;

    bool operator==(const TextContext&) const = default;
};

// Affinity: value type.
struct FocusSnapshot {
    std::optional<AppIdentity> identity;
    std::optional<TextContext> text;

    bool operator==(const FocusSnapshot&) const = default;
};

// Whether `current` is still the app, window and site `expected` was. A browser's tabs share its
// window and pid; only the site tells them apart. Two sites the catalogue doesn't know both read
// as "".
bool same_target(const std::optional<AppIdentity>& current,
                 const std::optional<AppIdentity>& expected);

} // namespace fv::focus
