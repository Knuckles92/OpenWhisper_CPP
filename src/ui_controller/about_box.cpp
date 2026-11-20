#include "ui_controller/about_box.h"

#include <system_error>

#include <QCoreApplication>
#include <QMessageBox>

#include "appkit/qt_bridge.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "engine/version.h"

namespace fv {
namespace {

bool isWithin(const std::filesystem::path& path, const std::filesystem::path& folder) {
    const auto relative = path.lexically_normal().lexically_relative(folder.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}

} // namespace

QString installChannelLabel(const std::filesystem::path& applicationDir,
                            const std::filesystem::path& localAppData) {
    if (!localAppData.empty() &&
        isWithin(applicationDir, localAppData / "Programs" / std::string(identity().product))) {
        return QStringLiteral("Windows installer");
    }
    std::error_code error;
    for (std::filesystem::path folder = applicationDir; !folder.empty();
         folder = folder.parent_path()) {
        if (std::filesystem::exists(folder / ".git", error)) {
            return QStringLiteral("Source checkout (git)");
        }
        if (folder == folder.parent_path()) {
            break;
        }
    }
    return QStringLiteral("Source copy");
}

QString installChannel() {
    const std::filesystem::path appDir =
        base_utf8::path_from_utf8(qt::toUtf8(QCoreApplication::applicationDirPath()));
    const std::optional<std::string> localAppData = system_env().get("LOCALAPPDATA");
    return installChannelLabel(appDir, localAppData ? base_utf8::path_from_utf8(*localAppData)
                                                    : std::filesystem::path());
}

AboutText aboutText(std::string_view version, const QString& channel, const QString& updateStatus) {
    const QString product = qt::toQString(identity().product);
    const QString statusHtml = updateStatus.isEmpty()
                                   ? QString()
                                   : QStringLiteral("<p>Update status: %1</p>").arg(updateStatus);
    AboutText about;
    about.title = QStringLiteral("About %1").arg(product);
    about.html = QStringLiteral("<p><b>%1 - Speech-to-Text Application</b></p>"
                                "<p>Version %2<br>"
                                "Install: %3</p>"
                                "%4"
                                "<p>Record audio and turn it into text. Works offline with "
                                "Parakeet on this computer.</p>"
                                "<p>Features:<br>"
                                "&bull; Local transcription<br>"
                                "&bull; Global hotkeys (press * to record)<br>"
                                "&bull; Cool waveform visualizations<br>"
                                "&bull; Auto-pastes text for you<br>"
                                "&bull; Runs in the background</p>"
                                "<p>Open source and free to use.</p>")
                     .arg(product, qt::toQString(version), channel, statusHtml);
    return about;
}

void showAboutBox(QWidget* parent) {
    const AboutText about = aboutText(kVersion, installChannel());
    QMessageBox::about(parent, about.title, about.html);
}

} // namespace fv
