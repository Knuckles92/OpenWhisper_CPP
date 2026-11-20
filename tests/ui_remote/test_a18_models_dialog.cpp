// A18-15: "Manage host models" opens RemoteModelsDialog for the paired host without blocking
// Settings (settings_remote.py _manage_models: WA_DeleteOnClose, show()).

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QPointer>
#include <QWidget>

#include "base/testing/manual_executor.h"
#include "remote/testing/fake_remote_engine.h"
#include "ui_remote/remote_models_dialog.h"
#include "ui_remote_support.h"

TEST_CASE("Manage host models opens a non-modal dialog that deletes itself on close",
          "[widget][remote_models][a18_rows]") {
    QWidget window;
    window.show();
    auto remote = std::make_shared<fv::remote::testing::FakeRemoteEngine>();
    remote->pairing = fvtest::pairedWith();
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    fv::openRemoteModelsDialog(&window, QStringLiteral("devbox"), remote->pairing->fingerprint,
                               remote, io, ui, {});
    const auto dialogs = window.findChildren<QDialog*>(QStringLiteral("remoteModelsDialog"),
                                                       Qt::FindDirectChildrenOnly);
    REQUIRE(dialogs.size() == 1);
    QPointer<QDialog> dialog = dialogs.front();
    CHECK(dialog->isVisible());
    CHECK_FALSE(dialog->isModal());
    CHECK(dialog->windowModality() == Qt::NonModal);
    CHECK(dialog->testAttribute(Qt::WA_DeleteOnClose));
    CHECK(dialog->windowTitle() == QStringLiteral("Models on devbox"));
    // Settings behind it still takes input.
    CHECK(window.isEnabled());
    io.run_all();
    ui.run_all();
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(dialog.isNull());
}
