#pragma once

#include <functional>
#include <optional>
#include <string>

#include "base/executor.h"
#include "focus/clipboard_port.h"
#include "focus/snapshot.h"
#include "focus/synthetic_keys.h"
#include "walkthrough/session.h"

namespace fv::walkthrough {

std::optional<WalkthroughResult> latest_result(const std::filesystem::path& root);

// Two-part paste: the receiving app gets time to read the PDF before the instructions.
class WalkthroughPaste {
public:
    WalkthroughPaste(
        Executor& ui, focus::ClipboardPort& clipboard, focus::SyntheticKeys& keys,
        std::function<std::optional<focus::AppIdentity>()> identity,
        std::function<bool(std::string_view)> marker_present,
        std::function<void(std::string)> report,
        std::function<bool()> accessibility_trusted = [] { return true; });
    ~WalkthroughPaste();
    WalkthroughPaste(const WalkthroughPaste&) = delete;
    WalkthroughPaste& operator=(const WalkthroughPaste&) = delete;
    bool busy() const { return busy_; }
    void paste(const WalkthroughResult& result, std::optional<focus::AppIdentity> target);
    void cancel();

private:
    void paste_instructions();
    Executor& ui_;
    focus::ClipboardPort& clipboard_;
    focus::SyntheticKeys& keys_;
    std::function<std::optional<focus::AppIdentity>()> identity_;
    std::function<bool(std::string_view)> marker_present_;
    std::function<void(std::string)> report_;
    std::function<bool()> trusted_;
    TimerHandle timer_;
    std::optional<focus::AppIdentity> target_;
    std::string marker_;
    std::string prompt_;
    bool busy_ = false;
};
} // namespace fv::walkthrough
