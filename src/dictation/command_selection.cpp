#include "dictation/command_selection.h"

#include <exception>
#include <format>
#include <utility>

#include "base/identity.h"
#include "base/log.h"
#include "focus/catalog.h"
#include "pyport/text.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.runtime.command"};

} // namespace

std::string password_message() {
    return std::format("{} doesn't read password fields", identity().product);
}

std::string clipboard_hidden_message() {
    return std::format("{} can't read selected text on this desktop", identity().product);
}

SelectionSource::SelectionSource() : future_(promise_.get_future().share()) {}

bool SelectionSource::resolve(SelectionValue value) {
    std::lock_guard lock(mutex_);
    if (resolved_) {
        return false;
    }
    resolved_ = true;
    promise_.set_value(std::move(value));
    return true;
}

bool SelectionSource::done() const {
    std::lock_guard lock(mutex_);
    return resolved_;
}

std::optional<UnreadSelection> refusal(const std::optional<focus::FocusSnapshot>& snapshot) {
    if (!snapshot || !snapshot->text || !snapshot->text->blocked) {
        return std::nullopt;
    }
    if (snapshot->text->source != focus::EXCLUDED_SOURCE) {
        return UnreadSelection{password_message()};
    }
    const auto kind = focus::catalog::classify(snapshot->identity);
    const std::string name = kind && !kind->name.empty() ? kind->name : std::string("this app");
    return UnreadSelection{std::format("{} doesn't read text in {}", identity().product, name)};
}

std::optional<std::string> known_selection(const std::optional<focus::FocusSnapshot>& snapshot) {
    if (!snapshot || !snapshot->text || !snapshot->text->selection_known) {
        return std::nullopt;
    }
    return snapshot->text->selected;
}

bool looks_like_copied_line(std::string_view text) {
    if (!text.ends_with('\n')) {
        return false;
    }
    const std::string line = py::rstrip(text.substr(0, text.size() - 1), "\r");
    return line.find('\n') == std::string::npos;
}

CopySelection clipboard_copy(focus::ClipboardPort& clipboard, focus::SyntheticKeys& keys,
                             Executor& ui) {
    return [&clipboard, &keys, &ui](std::function<void(std::optional<std::string>)> copied) {
        ui.post([&clipboard, &keys, copied = std::move(copied)] {
            clipboard.capture_selection([&keys] { keys.send_copy(); }, copied,
                                        focus::SELECTION_TIMEOUT);
        });
    };
}

void read_selection(const std::shared_ptr<focus::FocusRequest>& focus, focus::SyntheticKeys& keys,
                    const CopySelection& copy, const std::function<void(SelectionValue)>& done,
                    const focus::SurfaceLookup& surface) {
    try {
        const auto snapshot = resolved(focus, focus::CONTEXT_CAPTURE_DEADLINE);
        if (auto refused = refusal(snapshot)) {
            done(std::move(*refused));
            return;
        }
        if (auto known = known_selection(snapshot)) {
            done(std::move(*known));
            return;
        }
        const std::optional<focus::AppIdentity> identity =
            snapshot ? snapshot->identity : std::nullopt;
        if (focus::is_terminal(identity, surface)) {
            done(std::string());
            return;
        }
        const bool copy_line = focus::copies_line_without_selection(identity, surface);
        if (!keys.wait_for_modifiers_released()) {
            kLog.info("Shortcut keys still held; not copying the selection");
            done(UnreadSelection{std::string(KEYS_HELD_MESSAGE)});
            return;
        }
        copy([done, copy_line](std::optional<std::string> text) {
            if (!text) {
                done(UnreadSelection{clipboard_hidden_message()});
                return;
            }
            done(copy_line && looks_like_copied_line(*text) ? std::string() : std::move(*text));
        });
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't read the selection");
        done(std::string());
    } catch (...) {
        kLog.error("Couldn't read the selection");
        done(std::string());
    }
}

} // namespace fv::dictation
