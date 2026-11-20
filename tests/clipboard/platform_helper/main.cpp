// The other process of test_platform.cpp: TemporaryClipboard on Qt's "windows" platform, which
// the offscreen test executable cannot load. It writes the real clipboard, so only the
// [interactive] test starts it; the clipboard it found is written back before it exits.
//
// Exit status 0 when every check passed, 1 when one failed (each failure is printed), 2 when
// the windows platform did not load.

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

#include <QClipboard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMimeData>
#include <QTimer>

#include "clipboard/render.h"
#include "clipboard/sequence.h"
#include "clipboard/snapshot.h"
#include "clipboard/temporary_clipboard.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAILED: %s\n", what);
    }
}

// CF_UNICODETEXT as another app reads it. The clipboard may be open for a moment elsewhere.
std::optional<std::wstring> win32_text() {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (OpenClipboard(nullptr)) {
            std::optional<std::wstring> text;
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
                    text = std::wstring(chars);
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
            return text;
        }
        Sleep(10);
    }
    return std::nullopt;
}

void run_events(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "windows");
    QGuiApplication app(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("windows")) {
        std::fprintf(stderr, "The windows platform plugin did not load.\n");
        return 2;
    }
    using namespace fv::clipboard;
    QClipboard* clipboard = QGuiApplication::clipboard();
    const ClipboardSnapshot owners = ClipboardSnapshot::capture(clipboard->mimeData());

    const QString previous = QStringLiteral("clipboard test: previous text");
    const QString dictated = QStringLiteral("clipboard test: dictated text");
    clipboard->setText(previous);
    run_events(50);

    const SequenceSource sequence = system_clipboard_sequence();
    const Renderer renderer = system_renderer();
    check(static_cast<bool>(sequence), "the windows platform has a clipboard counter");
    check(static_cast<bool>(renderer.text) && static_cast<bool>(renderer.html),
          "the windows platform has the text and HTML renderers");
    TemporaryClipboard temporary(clipboard, sequence, renderer);

    const auto stage = temporary.stage_text(dictated.toStdString(), std::nullopt);
    check(stage.written && stage.lease.has_value(), "the stage kept the previous text");
    check(win32_text() == dictated.toStdWString(),
          "GetClipboardData(CF_UNICODETEXT) reads the staged text");
    if (stage.lease) {
        check(temporary.schedule_restore(*stage.lease, fv::focus::CLIPBOARD_RESTORE_DELAY),
              "the restore was scheduled");
        run_events(150);
        check(win32_text() == dictated.toStdWString(), "the staged text stays for 150 ms");
        QElapsedTimer waited;
        waited.start();
        while (win32_text() != previous.toStdWString() && waited.elapsed() < 2000) {
            run_events(10);
        }
        check(win32_text() == previous.toStdWString(), "the previous text came back");
        check(waited.elapsed() + 150 >= 250, "the restore waited 250 ms");
    }

    clipboard->setMimeData(owners.to_mime_data().release());
    run_events(50);
    std::fprintf(stderr, "%d check(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
