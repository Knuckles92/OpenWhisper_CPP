#include "clipboard/render.h"

#include <windows.h>

#include <QGuiApplication>
#include <QString>

namespace fv::clipboard {

namespace {

bool on_windows_clipboard() {
    return QGuiApplication::platformName() == QStringLiteral("windows");
}

std::function<void()> renderer_for(UINT format) {
    return [format] {
        if (!OpenClipboard(nullptr)) {
            return;
        }
        GetClipboardData(format);
        CloseClipboard();
    };
}

} // namespace

std::function<void()> system_text_renderer() {
    if (!on_windows_clipboard()) {
        return {};
    }
    return renderer_for(CF_UNICODETEXT);
}

std::function<void()> system_html_renderer() {
    if (!on_windows_clipboard()) {
        return {};
    }
    // The name Qt registers for text/html; the same name gives the same id.
    const UINT html_format = RegisterClipboardFormatW(L"HTML Format");
    if (html_format == 0) {
        return {};
    }
    return renderer_for(html_format);
}

} // namespace fv::clipboard
