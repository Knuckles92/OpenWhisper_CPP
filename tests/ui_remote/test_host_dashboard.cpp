#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>

#include "base/testing/manual_executor.h"
#include "remote/testing/fake_client_history_source.h"
#include "remote/testing/fake_remote_engine.h"
#include "ui_remote/client_history_dialog.h"
#include "ui_remote/host_dashboard.h"
#include "ui_remote/pair_request_prompter.h"

TEST_CASE("Host Mode only reads history from the paired computer the user selects", "[widget]") {
    fv::ManualExecutor io;
    fv::ManualExecutor ui;
    fv::remote::testing::FakeClientHistorySource source;
    source.set_clients(
        {{"offline", "Unavailable peer", "unavailable"}, {"paired", "Synthetic peer", "online"}});
    source.answer("paired", "transcriptions",
                  {{"items", fv::Json::array({{{"id", "entry"}, {"title", "Synthetic sample"}}})}});
    source.answer("paired", "transcription", {{"text", "Synthetic transcript"}});
    QWidget parent;
    fv::openClientHistory(parent, source, io, ui);
    io.run_all();
    ui.run_all();

    auto* dialog = parent.findChild<QDialog*>(QStringLiteral("pairedHistoryDialog"));
    REQUIRE(dialog);
    auto* devices = dialog->findChild<QComboBox*>(QStringLiteral("pairedHistoryComputer"));
    auto* load = dialog->findChild<QPushButton*>(QStringLiteral("pairedHistoryList"));
    auto* results = dialog->findChild<QListWidget*>(QStringLiteral("pairedHistoryRows"));
    auto* open = dialog->findChild<QPushButton*>(QStringLiteral("pairedHistoryOpen"));
    auto* detail = dialog->findChild<QPlainTextEdit*>(QStringLiteral("pairedHistoryDetail"));
    REQUIRE(devices);
    REQUIRE(load);
    REQUIRE(results);
    REQUIRE(open);
    REQUIRE(detail);
    CHECK_FALSE(load->isEnabled());
    devices->setCurrentIndex(1);
    CHECK(load->isEnabled());
    CHECK(source.queries().empty());

    load->click();
    io.run_all();
    ui.run_all();
    REQUIRE(results->count() == 1);
    CHECK(results->item(0)->text() == QStringLiteral("Synthetic sample"));
    results->setCurrentRow(0);
    open->click();
    io.run_all();
    ui.run_all();
    CHECK(detail->toPlainText() == QStringLiteral("Synthetic transcript"));
    const auto requests = source.queries();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].device_id == "paired");
    CHECK(requests[1].params.at("record_id") == "entry");
}
