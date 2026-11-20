// The About box (ui_controller.py:2839-2879) in OpenWhisper_CPP's words: a tree test of its title,
// product line, version and install channel, and the install channel's label.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>

#include "controller_support.h"
#include "engine/version.h"
#include "ui_controller/about_box.h"

using fvtest::ControllerRig;

TEST_CASE("the About box names the product, version and install", "[widget]") {
    const fv::AboutText about = fv::aboutText("0.1.0", QStringLiteral("Windows installer"));

    CHECK(about.title == QStringLiteral("About OpenWhisper_CPP"));
    CHECK(about.html.startsWith(
        QStringLiteral("<p><b>OpenWhisper_CPP - Speech-to-Text Application</b></p>")));
    CHECK(
        about.html.contains(QStringLiteral("<p>Version 0.1.0<br>Install: Windows installer</p>")));
    CHECK_FALSE(about.html.contains(QStringLiteral("Update status")));
    CHECK(about.html.contains(QStringLiteral("Open source and free to use.")));
    CHECK_FALSE(about.html.contains(QStringLiteral("Whisper")));

    const fv::AboutText withUpdate =
        fv::aboutText("0.1.0", QStringLiteral("Source copy"), QStringLiteral("Up to date"));
    CHECK(withUpdate.html.contains(QStringLiteral("<p>Update status: Up to date</p>")));
}

TEST_CASE("the install channel follows where the app runs from", "[unit]") {
    fv::testkit::TempDataRoot root;
    const auto local = root.path() / "Local";
    CHECK(fv::installChannelLabel(local / "Programs" / "OpenWhisper_CPP", local) ==
          QStringLiteral("Windows installer"));
    const auto checkout = root.path() / "checkout";
    std::filesystem::create_directories(checkout / ".git");
    std::filesystem::create_directories(checkout / "out" / "bin");
    CHECK(fv::installChannelLabel(checkout / "out" / "bin", local) ==
          QStringLiteral("Source checkout (git)"));
    std::filesystem::create_directories(root.path() / "copy");
    CHECK(fv::installChannelLabel(root.path() / "copy", local) == QStringLiteral("Source copy"));
}

TEST_CASE("Help > About shows the About box over the window", "[widget]") {
    ControllerRig rig;
    QString title;
    QString text;
    // The box is modal: read it once it is up, then close it.
    QTimer::singleShot(0, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<QMessageBox*>(widget);
                box != nullptr && box->isVisible()) {
                title = box->windowTitle();
                text = box->text();
                box->accept();
            }
        }
    });

    Q_EMIT rig.window().aboutRequested();

    CHECK(title == QStringLiteral("About OpenWhisper_CPP"));
    CHECK(text.contains(QStringLiteral("Version %1").arg(QString::fromUtf8(fv::kVersion))));
}
