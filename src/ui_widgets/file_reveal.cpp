#include "ui_widgets/file_reveal.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include "base/log.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.utils.file_reveal"};

FileRevealHooks& hooks() {
    static FileRevealHooks current;
    return current;
}

bool openUrl(const QUrl& url) {
    const auto& custom = hooks().openUrl;
    return custom ? custom(url) : QDesktopServices::openUrl(url);
}

#ifdef Q_OS_WIN
bool startDetached(const QString& commandLine) {
    const auto& custom = hooks().startDetached;
    if (custom) {
        return custom(commandLine);
    }
    // The program ends at the first space: only Explorer is ever launched this way.
    const qsizetype space = commandLine.indexOf(QLatin1Char(' '));
    QProcess process;
    process.setProgram(commandLine.left(space));
    process.setNativeArguments(commandLine.mid(space + 1));
    return process.startDetached();
}

#endif

// os.path.abspath: absolute, with "." and ".." resolved and the platform's separators.
QString absolutePath(const QString& path) {
    return QDir::toNativeSeparators(QDir::cleanPath(QDir::current().absoluteFilePath(path)));
}

// The folder holding target: the fallback when selecting fails.
bool openContainingFolder(const QString& target) {
    const QFileInfo info(target);
    const QString folder = info.isDir() ? target : QFileInfo(target).absolutePath();
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
        kLog.warning("No folder to open for {}", target.toStdString());
        return false;
    }
    return openUrl(QUrl::fromLocalFile(folder));
}

} // namespace

FileRevealHooks setFileRevealHooks(FileRevealHooks replacement) {
    FileRevealHooks previous = std::move(hooks());
    hooks() = std::move(replacement);
    return previous;
}

bool revealInFileManager(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    const QString target = absolutePath(path);

#ifdef Q_OS_WIN
    if (QFileInfo::exists(target)) {
        if (startDetached(QStringLiteral("explorer /select,\"%1\"").arg(target))) {
            return true;
        }
        kLog.warning("Failed to reveal {}: the file manager did not start", target.toStdString());
    }
#endif
    return openContainingFolder(target);
}

bool openFolderInFileManager(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    const QString folder = absolutePath(path);
    if (!QFileInfo(folder).isDir()) {
        kLog.warning("No folder to open: {}", folder.toStdString());
        return false;
    }
    return openUrl(QUrl::fromLocalFile(folder));
}

} // namespace fv
