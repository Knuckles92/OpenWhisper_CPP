#include "walkthrough/paste.h"

#include <chrono>
#include <random>
#include <utility>

#include "walkthrough/library.h"
#include "walkthrough/storage.h"

namespace fv::walkthrough {
std::optional<WalkthroughResult> latest_result(const std::filesystem::path& root) {
    for (const auto& folder : entries(root)) {
        if (std::filesystem::is_regular_file(folder / MANIFEST_NAME))
            return load_result(folder);
    }
    return std::nullopt;
}
WalkthroughPaste::WalkthroughPaste(Executor& ui, focus::ClipboardPort& clipboard,
                                   focus::SyntheticKeys& keys,
                                   std::function<std::optional<focus::AppIdentity>()> identity,
                                   std::function<bool(std::string_view)> marker_present,
                                   std::function<void(std::string)> report,
                                   std::function<bool()> trusted)
    : ui_(ui), clipboard_(clipboard), keys_(keys), identity_(std::move(identity)),
      marker_present_(std::move(marker_present)), report_(std::move(report)),
      trusted_(std::move(trusted)) {}
WalkthroughPaste::~WalkthroughPaste() {
    cancel();
}
void WalkthroughPaste::paste(const WalkthroughResult& result,
                             std::optional<focus::AppIdentity> target) {
    if (busy_)
        return;
    if (!trusted_()) {
        report_("Walkthrough not pasted: enable Accessibility permission to auto-paste");
        return;
    }
    if (!target || target->is_self || !focus::same_target(identity_(), target)) {
        report_("Walkthrough not pasted: focus changed or the destination is unavailable");
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(result.pdf, error)) {
        report_("Walkthrough not pasted: the PDF is missing");
        return;
    }
    target_ = std::move(target);
    prompt_ = result.clipboard("agent_task").second;
    std::random_device random;
    marker_.clear();
    for (int i = 0; i < 4; ++i)
        marker_ += std::to_string(random()) + ":";
    try {
        if (keys_.modifiers_held()) {
            cancel();
            report_("Walkthrough not pasted: modifier keys are held");
            return;
        }
        focus::ClipboardFiles files;
        files.paths = {result.pdf};
        files.marker = marker_;
        if (!clipboard_.write_files(std::move(files)))
            throw std::runtime_error("couldn't copy the PDF");
        busy_ = true;
        keys_.send_paste();
        timer_ = ui_.post_after(std::chrono::milliseconds(800), [this] { paste_instructions(); });
    } catch (const std::exception& error) {
        cancel();
        report_("Couldn't paste the walkthrough PDF: " + std::string(error.what()));
    }
}
void WalkthroughPaste::paste_instructions() {
    if (!busy_)
        return;
    try {
        if (!focus::same_target(identity_(), target_))
            report_("PDF paste sent; instructions stopped because focus changed");
        else if (!marker_present_(marker_))
            report_("PDF paste sent; instructions stopped because the clipboard changed");
        else if (keys_.modifiers_held())
            report_("PDF paste sent; instructions stopped because modifier keys are held");
        else {
            if (!clipboard_.write_text(prompt_))
                throw std::runtime_error("couldn't copy the instructions");
            keys_.send_paste();
            report_("PDF and instructions pasted; review them before sending");
        }
    } catch (const std::exception& error) {
        report_("PDF paste sent; couldn't paste instructions: " + std::string(error.what()));
    }
    cancel();
}
void WalkthroughPaste::cancel() {
    timer_.cancel();
    busy_ = false;
    marker_.clear();
    target_.reset();
    prompt_.clear();
}
} // namespace fv::walkthrough
