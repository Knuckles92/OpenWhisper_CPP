#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <QMetaObject>
#include <QString>
#include <QTimer>

#include "app_theme.h"
#include "base/testing/manual_executor.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_record/scratchpad.h"
#include "ui_record/scratchpad_saver.h"

namespace fvtest {

// What the Scratchpad tests stand on: the application styled as OpenWhisper styles it, a data
// folder of its own holding the notes file, in-memory settings, a manual persistence strand
// (the saver writes when the test runs it) and a clipboard copy that records what it is given.
// Nothing touches the real clipboard, settings or data folder.
class PadRig {
    // First member (destroyed last): the stylesheet outlives every widget.
    AppTheme theme_;

public:
    explicit PadRig(const fv::Json& saved = fv::Json::object(),
                    const QString& theme = QStringLiteral("dark"), int scalePercent = 100)
        : theme_(theme, scalePercent), settings(saved) {
        service = std::make_unique<fv::ScratchpadService>(environment(), copier());
    }
    ~PadRig() {
        // Writes what is pending while the folder still exists.
        service.reset();
    }
    PadRig(const PadRig&) = delete;
    PadRig& operator=(const PadRig&) = delete;

    fv::ScratchpadEnvironment environment() {
        fv::ScratchpadEnvironment made;
        made.settings = &settings;
        made.persistence = &persistence;
        made.file = [path = file()] {
            return path;
        };
        return made;
    }

    // Copy all's clipboard: records each text and answers copyWorks.
    fv::ScratchpadWindow::CopyText copier() {
        return [this](const QString& text) {
            copied.push_back(text);
            return copyWorks;
        };
    }

    std::filesystem::path file() const { return root.path() / "scratchpad.txt"; }

    // The first toggle: the window exists, is shown and focused (as far as `active` says).
    fv::ScratchpadWindow& open() {
        fv::toggleScratchpad(*service);
        fv::ScratchpadWindow& pad = *service->window();
        pad.setActivityProbe([this] { return active; });
        return pad;
    }

    // What the notes file holds, byte for byte.
    std::string saved() const {
        std::ifstream stream(file(), std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
    void write(const std::string& bytes) const {
        std::ofstream stream(file(), std::ios::binary);
        stream << bytes;
    }
    // Lets the strand write what was submitted.
    void settle() { persistence.run_all(); }

    fv::testkit::TempDataRoot root;
    fv::MemorySettingsStore settings;
    fv::ManualExecutor persistence;
    std::vector<QString> copied;
    bool copyWorks = true;
    // Whether the window counts as the desktop's active window.
    bool active = true;
    std::unique_ptr<fv::ScratchpadService> service;
};

// Python's `timer.timeout.emit()`: the timer's slot runs now. The timer keeps its own state.
inline void fire(QTimer* timer) {
    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
}

} // namespace fvtest
