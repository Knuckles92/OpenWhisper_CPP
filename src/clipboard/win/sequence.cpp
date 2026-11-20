#include "clipboard/sequence.h"

#include <windows.h>

#include <QGuiApplication>
#include <QString>

namespace fv::clipboard {

SequenceSource system_clipboard_sequence() {
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        return {};
    }
    return []() -> std::optional<std::uint64_t> {
        const DWORD sequence = GetClipboardSequenceNumber();
        // Zero means this window station has no clipboard access.
        if (sequence == 0) {
            return std::nullopt;
        }
        return sequence;
    };
}

} // namespace fv::clipboard
