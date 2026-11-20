// Settings → Remote engine: where this computer's records are kept, and the records a host keeps
// for its paired computers (tests/test_remote_records_ui.py's Settings tests).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QPushButton>

#include "ui_remote/record_recovery_dialog.h"
#include "ui_remote_support.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"

using fv::Json;
using fvtest::PageHarness;
using fvtest::pumpFor;
using fvtest::pumpUntil;

namespace {

// Paired with devbox, as FakeRecords starts.
PageHarness::Options paired(std::function<void(fv::remote::SyncStatus&)> status = {}) {
    PageHarness::Options options;
    options.script = [status = std::move(status)](auto& engine, auto& records) {
        engine.pairing = fvtest::pairedWith();
        records.status_value.location = "local";
        records.status_value.paired = true;
        records.status_value.host_name = "devbox";
        if (status) {
            status(records.status_value);
        }
    };
    return options;
}

} // namespace

// from tests/test_remote_records_ui.py::test_the_storage_choice_shows_only_while_paired
TEST_CASE("the storage choice shows only while paired", "[test_remote_records_ui]") {
    PageHarness::Options options;
    options.script = [](auto&, auto& records) {
        records.status_value.paired = false;
        records.status_value.host_name = "devbox";
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    CHECK_FALSE(harness.page->storageTile()->isVisibleTo(harness.page.get()));
    harness.records.status_value.paired = true;
    harness.page->refresh();
    REQUIRE(harness.settle());
    CHECK(harness.page->storageTile()->isVisibleTo(harness.page.get()));
    CHECK(harness.page->locationButton("local")->isChecked());
    CHECK(harness.page->locationButton("host")->text() == QStringLiteral("devbox"));
}

// from tests/test_remote_records_ui.py::test_choosing_a_location_saves_it_and_says_what_happens
TEST_CASE("choosing a location saves it and says what happens", "[test_remote_records_ui]") {
    PageHarness harness(paired());
    REQUIRE(harness.show());
    harness.page->locationButton("both")->click();
    REQUIRE(pumpUntil([&] {
        return harness.page->storageStatus()->text().contains(QStringLiteral("copied to devbox"));
    }));
    CHECK(harness.recordsCalled("set_location(both)"));
    CHECK(harness.page->locationButton("both")->isChecked());
    CHECK(harness.page->sendExistingButton()->text() == QStringLiteral("Copy existing records"));
    CHECK(harness.page->storageStatus()->text().contains(
        QStringLiteral("New records will also be copied to devbox.")));
}

// from tests/test_remote_records_ui.py::test_a_host_that_refuses_says_how_to_turn_it_on
TEST_CASE("a host that refuses says how to turn it on", "[test_remote_records_ui]") {
    PageHarness harness(paired([](fv::remote::SyncStatus& status) {
        status.location = "host";
        status.host_keeps = false;
        status.host_supports = true;
        status.pending = 2;
    }));
    REQUIRE(harness.show());
    const QString text = harness.page->storageStatus()->text();
    CHECK(text.contains(QStringLiteral("Keep records for paired computers")));
    CHECK(text.contains(QStringLiteral("2 waiting to be sent")));
    CHECK_FALSE(harness.page->sendExistingButton()->isEnabled());
}

// from tests/test_remote_records_ui.py::test_what_the_host_keeps_is_counted_and_can_be_brought_back
TEST_CASE("what the host keeps is counted and can be brought back", "[test_remote_records_ui]") {
    PageHarness harness(paired([](fv::remote::SyncStatus& status) {
        status.stored = {{"dictation", {3, 2048}}, {"meeting", {1, 0}}};
    }));
    REQUIRE(harness.show());
    CHECK(harness.page->storageStatus()->text().contains(
        QStringLiteral("On devbox: 3 dictations and 1 meeting (2.0 KB).")));
    CHECK(harness.page->bringBackButton()->isVisibleTo(harness.page.get()));
}

// from tests/test_remote_records_ui.py::test_the_host_toggle_saves_the_opt_in
TEST_CASE("the host toggle saves the opt in", "[test_remote_records_ui]") {
    PageHarness harness(paired());
    REQUIRE(harness.show());
    CHECK_FALSE(harness.page->keepRecordsTile()->checkbox()->isChecked());
    harness.page->keepRecordsTile()->checkbox()->setChecked(true);
    CHECK(pumpUntil([&] { return harness.called("set_keep_records(true)"); }));
    CHECK(pumpUntil([&] { return harness.engine.host_state().keep_records; }));
}

// from tests/test_remote_records_ui.py::test_host_record_recovery_requires_explicit_assignment
TEST_CASE("host record recovery requires explicit assignment", "[test_remote_records_ui]") {
    for (const bool accepted : {false, true}) {
        CAPTURE(accepted);
        std::vector<std::pair<std::string, std::string>> choices;
        PageHarness::Options options = paired();
        const auto script = options.script;
        options.script = [script](auto& engine, auto& records) {
            script(engine, records);
            fv::remote::PairedDevice device;
            device.id = "new-laptop";
            device.name = "Laptop";
            engine.state.devices = {device};
            engine.owners = {{"old-laptop", "Laptop", {{"dictation", 2}, {"meeting", 1}}}};
        };
        options.customize = [&choices, accepted](fv::RemoteEnginePageServices& services) {
            services.prompts.recoverRecords =
                [&choices, accepted](QWidget* parent,
                                     const std::vector<fv::remote::RecordOwnerGroup>& owners,
                                     const std::vector<fv::remote::PairedDevice>& devices)
                -> std::optional<std::pair<std::string, std::string>> {
                fv::RecordRecoveryDialog modal(owners, devices, parent);
                CHECK(modal.objectName() == QStringLiteral("remoteRecordRecoveryDialog"));
                CHECK(modal.ownerCombo()->objectName() ==
                      QStringLiteral("remoteRecoveryOwnerCombo"));
                CHECK(modal.ownerCombo()->itemText(0) ==
                      QStringLiteral("Laptop · old-lapt · 2 dictations and 1 meeting"));
                CHECK(modal.deviceCombo()->itemText(0) == QStringLiteral("Laptop · new-lapt"));
                CHECK_FALSE(modal.recoverButton()->isDefault());
                choices.push_back(modal.choice());
                if (!accepted) {
                    return std::nullopt;
                }
                return modal.choice();
            };
        };
        PageHarness harness(options);
        REQUIRE(harness.show());
        harness.page->resize(520, 740);
        CHECK(harness.page->recoverRecordsButton()->isEnabled());
        harness.page->recoverRecordsButton()->click();
        REQUIRE(pumpUntil([&] { return !choices.empty(); }));
        CHECK(choices ==
              std::vector<std::pair<std::string, std::string>>{{"old-laptop", "new-laptop"}});
        REQUIRE(pumpUntil([&] { return !harness.page->recoveryBusy() && harness.settle(); }));
        CHECK(harness.called("recover_device_records(old-laptop,new-laptop)") == accepted);
        if (accepted) {
            CHECK(harness.page->recoveryMessage()->text().contains(QStringLiteral("available")));
        }
    }
}

// from
// tests/test_remote_records_ui.py::test_record_storage_controls_do_not_grant_mcp_history_sharing
TEST_CASE("record storage controls do not grant mcp history sharing", "[test_remote_records_ui]") {
    PageHarness harness(paired());
    REQUIRE(harness.show());
    harness.page->keepRecordsTile()->checkbox()->setChecked(true);
    harness.page->locationButton("both")->click();
    REQUIRE(pumpUntil([&] {
        return harness.called("set_keep_records(true)") &&
               harness.recordsCalled("set_location(both)");
    }));
    CHECK(harness.records.location() == "both");
    pumpFor(std::chrono::milliseconds(100));
    for (const std::string& call : harness.engine.calls()) {
        CHECK_FALSE(call.starts_with("set_share_history("));
    }
}

// ---- beyond the ported tests ----

TEST_CASE("bringing records back asks first and keeps new ones here", "[remote_page_records]") {
    PageHarness harness(paired([](fv::remote::SyncStatus& status) {
        status.location = "host";
        status.stored = {{"dictation", {3, 2048}}};
    }));
    harness.records.bring_back_result = 3;
    harness.records.progress_lines = {"Bringing back 2…"};
    REQUIRE(harness.show());
    harness.page->bringBackButton()->click();
    REQUIRE(harness.questions.size() == 1);
    CHECK(harness.questions[0] ==
          QStringLiteral("Bring records back: Bring every record on devbox back to this computer, "
                         "and keep new ones here from now on?"));
    REQUIRE(pumpUntil([&] {
        return !harness.page->recordsBusy() &&
               harness.page->storageStatus()->text().contains(
                   QStringLiteral("Brought 3 records back from devbox."));
    }));
    const auto calls = harness.records.calls();
    const auto at = [&](const std::string& call) {
        return std::find(calls.begin(), calls.end(), call) - calls.begin();
    };
    CHECK(at("set_location(local)") < at("bring_back()"));
    CHECK(at("bring_back()") < static_cast<long long>(calls.size()));
}

TEST_CASE("a no to bringing records back changes nothing", "[remote_page_records]") {
    PageHarness harness(paired([](fv::remote::SyncStatus& status) {
        status.location = "both";
        status.stored = {{"meeting", {1, 0}}};
    }));
    harness.questionAnswer = false;
    REQUIRE(harness.show());
    harness.page->bringBackButton()->click();
    pumpFor(std::chrono::milliseconds(100));
    CHECK_FALSE(harness.recordsCalled("bring_back()"));
    CHECK_FALSE(harness.recordsCalled("set_location(local)"));
}

TEST_CASE("sending existing records says how many were queued", "[remote_page_records]") {
    PageHarness harness(paired([](fv::remote::SyncStatus& status) { status.location = "host"; }));
    harness.records.send_existing_result = 1;
    REQUIRE(harness.show());
    CHECK(harness.page->sendExistingButton()->text() == QStringLiteral("Move existing records"));
    harness.page->sendExistingButton()->click();
    CHECK(pumpUntil([&] {
        return harness.page->storageStatus()->text().contains(
            QStringLiteral("Queued 1 record to send."));
    }));
}

TEST_CASE("forgetting a host that keeps records asks first", "[remote_page_records]") {
    PageHarness harness(
        paired([](fv::remote::SyncStatus& status) { status.stored = {{"dictation", {2, 0}}}; }));
    harness.questionAnswer = false;
    REQUIRE(harness.show());
    harness.page->forgetButton()->click();
    REQUIRE(harness.questions.size() == 1);
    CHECK(harness.questions[0] ==
          QStringLiteral("Forget host: devbox keeps 2 dictations of this computer's. Once you "
                         "forget it, this computer can't list or bring them back; they stay on "
                         "devbox. Forget it anyway?"));
    pumpFor(std::chrono::milliseconds(100));
    CHECK_FALSE(harness.called("forget_host()"));
}

TEST_CASE("without a record sync the storage tile stays hidden", "[remote_page_records]") {
    PageHarness::Options options = paired();
    options.records = false;
    PageHarness harness(options);
    REQUIRE(harness.show());
    CHECK(harness.page->storageTile()->isHidden());
}

namespace {

// A record sync whose status can't be read (_refresh_records' except branch).
class FailingRecords : public fv::remote::testing::FakeRecordSync {
public:
    fv::remote::SyncStatus status() override { throw std::runtime_error("database is locked"); }
};

} // namespace

TEST_CASE("a record sync whose status fails hides the storage tile", "[remote_page_records]") {
    fv::remote::testing::FakeRemoteEngine engine;
    engine.pairing = fvtest::pairedWith();
    FailingRecords records;
    fv::FakeClock clock(fvtest::referenceTime(), "America/Los_Angeles");
    fv::ThreadPool pool("remote-page-test-io", 2);
    fv::SettingsPage container;
    fv::RemoteEnginePageServices services;
    services.service = &engine;
    services.records = &records;
    services.io = &pool;
    services.clock = &clock;
    services.openUrl = [](const QUrl&) {
    };
    auto page = std::make_unique<fv::RemoteEnginePage>(container, std::move(services));
    page->resize(900, 1200);
    page->show();
    page->onShown();
    REQUIRE(pumpUntil([&] { return !page->refreshing(); }));
    // The rest of the page still draws.
    CHECK_FALSE(page->clientTile()->isHidden());
    CHECK(page->storageTile()->isHidden());
    // A record-sync event reads the status again, and it still fails.
    records.changed.emit("records");
    pumpFor(std::chrono::milliseconds(50));
    CHECK(page->storageTile()->isHidden());
    page.reset();
}

TEST_CASE("nothing to recover says so", "[remote_page_records]") {
    PageHarness harness(paired());
    REQUIRE(harness.show());
    harness.page->recoverRecordsButton()->click();
    REQUIRE(pumpUntil([&] { return !harness.informations.empty(); }));
    CHECK(harness.informations[0] ==
          QStringLiteral("Recover stored records: Pair the original computer again, then assign "
                         "its old records here."));
}
