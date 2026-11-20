#include "ui_controller/clipboard_service.h"

#include <exception>
#include <utility>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "clipboard/temporary_clipboard.h"
#include "focus/synthetic_keys.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

} // namespace

ClipboardService::ClipboardService(std::shared_ptr<clipboard::TemporaryClipboard> clipboard,
                                   std::shared_ptr<focus::SyntheticKeys> keys, QObject* parent)
    : QObject(parent), clipboard_(std::move(clipboard)), keys_(std::move(keys)) {
    connect(clipboard_.get(), &clipboard::TemporaryClipboard::restoreFailed, this,
            [this](const QString&) {
                Q_EMIT statusRequested(QString::fromUtf8(kRestoreFailedStatus));
            });
}

ClipboardService::~ClipboardService() = default;

bool ClipboardService::copyToClipboard(const QString& text, const QString& html) {
    return clipboard_->write_text(qt::toUtf8(text), qt::toUtf8(html));
}

void ClipboardService::prefetch() {
    clipboard_->request_prefetch();
}

void ClipboardService::discardPrefetch() {
    clipboard_->discard_prefetch();
}

focus::StageResult ClipboardService::stageTranscriptForPaste(const std::string& text,
                                                             const std::optional<std::string>& html,
                                                             bool restore) {
    std::optional<std::string> rich;
    if (html && !html->empty()) {
        rich = html;
    }
    return clipboard_->stage_text(text, std::move(rich), restore);
}

void ClipboardService::captureSelection(std::function<void(std::optional<std::string>)> callback,
                                        std::optional<std::chrono::milliseconds> timeout) {
    std::shared_ptr<focus::SyntheticKeys> keys = keys_;
    clipboard_->capture_selection(
        [keys] {
            if (keys != nullptr) {
                keys->send_copy();
            }
        },
        std::move(callback), timeout.value_or(focus::SELECTION_TIMEOUT));
}

bool ClipboardService::scheduleClipboardRestore(const focus::StageResult& stage) {
    if (!stage.lease) {
        return false;
    }
    return clipboard_->schedule_restore(*stage.lease, focus::CLIPBOARD_RESTORE_DELAY);
}

bool ClipboardService::commitTranscriptClipboard(const focus::StageResult& stage,
                                                 const std::string& text) {
    if (!stage.lease) {
        return stage.written;
    }
    return clipboard_->commit_text(*stage.lease, text);
}

void ClipboardService::cleanup() {
    try {
        clipboard_->cleanup();
    } catch (const std::exception& error) {
        kLog.warning("Error restoring clipboard during cleanup: {}", error.what());
    }
}

} // namespace fv
