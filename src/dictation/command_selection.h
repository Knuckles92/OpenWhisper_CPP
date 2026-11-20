#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/executor.h"
#include "dictation/job.h"
#include "focus/capture_service.h"
#include "focus/clipboard_port.h"
#include "focus/snapshot.h"
#include "focus/synthetic_keys.h"

// Where Command Mode and transforms find the selected text (services/runtime/command.py 46-133,
// 289-334): UI Automation's answer when the focus capture knows it, else a copy once the
// shortcut's keys are up, never in a terminal, an app the user excluded or a password field.
// Selections are never logged.
namespace fv::dictation {

inline constexpr std::string_view NO_PROVIDER_MESSAGE = "Set up AI cleanup to use Command Mode";
inline constexpr std::string_view KEYS_HELD_MESSAGE = "Release the shortcut keys, then try again";
// "<product> doesn't read password fields".
std::string password_message();
// "<product> can't read selected text on this desktop".
std::string clipboard_hidden_message();
// How long the worker waits for the selection after the transcript is in: the modifier wait, the
// copy's own timeout and the capture deadline, all started at the stop, which the post-roll and
// the final decode usually outlast (1.9 s).
inline constexpr std::chrono::milliseconds SELECTION_WAIT =
    focus::MODIFIER_WAIT + focus::SELECTION_TIMEOUT + focus::CONTEXT_CAPTURE_DEADLINE;

// Nothing to paste, for a reason the user acts on. Not a failed transcription: the runtime shows
// the message as it is and keeps no instruction audio.
class CommandRefused : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The answer a selection future resolves to, set at most once (_resolve: a second value, from a
// read that raced another, is dropped).
// Affinity: thread-safe.
class SelectionSource {
public:
    SelectionSource();
    SelectionSource(const SelectionSource&) = delete;
    SelectionSource& operator=(const SelectionSource&) = delete;

    SelectionFuture future() const { return future_; }
    // Resolves the future unless it already is; true when this call did.
    bool resolve(SelectionValue value);
    bool done() const;

private:
    mutable std::mutex mutex_;
    bool resolved_ = false;
    std::promise<SelectionValue> promise_;
    SelectionFuture future_;
};

// _refusal: why the capture refused to read this app's selection ("<product> doesn't read
// password fields", "<product> doesn't read text in <app>"), or nullopt.
std::optional<UnreadSelection> refusal(const std::optional<focus::FocusSnapshot>& snapshot);
// _known_selection: the selection UI Automation read, or nullopt when it is unknown.
std::optional<std::string> known_selection(const std::optional<focus::FocusSnapshot>& snapshot);
// _looks_like_copied_line: whether `text` is what a copy-line editor (VS Code, Visual Studio,
// JetBrains IDEs, Sublime Text, Zed) copies with nothing selected: exactly one line ending in one
// line break.
bool looks_like_copied_line(std::string_view text);

// Reads the focused app's selection through a copy on the UI thread, calling `copied` there with
// the text, or nullopt when this clipboard can't see other apps' copies.
using CopySelection = std::function<void(std::function<void(std::optional<std::string>)> copied)>;

// The copy every reader uses: posts to `ui` and runs clipboard.capture_selection with Ctrl+C from
// `keys` and focus::SELECTION_TIMEOUT. `clipboard`, `keys` and `ui` outlive every copy.
CopySelection clipboard_copy(focus::ClipboardPort& clipboard, focus::SyntheticKeys& keys,
                             Executor& ui);

// _read_selection: finds the selected text, then done(text). UI Automation's answer wins, even an
// empty one (waiting at most CONTEXT_CAPTURE_DEADLINE for `focus`). Otherwise the text is copied
// once the shortcut's modifiers are up, except in a terminal, where a copy shortcut would
// interrupt the running program ("" then). `done` gets an UnreadSelection instead when the app or
// field must not be read, the modifiers stayed down (Ctrl+Alt+C types a character on AltGr
// layouts, Ctrl+Shift+C opens DevTools), or the clipboard can't see other apps' copies; nothing
// is copied then. A copy-line editor's one-line copy counts as nothing selected. Blocks: never on
// the UI thread. A failure gives done(""). `surface` replaces the app catalogue (tests).
void read_selection(const std::shared_ptr<focus::FocusRequest>& focus, focus::SyntheticKeys& keys,
                    const CopySelection& copy, const std::function<void(SelectionValue)>& done,
                    const focus::SurfaceLookup& surface = {});

} // namespace fv::dictation
