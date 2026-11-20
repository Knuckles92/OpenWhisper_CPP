#include "ui_shell/qt_log.h"

#include <atomic>
#include <format>
#include <string>

#include <QtLogging>

#include "appkit/qt_bridge.h"
#include "base/log.h"

namespace fv {
namespace {

constexpr log::Logger kQt{"qt"};
constexpr log::Logger kRoot{"root"};

std::atomic_bool installed{false};

void handleQtMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    // A Qt message handler must not throw back into Qt, whatever a sink does.
    try {
        const std::string text =
            qt::toUtf8(message) + qtContextSuffix(context.file, context.line, context.function);
        switch (type) {
        case QtDebugMsg:
            kQt.debug("{}", text);
            break;
        case QtWarningMsg:
            kQt.warning("{}", text);
            break;
        case QtCriticalMsg:
            kQt.error("{}", text);
            break;
        case QtFatalMsg:
            kQt.critical("{}", text);
            break;
        case QtInfoMsg:
        default:
            kQt.info("{}", text);
            break;
        }
    } catch (...) {
        // Nothing can report a failure of the reporting itself.
    }
}

} // namespace

std::string qtContextSuffix(const char* file, int line, const char* function) {
    const bool hasFile = file != nullptr && *file != '\0';
    const bool hasFunction = function != nullptr && *function != '\0';
    if (!hasFile && !hasFunction && line == 0) {
        return {};
    }
    return std::format(" ({}:{} {})", file != nullptr ? file : "None", line,
                       function != nullptr ? function : "None");
}

void installQtMessageHandler() {
    if (installed.exchange(true)) {
        return;
    }
    qInstallMessageHandler(&handleQtMessage);
    kRoot.info("Qt message handler installed");
}

} // namespace fv
