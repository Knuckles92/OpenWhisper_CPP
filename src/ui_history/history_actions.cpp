#include "ui_history/history_actions.h"

#include <exception>
#include <optional>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "history/manager.h"
#include "ui_history/history_store.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.history_actions"};

QString words(const char* utf8) {
    return QString::fromUtf8(utf8);
}

// _report: the status line, plus a notice near the pointer.
void report(HistoryActionsUi& ui, const QString& status, const QString& notice = {},
            bool done = false) {
    ui.setStatus(status);
    ui.showNotice(notice.isEmpty() ? status : notice, done);
}

// _last_edited_dictation: the last dictation if AI cleanup changed it, else nullopt with the
// status saying why.
std::optional<HistoryEntry> lastEditedDictation(HistoryActionsUi& ui, HistoryStore& history,
                                                const char* nothingYet) {
    std::optional<HistoryEntry> entry;
    try {
        entry = history.last_dictation();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not read the last dictation");
        report(ui, words(history_actions::kReadFailed));
        return std::nullopt;
    }
    if (!entry) {
        report(ui, words(nothingYet));
        return std::nullopt;
    }
    if (entry_version(*entry).empty()) {
        report(ui, words(history_actions::kNotEdited));
        return std::nullopt;
    }
    return entry;
}

} // namespace

void HistoryActionsUi::showNotice(const QString& /*text*/, bool /*done*/) {}

void paste_last_original(HistoryActionsUi& ui, HistoryStore& history) {
    const std::optional<HistoryEntry> entry =
        lastEditedDictation(ui, history, history_actions::kNothingYet);
    if (!entry) {
        return;
    }
    if (!ui.canPasteNow()) {
        report(ui, words(history_actions::kUnavailable));
        return;
    }
    if (!ui.pasteTextNow(qt::toQString(entry->raw_text.value_or("")))) {
        return; // The runtime said why: busy, or the paste itself failed.
    }
    bool marked = false;
    try {
        history.use_version(entry->id, ORIGINAL_VERSION);
        marked = true;
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not mark the last dictation as original");
    }
    if (marked) {
        ui.refreshHistory();
    }
    report(ui, words(history_actions::kPasted), QStringLiteral("Original pasted"), true);
}

void copy_last_original(HistoryActionsUi& ui, HistoryStore& history) {
    const std::optional<HistoryEntry> entry =
        lastEditedDictation(ui, history, history_actions::kNothingToCopy);
    if (!entry) {
        return;
    }
    if (!ui.copyToClipboard(qt::toQString(entry->raw_text.value_or("")))) {
        report(ui, words(history_actions::kCopyFailed));
        return;
    }
    report(ui, words(history_actions::kCopied), QStringLiteral("Copied — paste with Ctrl+V"), true);
}

} // namespace fv
