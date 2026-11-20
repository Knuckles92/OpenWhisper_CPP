#pragma once

#include <filesystem>
#include <string_view>

#include <QString>

class QWidget;

namespace fv {

// app_update.channel_label: "Windows installer", "Source checkout (git)" or "Source copy".
// OpenWhisper_CPP's installer puts the app in %LOCALAPPDATA%\Programs\<product> (M3); a build found
// inside a git checkout is a source checkout.
QString installChannelLabel(const std::filesystem::path& applicationDir,
                            const std::filesystem::path& localAppData);
// The label for this executable.
QString installChannel();

// The About box (ui_controller.py:2839-2879) in OpenWhisper_CPP's words: the title, the product
// line, the version and install channel, what the app does and its features, and "Update status:
// ..." once a check has a result (UpdatePresenter::aboutStatusLine). OpenWhisper's website line
// names OpenWhisper's site; OpenWhisper_CPP has none yet, so the closing line keeps only "Open
// source and free to use."
struct AboutText {
    QString title; // "About OpenWhisper_CPP"
    QString html;
};
AboutText aboutText(std::string_view version, const QString& channel,
                    const QString& updateStatus = {});

// QMessageBox::about over parent.
void showAboutBox(QWidget* parent);

} // namespace fv
