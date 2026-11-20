// The a18 audit's Settings → Remote engine rows whose behaviour matched but whose tests pinned
// only part (docs/port/audit/a18-remote-engine-ui.json, A18-01 to A18-53): the assertions each
// row's note listed as missing, over PageHarness and the remote fakes. The engines and record
// syncs below hold a call open (a Gate) or throw where the page's busy and failure paths need it.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QTimer>

#include "base/identity.h"
#include "credentials/store.h"
#include "pyport/errors.h"
#include "remote/errors.h"
#include "remote/protocol.h"
#include "ui_remote/record_recovery_dialog.h"
#include "ui_remote/remote_name_dialog.h"
#include "ui_remote_support.h"
#include "ui_theme/palette.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::remote::ClientPairing;
using fv::remote::NearbyHost;
using fvtest::pumpFor;
using fvtest::pumpUntil;

namespace {

constexpr auto kTags = "[remote_page][a18_rows]";

const std::string kFingerprint = std::string(32, 'A') + std::string(32, 'B');

QString product() {
    return QString::fromStdString(std::string(fv::identity().product));
}

QString shortFingerprint(const std::string& fingerprint) {
    return QString::fromStdString(fv::remote::short_fingerprint(fingerprint));
}

// Holds the calls that pass it until the test opens it (open at first; at most 5 s closed).
class Gate {
public:
    void close() {
        std::lock_guard lock(mutex_);
        open_ = false;
        entered_ = 0;
    }
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        opened_.notify_all();
    }
    void pass() {
        std::unique_lock lock(mutex_);
        ++entered_;
        opened_.wait_for(lock, std::chrono::seconds(5), [this] { return open_; });
    }
    bool isOpen() const {
        std::lock_guard lock(mutex_);
        return open_;
    }
    // How many calls reached the gate since it was closed.
    int entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable opened_;
    bool open_ = true;
    int entered_ = 0;
};

// FakeRemoteEngine whose searches, pairings and requests wait on gates, and whose renames,
// removals, sharing switch and record recovery throw when told to.
class HeldEngine : public fv::remote::testing::FakeRemoteEngine {
public:
    Gate scanGate;
    Gate pairGate;
    // A pairing request waits here before the host shows its number, then until allowed.
    Gate codeGate;
    Gate allowGate;
    Gate recoverGate;
    std::atomic<bool> failEnable{false};
    std::atomic<bool> failRename{false};
    std::atomic<bool> failRemove{false};
    std::atomic<bool> failOwners{false};
    std::atomic<bool> failRecover{false};
    std::atomic<std::thread::id> enableThread{};

    // What host_state reports as the pairing code from now on (the countdown reads it on io).
    void setPairing(std::optional<std::pair<std::string, double>> code) {
        std::lock_guard lock(own_);
        pairingOverride_ = std::move(code);
    }
    fv::remote::HostState host_state() override {
        fv::remote::HostState read = FakeRemoteEngine::host_state();
        std::lock_guard lock(own_);
        if (pairingOverride_) {
            read.pairing = *pairingOverride_;
        }
        return read;
    }

    fv::remote::NearbyScan scan_nearby(bool sweep) override {
        scanGate.pass();
        return FakeRemoteEngine::scan_nearby(sweep);
    }
    ClientPairing pair(std::string_view address, std::string_view code, bool tailscale) override {
        pairGate.pass();
        return FakeRemoteEngine::pair(address, code, tailscale);
    }
    ClientPairing request_pairing(std::string_view address,
                                  std::function<void(std::string sas)> on_code,
                                  fv::CancelToken cancel) override {
        codeGate.pass();
        if (on_code) {
            on_code("123456");
        }
        while (!allowGate.isOpen()) {
            if (cancel.wait_for(std::chrono::milliseconds(20))) {
                throw fv::remote::PairingCanceled("Pairing canceled.");
            }
        }
        return FakeRemoteEngine::request_pairing(address, {}, cancel);
    }
    void set_host_enabled(bool enabled) override {
        enableThread = std::this_thread::get_id();
        if (failEnable) {
            throw std::runtime_error("The port is taken.");
        }
        FakeRemoteEngine::set_host_enabled(enabled);
    }
    std::optional<ClientPairing> rename_host(std::string_view name) override {
        if (failRename) {
            throw std::runtime_error("The settings file is read-only.");
        }
        return FakeRemoteEngine::rename_host(name);
    }
    std::optional<fv::remote::PairedDevice> rename_device(std::string_view id,
                                                          std::string_view name) override {
        if (failRename) {
            throw std::runtime_error("The device list is read-only.");
        }
        return FakeRemoteEngine::rename_device(id, name);
    }
    bool remove_device(std::string_view id, bool delete_records) override {
        if (failRemove) {
            throw std::runtime_error("The device list is read-only.");
        }
        return FakeRemoteEngine::remove_device(id, delete_records);
    }
    std::vector<fv::remote::RecordOwnerGroup> recoverable_record_owners() override {
        if (failOwners) {
            throw std::runtime_error("The record store is locked.");
        }
        return FakeRemoteEngine::recoverable_record_owners();
    }
    void recover_device_records(std::string_view owner, std::string_view device) override {
        recoverGate.pass();
        if (failRecover) {
            throw std::runtime_error("The record store is locked.");
        }
        FakeRemoteEngine::recover_device_records(owner, device);
    }

private:
    std::mutex own_;
    std::optional<std::optional<std::pair<std::string, double>>> pairingOverride_;
};

// FakeRecordSync whose jobs wait on a gate (bring_back after its progress lines), and which can
// ask before sending existing records as the app's manual transfers do.
class HeldRecords : public fv::remote::testing::FakeRecordSync {
public:
    Gate jobGate;
    std::atomic<bool> manual{false};

    int send_existing() override {
        const int queued = FakeRecordSync::send_existing();
        jobGate.pass();
        return queued;
    }
    int bring_back(std::function<void(std::string)> progress) override {
        const int moved = FakeRecordSync::bring_back(std::move(progress));
        jobGate.pass();
        return moved;
    }
    bool manual_transfers() const override { return manual; }
};

// The page over HeldEngine and HeldRecords.
struct Harness : fvtest::BasicPageHarness<HeldEngine> {
    using Base = fvtest::BasicPageHarness<HeldEngine>;
    using Base::Base;
};

// The page over HeldEngine with HeldRecords in place of the plain fake record sync.
struct RecordsHarness {
    explicit RecordsHarness(std::function<void(HeldEngine&, HeldRecords&)> script = {},
                            std::function<void(fv::RemoteEnginePageServices&)> customize = {}) {
        records.status_value.location = "local";
        if (script) {
            script(base.engine, records);
        }
        base.page.reset();
        fv::RemoteEnginePageServices services;
        services.service = &base.engine;
        services.io = &base.pool;
        services.records = &records;
        services.clock = &base.clock;
        services.openUrl = [this](const QUrl& url) {
            base.opened.push_back(url.toString());
        };
        services.prompts.question = [this](QWidget*, const QString& title, const QString& text) {
            base.questions.push_back(title + QStringLiteral(": ") + text);
            return base.questionAnswer;
        };
        services.prompts.warning = [this](QWidget*, const QString& title, const QString& text) {
            base.warnings.push_back(title + QStringLiteral(": ") + text);
        };
        if (customize) {
            customize(services);
        }
        base.page = std::make_unique<fv::RemoteEnginePage>(base.container, std::move(services));
        base.page->resize(900, 1200);
    }
    ~RecordsHarness() {
        base.engine.scanGate.open();
        records.jobGate.open();
        base.page.reset();
    }
    bool recordsCalled(const std::string& call) const {
        const auto calls = records.calls();
        return std::find(calls.begin(), calls.end(), call) != calls.end();
    }
    fv::RemoteEnginePage& page() { return *base.page; }

    // Declared first: destroyed after the base harness, whose pool waits for the jobs using it.
    HeldRecords records;
    // Built unbound, so that nothing reads the engine while the script fills it.
    Harness base{unbound()};

private:
    static Harness::Options unbound() {
        Harness::Options options;
        options.bound = false;
        options.records = false;
        return options;
    }
};

// Paired with devbox; the record sync knows it.
void pairedRecords(HeldEngine& engine, fv::remote::testing::FakeRecordSync& records) {
    engine.pairing = fvtest::pairedWith();
    records.status_value.paired = true;
    records.status_value.host_name = "devbox";
}

// A computer that answered the search on this network.
NearbyHost lanHost(std::string name = "devbox", std::string address = "192.168.1.20",
                   bool compatible = true, bool approval = true) {
    NearbyHost host;
    host.host_name = name;
    host.fingerprint = kFingerprint;
    host.engine = fvtest::fakeEngine();
    host.compatible = compatible;
    host.approval = approval;
    fv::remote::LanHost lan;
    lan.address = std::move(address);
    lan.port = 47821;
    lan.host_name = name;
    lan.fingerprint = kFingerprint;
    lan.engine = host.engine;
    lan.protocol = 1;
    lan.approval = approval;
    host.lan = lan;
    return host;
}

// A computer of the same Tailscale account, which pairs without a code.
NearbyHost tailnetHost() {
    NearbyHost devbox;
    devbox.host_name = "devbox";
    devbox.fingerprint = kFingerprint;
    devbox.engine = fvtest::fakeEngine();
    devbox.compatible = true;
    devbox.approval = true;
    fv::remote::TailnetHost peer;
    peer.peer.name = "devbox";
    peer.peer.address = "100.64.0.2";
    peer.peer.owner = "owner@example.com";
    peer.peer.mine = true;
    peer.host_name = "devbox";
    peer.engine = devbox.engine;
    peer.tailscale_pairing = true;
    peer.compatible = true;
    peer.fingerprint = kFingerprint;
    peer.approval = true;
    devbox.tailnet = peer;
    return devbox;
}

fv::net::TailscaleStatus tailscaleState(std::string state, std::string owner = {}) {
    fv::net::TailscaleStatus status;
    status.state = std::move(state);
    if (status.state == "running") {
        status.name = "laptop";
        status.address = "100.64.0.1";
    }
    status.owner = std::move(owner);
    return status;
}

int countCalls(const std::vector<std::string>& calls, const std::string& prefix) {
    int count = 0;
    for (const std::string& call : calls) {
        count += call.starts_with(prefix) ? 1 : 0;
    }
    return count;
}

// The visible find-list notes (not the found computers' rows).
QList<fv::WrappedLabel*> findNotes(fv::RemoteEnginePage& page) {
    QList<fv::WrappedLabel*> notes;
    for (auto* label : page.findList()->findChildren<fv::WrappedLabel*>()) {
        if (label->objectName() == QStringLiteral("remoteFindNote") && !label->isHidden()) {
            notes.append(label);
        }
    }
    return notes;
}

QStringList noteTexts(fv::RemoteEnginePage& page) {
    QStringList texts;
    for (auto* label : findNotes(page)) {
        texts.append(label->text());
    }
    return texts;
}

QString tailscaleLink(const QString& text) {
    return QStringLiteral("<a href=\"https://tailscale.com/download\" style=\"color: %1; "
                          "text-decoration: underline;\">%2</a>")
        .arg(QString::fromStdString(fv::ui::currentPalette().css("accent")), text);
}

// The label of each visible paired-computer row.
QStringList deviceRows(fv::RemoteEnginePage& page) {
    QStringList texts;
    for (auto* label : page.devicesList()->findChildren<fv::WrappedLabel*>()) {
        if (!label->isHidden()) {
            texts.append(label->text());
        }
    }
    return texts;
}

// The visible buttons with this text in the find list.
QList<QPushButton*> findButtons(fv::RemoteEnginePage& page, const QString& text) {
    QList<QPushButton*> found;
    for (auto* button : page.findList()->findChildren<QPushButton*>()) {
        if (button->text() == text && !button->isHidden()) {
            found.append(button);
        }
    }
    return found;
}

// A font as the page sets the pairing numbers: from the label's own, 1.8x, bold, spaced 3 px.
void checkCodeFont(const QLabel* label) {
    const QFont font = label->font();
    CHECK(font.bold());
    CHECK(font.letterSpacingType() == QFont::AbsoluteSpacing);
    CHECK(font.letterSpacing() == Catch::Approx(3.0));
    CHECK(font.pointSizeF() == Catch::Approx(QLabel().font().pointSizeF() * 1.8));
}

} // namespace

// ---- A18-01: the two tabs ----

TEST_CASE("a18 rows: the role tabs are named, say what is paired, and open on the side in use",
          kTags) {
    SECTION("paired: the client tab says with what, and stays open while sharing too") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            engine.pairing = fvtest::pairedWith();
            engine.state.enabled = true;
            engine.state.running = true;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        auto* tabs = harness.page->tabs();
        CHECK(tabs->accessibleName() == QStringLiteral("Remote engine role"));
        REQUIRE(pumpUntil([&] {
            return tabs->buttons()[0]->detailLabel()->text() == QStringLiteral("Paired · devbox");
        }));
        CHECK(tabs->buttons()[1]->detailLabel()->text() == QStringLiteral("Sharing · 0 paired"));
        pumpFor(std::chrono::milliseconds(100));
        CHECK(tabs->currentIndex() == 0);
        // Looking at the client tab again searches for nothing while paired.
        harness.clock.advance(std::chrono::seconds(21));
        tabs->buttons()[1]->click();
        tabs->buttons()[0]->click();
        REQUIRE(harness.settle());
        pumpFor(std::chrono::milliseconds(100));
        CHECK(countCalls(harness.engine.calls(), "scan_nearby(") == 0);
    }
    SECTION("sharing switched on but not running yet says Starting… and opens that tab") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            engine.state.enabled = true;
            engine.state.running = false;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        auto* tabs = harness.page->tabs();
        REQUIRE(pumpUntil([&] {
            return tabs->buttons()[1]->detailLabel()->text() == QStringLiteral("Starting…");
        }));
        CHECK(tabs->currentIndex() == 1);
    }
    SECTION("a page that opened on sharing searches when the client tab is first looked at") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            engine.state.enabled = true;
            engine.state.running = true;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        auto* tabs = harness.page->tabs();
        REQUIRE(pumpUntil([&] { return tabs->currentIndex() == 1; }));
        REQUIRE(harness.settle());
        const int before = countCalls(harness.engine.calls(), "scan_nearby(false)");
        harness.clock.advance(std::chrono::seconds(21));
        tabs->buttons()[0]->click();
        CHECK(pumpUntil([&] {
            return countCalls(harness.engine.calls(), "scan_nearby(false)") == before + 1;
        }));
        CHECK(tabs->currentIndex() == 0);
    }
}

// ---- A18-03, A18-49: looking for computers ----

TEST_CASE("a18 rows: a running search says where it looks and holds the find buttons", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        // Known before any search ends: the description already counts the tailnet.
        engine.tailscale_value = tailscaleState("running", "owner@example.com");
        engine.scan.status = tailscaleState("running", "owner@example.com");
        engine.scanGate.close();
    };
    Harness harness(options);
    auto& page = *harness.page;
    page.show();
    page.onShown();
    REQUIRE(pumpUntil([&] { return harness.engine.scanGate.entered() == 1; }));
    auto* description = page.findTile()->descriptionLabel();
    CHECK(pumpUntil([&] {
        return description->text() ==
               QStringLiteral("Looking for computers on this network and your tailnet...");
    }));
    CHECK(page.scanBusy());
    CHECK_FALSE(page.searchButton()->isEnabled());
    CHECK_FALSE(page.sweepButton()->isEnabled());
    harness.engine.scanGate.open();
    REQUIRE(harness.settle());
    CHECK(description->text() ==
          QStringLiteral("Computers on this network and your tailnet that share their engine. "
                         "Signed in to Tailscale as owner@example.com."));
    CHECK(page.searchButton()->isEnabled());
    CHECK(page.sweepButton()->isEnabled());

    // A search after one that found the tailnet words it from that search.
    harness.engine.scanGate.close();
    page.searchButton()->click();
    REQUIRE(pumpUntil([&] { return harness.engine.scanGate.entered() == 1; }));
    CHECK(description->text() ==
          QStringLiteral("Looking for computers on this network and your tailnet..."));
    harness.engine.scanGate.open();
    REQUIRE(harness.settle());
}

TEST_CASE("a18 rows: without Tailscale a running search looks on this network only", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.scanGate.close();
    };
    Harness harness(options);
    auto& page = *harness.page;
    page.show();
    page.onShown();
    REQUIRE(pumpUntil([&] { return harness.engine.scanGate.entered() == 1; }));
    CHECK(pumpUntil([&] {
        return page.findTile()->descriptionLabel()->text() ==
               QStringLiteral("Looking for computers on this network...");
    }));
    harness.engine.scanGate.open();
    REQUIRE(harness.settle());
    CHECK(page.findTile()->descriptionLabel()->text() ==
          QStringLiteral("Computers on this network that share their engine."));
}

TEST_CASE("a18 rows: the find tile's buttons say what they do", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.scan.hosts = {lanHost()};
    };
    Harness harness(options);
    REQUIRE(harness.show());
    auto& page = *harness.page;
    CHECK(page.findTile()->titleLabel()->text() == QStringLiteral("Find a computer"));
    CHECK(page.searchButton()->text() == QStringLiteral("Search again"));
    CHECK(page.sweepButton()->text() == QStringLiteral("Check every address"));
    CHECK(page.sweepButton()->toolTip() ==
          QStringLiteral(
              "Try each address on this network, for routers that hide computers from a search"));
    CHECK(page.manualButton()->text() == QStringLiteral("Enter an address"));
    // A search that found someone needs no sweep.
    CHECK(harness.rows().size() == 1);
    CHECK(page.sweepButton()->isHidden());
}

// ---- A18-04: a found computer's row ----

TEST_CASE("a18 rows: a found computer can't be picked while incompatible or while pairing", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.scan.hosts = {lanHost("devbox", "192.168.1.20"),
                             lanHost("oldbox", "192.168.1.21", false, false),
                             lanHost("studio", "192.168.1.22")};
        engine.allowGate.close();
    };
    Harness harness(options);
    REQUIRE(harness.show());
    auto& page = *harness.page;
    const auto codes = findButtons(page, QStringLiteral("Use a code"));
    REQUIRE(codes.size() == 1);
    CHECK_FALSE(codes[0]->isEnabled());
    CHECK(codes[0]->parentWidget()->findChild<fv::WrappedLabel*>()->toolTip() ==
          QStringLiteral("192.168.1.21:47821"));
    auto connects = findButtons(page, QStringLiteral("Connect"));
    REQUIRE(connects.size() == 2);
    CHECK(connects[0]->isEnabled());
    CHECK(connects[1]->isEnabled());

    connects[0]->click();
    REQUIRE(pumpUntil([&] { return !page.waitBox()->isHidden(); }));
    connects = findButtons(page, QStringLiteral("Connect"));
    REQUIRE(connects.size() == 2);
    CHECK_FALSE(connects[1]->isEnabled());
    page.waitCancelButton()->click();
    REQUIRE(pumpUntil([&] { return !page.pairingBusy(); }));
    harness.engine.allowGate.open();
    REQUIRE(harness.settle());
    connects = findButtons(page, QStringLiteral("Connect"));
    REQUIRE(connects.size() == 2);
    CHECK(connects[1]->isEnabled());
}

// ---- A18-05: nothing found ----

TEST_CASE("a18 rows: nothing found says what to do, then why Tailscale didn't help", kTags) {
    const QString none = QStringLiteral(
        "None found. On the computer you want to use, open Settings → Remote engine → Share this "
        "computer and turn on sharing.");
    const QString unswept = QStringLiteral(" If it's on, your router may be hiding it from the "
                                           "search: try Check every address, or enter its "
                                           "address.");
    struct Case {
        std::string state;
        bool swept;
        QStringList notes;
    };
    const std::vector<Case> cases = {
        {"stopped",
         false,
         {none + unswept, QStringLiteral("Tailscale is installed but turned off; turn it on to "
                                         "find your computers on other networks too.")}},
        {"needs_login",
         true,
         {none + QStringLiteral(" Checked every address on this network as well."),
          QStringLiteral("Tailscale is installed but not signed in; sign in to find your "
                         "computers on other networks too.")}},
        {"starting", false, {none + unswept, QStringLiteral("Tailscale is still starting.")}},
        {"running", false, {none + unswept}},
        {"not_installed",
         false,
         {none + unswept, QStringLiteral("On a different network? %1 on both computers and sign "
                                         "in with the same account.")
                              .arg(tailscaleLink(QStringLiteral("Get Tailscale")))}},
    };
    for (const Case& item : cases) {
        CAPTURE(item.state, item.swept);
        Harness::Options options;
        options.script = [&item](auto& engine, auto&) {
            engine.scan.status = tailscaleState(item.state);
            engine.scan.swept = item.swept;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        CHECK(noteTexts(*harness.page) == item.notes);
        CHECK(harness.page->sweepButton()->isHidden() == item.swept);
        if (item.state == "not_installed") {
            auto* link = findNotes(*harness.page).back();
            CHECK(link->textFormat() == Qt::RichText);
            CHECK_FALSE(link->openExternalLinks());
            Q_EMIT link->linkActivated(QStringLiteral("https://tailscale.com/download"));
            CHECK(harness.opened ==
                  std::vector<QString>{QStringLiteral("https://tailscale.com/download")});
        }
    }
}

// ---- A18-06: the address row ----

TEST_CASE("a18 rows: the address row's fields, its errors and Enter to pair", kTags) {
    Harness harness;
    REQUIRE(harness.show());
    auto& page = *harness.page;
    CHECK(page.addressEdit()->placeholderText() ==
          QStringLiteral("Host address, e.g. 192.168.1.20 or devbox.local"));
    CHECK(page.addressEdit()->minimumHeight() == 36);
    CHECK(page.codeEdit()->placeholderText() == QStringLiteral("Pairing code"));
    CHECK(page.codeEdit()->maxLength() == 12);
    CHECK(page.codeEdit()->maximumWidth() == 150);
    CHECK(page.codeEdit()->minimumHeight() == 36);
    CHECK(page.pairButton()->text() == QStringLiteral("Pair"));
    CHECK(qobject_cast<fv::PrimaryButton*>(page.pairButton()) != nullptr);

    page.manualButton()->click();
    // parse_address's own sentence, as the page shows it.
    for (const std::string bad : {"", "[fe80::1", "[fe80::1]47821", "dev box"}) {
        CAPTURE(bad);
        QString expected;
        try {
            fv::remote::parse_address(bad);
        } catch (const fv::py::ValueError& error) {
            expected = QString::fromUtf8(error.what());
        }
        REQUIRE_FALSE(expected.isEmpty());
        page.addressEdit()->setText(QString::fromStdString(bad));
        page.codeEdit()->setText(QStringLiteral("123456"));
        page.pairButton()->click();
        CHECK(page.findMessage()->text() == expected);
    }
    CHECK(countCalls(harness.engine.calls(), "pair(") == 0);

    page.addressEdit()->setText(QStringLiteral("192.168.1.20"));
    page.codeEdit()->setText(QStringLiteral("123456"));
    QTest::keyClick(page.codeEdit(), Qt::Key_Return);
    CHECK(pumpUntil([&] { return harness.called("pair(192.168.1.20,123456,false)"); }));
    REQUIRE(pumpUntil([&] { return !page.pairingBusy(); }));
}

TEST_CASE("a18 rows: the address row and Enter an address hide while a request waits", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.scan.hosts = {lanHost()};
        engine.codeGate.close();
        engine.allowGate.close();
    };
    Harness harness(options);
    REQUIRE(harness.show());
    auto& page = *harness.page;
    page.manualButton()->click();
    CHECK_FALSE(page.pairRow()->isHidden());
    findButtons(page, QStringLiteral("Connect")).front()->click();
    CHECK(page.pairRow()->isHidden());
    CHECK(page.manualButton()->isHidden());
    // A18-08, A18-49: before the host shows its number there is no number to compare.
    CHECK(page.findMessage()->text() == QStringLiteral("Asking devbox..."));
    REQUIRE(pumpUntil([&] { return harness.engine.codeGate.entered() == 1; }));
    pumpFor(std::chrono::milliseconds(50));
    CHECK(page.waitBox()->isHidden());
    CHECK(page.findMessage()->text() == QStringLiteral("Asking devbox..."));

    harness.engine.codeGate.open();
    REQUIRE(pumpUntil([&] { return !page.waitBox()->isHidden(); }));
    CHECK(page.findMessage()->text() ==
          QStringLiteral("Waiting for devbox to allow this computer..."));
    CHECK(page.waitLabel()->text() ==
          QStringLiteral("On devbox, click Allow if it shows this number. If it shows a "
                         "different one, click Deny there: something else is answering for it."));
    CHECK(page.waitCodeLabel()->text() == QStringLiteral("123 456"));
    checkCodeFont(page.waitCodeLabel());
    CHECK(page.pairRow()->isHidden());
    CHECK(page.manualButton()->isHidden());

    page.waitCancelButton()->click();
    REQUIRE(pumpUntil([&] { return !page.pairingBusy(); }));
    harness.engine.allowGate.open();
    REQUIRE(harness.settle());
    // The person had opened the row; it comes back once nothing waits.
    CHECK_FALSE(page.pairRow()->isHidden());
}

// ---- A18-07: pairing ----

TEST_CASE("a18 rows: a code pairing says Pairing..., then which identity to compare", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        ClientPairing made = fvtest::pairedWith();
        made.fingerprint = kFingerprint;
        engine.pair_result = made;
    };
    Harness harness(options);
    REQUIRE(harness.show());
    auto& page = *harness.page;
    page.manualButton()->click();
    page.addressEdit()->setText(QStringLiteral("192.168.1.20"));
    page.codeEdit()->setText(QStringLiteral("123456"));
    harness.engine.pairGate.close();
    page.pairButton()->click();
    CHECK(page.findMessage()->text() == QStringLiteral("Pairing..."));
    CHECK_FALSE(page.pairButton()->isEnabled());
    REQUIRE(pumpUntil([&] { return harness.engine.pairGate.entered() == 1; }));
    CHECK(page.findMessage()->text() == QStringLiteral("Pairing..."));
    harness.engine.pairGate.open();
    REQUIRE(pumpUntil([&] { return !page.pairingBusy(); }));
    CHECK(page.clientMessage()->text() ==
          QStringLiteral("Paired with devbox. Check that its identity, %1, matches the one shown "
                         "there.")
              .arg(shortFingerprint(kFingerprint)));
    CHECK(page.codeEdit()->text().isEmpty());
}

TEST_CASE("a18 rows: a Tailscale pairing says so in full", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.scan.status = tailscaleState("running", "owner@example.com");
        engine.scan.hosts = {tailnetHost()};
        ClientPairing made = fvtest::pairedWith("100.64.0.2", "devbox");
        made.via = "tailscale";
        engine.pair_result = made;
    };
    Harness harness(options);
    REQUIRE(harness.show());
    findButtons(*harness.page, QStringLiteral("Connect")).front()->click();
    REQUIRE(pumpUntil([&] { return !harness.page->pairingBusy() && harness.settle(); }));
    CHECK(harness.page->clientMessage()->text() ==
          QStringLiteral("Paired with devbox through your Tailscale account. Click \"Use for "
                         "dictation\" to start using it."));
}

// ---- A18-11: saving the connection key ----

TEST_CASE("a18 rows: retrying the key says it asks for access", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.pending_name = "Test host";
    };
    Harness harness(options);
    REQUIRE(harness.show());
    REQUIRE(harness.settle());
    // After the search, which would take the failure otherwise.
    harness.engine.fail_next(std::make_exception_ptr(fv::CredentialStoreError("access denied")));
    auto& page = *harness.page;
    REQUIRE_FALSE(page.credentialRecovery()->isHidden());
    page.retryKeyButton()->click();
    CHECK(page.findMessage()->text() ==
          QStringLiteral("Requesting access to save the connection key..."));
    REQUIRE(pumpUntil([&] { return !page.pairingBusy() && harness.settle(); }));
    CHECK(page.findMessage()->text() ==
          QStringLiteral("Couldn't save the connection key. Retry access below to finish "
                         "pairing."));
}

// ---- A18-12, A18-13: the paired host ----

TEST_CASE("a18 rows: the paired host tile says where and who, with every action", kTags) {
    SECTION("on this network: the identity, the actions and how to go further") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            ClientPairing pairing = fvtest::pairedWith();
            pairing.fingerprint = kFingerprint;
            engine.pairing = pairing;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        auto& page = *harness.page;
        CHECK(page.clientTile()->titleLabel()->text() == QStringLiteral("Paired host"));
        CHECK(page.clientTile()->descriptionLabel()->text() ==
              QStringLiteral("Paired with devbox at 192.168.1.20:47821. Its identity is %1.")
                  .arg(shortFingerprint(kFingerprint)));
        CHECK(page.useButton()->text() == QStringLiteral("Use for dictation"));
        CHECK(qobject_cast<fv::PrimaryButton*>(page.useButton()) != nullptr);
        CHECK(page.meetingUseButton()->text() == QStringLiteral("Use for meetings"));
        CHECK(page.manageButton()->text() == QStringLiteral("Manage host models"));
        CHECK(page.renameHostButton()->text() == QStringLiteral("Rename host"));
        CHECK(page.renameHostButton()->toolTip() ==
              QStringLiteral("Change the name this computer shows for the host"));
        CHECK(page.forgetButton()->text() == QStringLiteral("Forget host"));

        // A18-13: the whole hint, opened through the page's opener.
        auto* hint = page.tailscaleHint();
        CHECK_FALSE(hint->isHidden());
        CHECK(hint->text() == QStringLiteral("Works while both computers are on the same network. "
                                             "To use devbox away from home too, %1 on both "
                                             "computers and sign in with the same account.")
                                  .arg(tailscaleLink(QStringLiteral("get Tailscale"))));
        CHECK_FALSE(hint->openExternalLinks());
        Q_EMIT hint->linkActivated(QStringLiteral("https://tailscale.com/download"));
        CHECK(harness.opened ==
              std::vector<QString>{QStringLiteral("https://tailscale.com/download")});
    }
    SECTION("over Tailscale") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            ClientPairing pairing = fvtest::pairedWith("100.64.0.2");
            pairing.fingerprint = kFingerprint;
            engine.pairing = pairing;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        CHECK(harness.page->clientTile()->descriptionLabel()->text() ==
              QStringLiteral("Paired with devbox over Tailscale at 100.64.0.2:47821. Its identity "
                             "is %1.")
                  .arg(shortFingerprint(kFingerprint)));
        CHECK(harness.page->tailscaleHint()->isHidden());
    }
    SECTION("on this network with a Tailscale address for away") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            ClientPairing pairing = fvtest::pairedWith();
            pairing.fingerprint = kFingerprint;
            pairing.alternates = {"192.168.1.21", "100.64.0.2"};
            engine.pairing = pairing;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        CHECK(harness.page->clientTile()->descriptionLabel()->text() ==
              QStringLiteral("Paired with devbox at 192.168.1.20:47821, or over Tailscale at "
                             "100.64.0.2 when away. Its identity is %1.")
                  .arg(shortFingerprint(kFingerprint)));
        // A13: already reachable away from home.
        CHECK(harness.page->tailscaleHint()->isHidden());
    }
}

// ---- A18-16: renaming ----

TEST_CASE("a18 rows: renaming says what the name is for, and says when it fails", kTags) {
    struct Asked {
        QString title;
        QString text;
    };
    std::vector<Asked> asked;
    std::optional<QString> answer = QStringLiteral("Studio PC");
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        ClientPairing pairing = fvtest::pairedWith("192.168.1.20", "Studio PC");
        pairing.paired_name = "DESKTOP-4K2J9";
        engine.pairing = pairing;
        fv::remote::PairedDevice laptop;
        laptop.id = "laptop-id";
        laptop.name = "Kitchen laptop";
        laptop.paired_name = "DESKTOP-LAPTOP";
        engine.state.devices = {laptop};
        engine.failRename = true;
    };
    options.customize = [&](fv::RemoteEnginePageServices& services) {
        services.prompts.askName = [&](QWidget*, const QString& title, const QString& text,
                                       const QString&, const QString&) {
            asked.push_back({title, text});
            return answer;
        };
    };
    Harness harness(options);
    REQUIRE(harness.show());
    REQUIRE(harness.settle());
    auto& page = *harness.page;
    page.renameHostButton()->click();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].title == QStringLiteral("Rename host"));
    CHECK(asked[0].text ==
          QStringLiteral("The name this computer shows for its host. Leave it empty to go back to "
                         "DESKTOP-4K2J9, the name it gave when pairing."));
    REQUIRE(pumpUntil([&] { return !harness.warnings.empty(); }));
    CHECK(harness.warnings ==
          std::vector<QString>{QStringLiteral("Could not rename host: The settings file is "
                                              "read-only.")});

    page.tabs()->buttons()[1]->click();
    QPushButton* rename = Harness::button(page.devicesList(), QStringLiteral("Rename"));
    REQUIRE(rename != nullptr);
    rename->click();
    REQUIRE(asked.size() == 2);
    CHECK(asked[1].title == QStringLiteral("Rename paired computer"));
    CHECK(asked[1].text ==
          QStringLiteral("The name this computer shows for it here, on the host dashboard and in "
                         "History. Leave it empty to go back to DESKTOP-LAPTOP, the name it "
                         "paired with."));
    REQUIRE(pumpUntil([&] { return harness.warnings.size() == 2; }));
    CHECK(harness.warnings[1] ==
          QStringLiteral("Could not rename computer: The device list is read-only."));
}

TEST_CASE("a18 rows: the name dialog is laid out as OpenWhisper's", kTags) {
    fv::RemoteNameDialog dialog(QStringLiteral("Rename host"), QStringLiteral("Why."),
                                QStringLiteral("Studio PC"), QStringLiteral("DESKTOP-4K2J9"));
    CHECK(dialog.minimumWidth() == 360);
    REQUIRE(dialog.layout() != nullptr);
    CHECK(dialog.layout()->contentsMargins() == QMargins(18, 18, 18, 18));
    CHECK(dialog.layout()->spacing() == 12);
    CHECK(dialog.edit()->minimumHeight() == 36);
    CHECK(dialog.windowTitle() == QStringLiteral("Rename host"));
    bool said = false;
    for (auto* label : dialog.findChildren<fv::WrappedLabel*>()) {
        said = said || label->text() == QStringLiteral("Why.");
    }
    CHECK(said);
}

// ---- A18-17: forgetting the host ----

TEST_CASE("a18 rows: forgetting a host that keeps records, once allowed, forgets and searches",
          kTags) {
    Harness::Options options;
    options.script = [](auto&, auto& records) {
        records.status_value.paired = true;
        records.status_value.host_name = "devbox";
        records.status_value.stored = {{"dictation", {3, 0}}};
    };
    Harness harness(options);
    REQUIRE(harness.show());
    REQUIRE(countCalls(harness.engine.calls(), "scan_nearby(false)") == 1);
    // Paired within the search cache's 20 s.
    harness.engine.pair("192.168.1.20", "123456", false);
    REQUIRE(pumpUntil([&] { return !harness.page->clientTile()->isHidden(); }));
    REQUIRE(harness.settle());
    harness.questionAnswer = true;
    harness.page->forgetButton()->click();
    CHECK(harness.questions.size() == 1);
    CHECK(pumpUntil([&] { return harness.called("forget_host()"); }));
    CHECK(pumpUntil([&] {
        return countCalls(harness.engine.calls(), "scan_nearby(false)") == 2 && harness.settle();
    }));
    CHECK(harness.page->findMessage()->text() ==
          QStringLiteral("Forgot the host. Pair again to use it."));
}

// ---- A18-19: what the storage line says ----

TEST_CASE("a18 rows: the storage line puts the sync's state in OpenWhisper's order", kTags) {
    struct Case {
        const char* name;
        std::function<void(fv::remote::SyncStatus&)> status;
        QString text;
    };
    const std::vector<Case> cases = {
        {"active replaces waiting, pending and its error",
         [](auto& status) {
             status.active = "Sending 2 of 5…";
             status.waiting = "Waiting for devbox to answer.";
             status.pending = 3;
             status.error = "Timed out.";
         },
         QStringLiteral("Sending 2 of 5…")},
        {"waiting shows with nothing active",
         [](auto& status) { status.waiting = "Waiting for devbox to answer."; },
         QStringLiteral("Waiting for devbox to answer.")},
        {"a host problem replaces waiting",
         [](auto& status) {
             status.location = "host";
             status.host_keeps = false;
             status.waiting = "Waiting for devbox to answer.";
         },
         QStringLiteral("devbox isn't keeping records for other computers yet. On devbox, turn on "
                        "\"Keep records for paired computers\" under Settings → Remote engine.")},
        {"an error shows with what is pending",
         [](auto& status) {
             status.pending = 2;
             status.error = "Timed out.";
             status.stored = {{"dictation", {3, 0}}};
         },
         QStringLiteral("2 waiting to be sent. Timed out. On devbox: 3 dictations.")},
        {"an error with nothing pending is not shown",
         [](auto& status) { status.error = "Timed out."; }, QString()},
    };
    for (const Case& item : cases) {
        CAPTURE(item.name);
        Harness::Options options;
        options.script = [&item](auto& engine, auto& records) {
            pairedRecords(engine, records);
            item.status(records.status_value);
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        auto* line = harness.page->storageStatus();
        CHECK(line->text() == item.text);
        CHECK(line->isHidden() == item.text.isEmpty());
    }
}

TEST_CASE("a18 rows: a records job's line replaces the active one, and its result comes last",
          kTags) {
    RecordsHarness harness([](HeldEngine& engine, HeldRecords& records) {
        pairedRecords(engine, records);
        records.status_value.location = "both";
        records.status_value.active = "Sending 1 of 2…";
        records.status_value.pending = 2;
        records.status_value.stored = {{"dictation", {3, 0}}};
        records.send_existing_result = 1;
        records.jobGate.close();
    });
    REQUIRE(harness.base.show());
    REQUIRE(harness.base.settle());
    auto& page = harness.page();
    CHECK(page.storageStatus()->text() ==
          QStringLiteral("Sending 1 of 2… On devbox: 3 dictations."));
    page.sendExistingButton()->click();
    CHECK(page.storageStatus()->text() ==
          QStringLiteral("Queuing records… On devbox: 3 dictations."));
    REQUIRE(pumpUntil([&] { return harness.records.jobGate.entered() == 1; }));
    harness.records.jobGate.open();
    REQUIRE(pumpUntil([&] { return !page.recordsBusy(); }));
    CHECK(pumpUntil([&] {
        return page.storageStatus()->text() ==
               QStringLiteral("Sending 1 of 2… On devbox: 3 dictations. Queued 1 record to send.");
    }));
}

// ---- A18-20: the record actions ----

TEST_CASE("a18 rows: the record actions show where they apply and wait for each other", kTags) {
    SECTION("kept here: nothing to send, nothing to bring back") {
        Harness::Options options;
        options.script = [](auto& engine, auto& records) {
            pairedRecords(engine, records);
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        CHECK(harness.page->sendExistingButton()->isHidden());
        CHECK(harness.page->bringBackButton()->isHidden());
        CHECK(harness.page->retryRecordsButton()->isHidden());
    }
    SECTION("sending: the tooltip, the busy line and everything held meanwhile") {
        RecordsHarness harness([](HeldEngine& engine, HeldRecords& records) {
            pairedRecords(engine, records);
            records.status_value.location = "host";
            records.status_value.stored = {{"dictation", {3, 0}}};
            records.send_existing_result = 0;
            records.jobGate.close();
        });
        REQUIRE(harness.base.show());
        REQUIRE(harness.base.settle());
        auto& page = harness.page();
        CHECK_FALSE(page.sendExistingButton()->isHidden());
        CHECK(page.sendExistingButton()->toolTip() ==
              QStringLiteral("Send the records already on this computer to devbox too"));
        CHECK_FALSE(page.bringBackButton()->isHidden());
        CHECK(page.bringBackButton()->toolTip() ==
              QStringLiteral("Move everything devbox keeps for this computer back here"));
        page.sendExistingButton()->click();
        CHECK(page.storageStatus()->text().startsWith(QStringLiteral("Queuing records…")));
        for (const char* location : {"local", "host", "both"}) {
            CHECK_FALSE(page.locationButton(location)->isEnabled());
        }
        CHECK_FALSE(page.sendExistingButton()->isEnabled());
        CHECK_FALSE(page.bringBackButton()->isEnabled());
        harness.records.jobGate.open();
        REQUIRE(pumpUntil([&] { return !page.recordsBusy(); }));
        CHECK(pumpUntil([&] {
            return page.storageStatus()->text().endsWith(
                QStringLiteral("Everything here is already on the host."));
        }));
        for (const char* location : {"local", "host", "both"}) {
            CHECK(page.locationButton(location)->isEnabled());
        }
        CHECK(page.sendExistingButton()->isEnabled());
        CHECK(page.bringBackButton()->isEnabled());
    }
    SECTION("bringing back: the busy line names the host") {
        RecordsHarness harness([](HeldEngine& engine, HeldRecords& records) {
            pairedRecords(engine, records);
            records.status_value.location = "local";
            records.status_value.stored = {{"dictation", {3, 0}}};
            records.bring_back_result = 3;
            records.jobGate.close();
        });
        REQUIRE(harness.base.show());
        REQUIRE(harness.base.settle());
        auto& page = harness.page();
        page.bringBackButton()->click();
        CHECK(page.storageStatus()->text().startsWith(
            QStringLiteral("Bringing records back from devbox…")));
        CHECK_FALSE(page.bringBackButton()->isEnabled());
        harness.records.jobGate.open();
        REQUIRE(pumpUntil([&] { return !page.recordsBusy(); }));
    }
    SECTION("Try again shows for a failed send and wakes the sync at once") {
        struct Case {
            int pending;
            std::string error;
            std::string active;
            bool shown;
        };
        for (const Case& item : std::vector<Case>{{2, "Timed out.", "", true},
                                                  {0, "Timed out.", "", false},
                                                  {2, "", "", false},
                                                  {2, "Timed out.", "Sending 1 of 2…", false}}) {
            CAPTURE(item.pending, item.error, item.active);
            Harness::Options options;
            options.script = [&item](auto& engine, auto& records) {
                pairedRecords(engine, records);
                records.status_value.pending = item.pending;
                records.status_value.error = item.error;
                records.status_value.active = item.active;
            };
            Harness harness(options);
            REQUIRE(harness.show());
            REQUIRE(harness.settle());
            auto* retry = harness.page->retryRecordsButton();
            CHECK(retry->isHidden() == !item.shown);
            if (item.shown) {
                CHECK(retry->text() == QStringLiteral("Try again"));
                retry->click();
                CHECK(pumpUntil([&] { return harness.recordsCalled("wake(true)"); }));
            }
        }
    }
    SECTION("with manual transfers, sending existing dictations asks first") {
        for (const bool yes : {false, true}) {
            CAPTURE(yes);
            RecordsHarness harness([](HeldEngine& engine, HeldRecords& records) {
                pairedRecords(engine, records);
                records.status_value.location = "host";
                records.manual = true;
                records.send_existing_result = 2;
            });
            harness.base.questionAnswer = yes;
            REQUIRE(harness.base.show());
            REQUIRE(harness.base.settle());
            auto& page = harness.page();
            CHECK(page.storageTile()->descriptionLabel()->text() ==
                  QStringLiteral("Copy or move existing dictations to the paired host when you "
                                 "choose. New dictations stay here until you send them."));
            page.sendExistingButton()->click();
            REQUIRE(harness.base.questions.size() == 1);
            CHECK(harness.base.questions[0] ==
                  QStringLiteral("Move existing records: Move the existing dictations and their "
                                 "recordings to devbox? Local copies are removed only after the "
                                 "host confirms storage."));
            if (yes) {
                CHECK(pumpUntil([&] { return harness.recordsCalled("send_existing()"); }));
                REQUIRE(pumpUntil([&] { return !page.recordsBusy(); }));
            } else {
                pumpFor(std::chrono::milliseconds(100));
                CHECK_FALSE(harness.recordsCalled("send_existing()"));
            }
        }
    }
}

// ---- A18-21: sharing ----

TEST_CASE("a18 rows: sharing switches off the GUI thread, and failing puts the switch back",
          kTags) {
    SECTION("with a computer paired already, no code is opened") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            fv::remote::PairedDevice laptop;
            laptop.id = "laptop-id";
            laptop.name = "Laptop";
            engine.state.devices = {laptop};
        };
        Harness harness(options);
        REQUIRE(harness.show());
        harness.page->shareTile()->checkbox()->setChecked(true);
        REQUIRE(pumpUntil([&] { return harness.called("set_host_enabled(true)"); }));
        REQUIRE(harness.settle());
        pumpFor(std::chrono::milliseconds(100));
        CHECK_FALSE(harness.called("open_pairing()"));
        CHECK(harness.engine.enableThread.load() != std::thread::id{});
        CHECK(harness.engine.enableThread.load() != std::this_thread::get_id());
    }
    SECTION("a change that throws shows the saved state again") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            engine.failEnable = true;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        auto* box = harness.page->shareTile()->checkbox();
        harness.engine.clear_calls();
        box->setChecked(true);
        CHECK(box->isChecked());
        // The refresh after the failure reads the state again and draws it.
        CHECK(pumpUntil([&] { return harness.called("host_state()") && !box->isChecked(); }));
        CHECK_FALSE(harness.engine.host_state().enabled);
    }
}

// ---- A18-23: the reachability notes ----

TEST_CASE("a18 rows: a note without a fix has no button, and an unchanged note is kept", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = true;
        engine.state.running = true;
        engine.state.notes = {{"vpn", "A VPN may hide this computer.", "", ""},
                              {"firewall", "The firewall may block it.", "Open settings", ""}};
    };
    Harness harness(options);
    REQUIRE(harness.show());
    auto* notes = harness.page->hostNotes();
    REQUIRE(pumpUntil([&] { return !notes->isHidden(); }));
    QList<QWidget*> rows;
    for (auto* row : notes->findChildren<QWidget*>(QStringLiteral("remoteHostNoteRow"))) {
        rows.append(row);
    }
    CHECK(rows.size() == 2);
    CHECK(notes->findChildren<QPushButton*>().isEmpty());
    QPointer<QWidget> first = rows.front();
    harness.page->refresh();
    REQUIRE(harness.settle());
    harness.engine.emit_changed("state");
    pumpFor(std::chrono::milliseconds(100));
    REQUIRE(harness.settle());
    CHECK_FALSE(first.isNull());
    CHECK(notes->findChildren<QWidget*>(QStringLiteral("remoteHostNoteRow")).size() == 2);
}

// ---- A18-26: the port ----

TEST_CASE("a18 rows: a refresh leaves a port being typed alone", kTags) {
    Harness harness;
    REQUIRE(harness.show());
    auto* spin = harness.page->portSpin();
    spin->setValue(50000);
    // Within the settle time, a state read is drawn.
    harness.page->refresh();
    REQUIRE(harness.settle());
    CHECK(spin->value() == 50000);
    CHECK(pumpUntil([&] { return harness.called("set_host_port(50000)"); }));
    REQUIRE(harness.settle());
    CHECK(spin->value() == 50000);
}

// ---- A18-27: the pairing code ----

TEST_CASE("a18 rows: the pairing code is drawn large and counts down from the host", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = true;
        engine.state.running = true;
        engine.state.pairing = std::make_pair(std::string("054321"), 54.9);
    };
    Harness harness(options);
    REQUIRE(harness.show());
    auto& page = *harness.page;
    CHECK(page.pairDeviceButton()->toolTip() ==
          QStringLiteral("For a computer that enters this one's address, or runs an older %1")
              .arg(product()));
    REQUIRE(pumpUntil([&] { return !page.pairingBox()->isHidden(); }));
    checkCodeFont(page.pairingCodeLabel());
    CHECK(page.pairingExpiryLabel()->text() ==
          QStringLiteral("Enter this code on the other computer. Expires in 0:54."));
    harness.engine.setPairing(std::make_pair(std::string("054321"), 30.2));
    CHECK(pumpUntil(
        [&] {
            return page.pairingExpiryLabel()->text() ==
                   QStringLiteral("Enter this code on the other computer. Expires in 0:30.");
        },
        std::chrono::milliseconds(3000)));
}

// ---- A18-28, A18-52: the paired computers ----

TEST_CASE("a18 rows: each paired computer's row says who, when, how and what it keeps", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto&) {
        fv::remote::PairedDevice laptop;
        laptop.id = "laptop-id";
        laptop.name = "Laptop";
        laptop.paired_at = "2026-11-14T09:00:00+00:00";
        laptop.via = "code";
        fv::remote::PairedDevice unnamed;
        unnamed.id = "unnamed-id";
        unnamed.paired_at = "2026-11-14T09:00:00+00:00";
        unnamed.via = "tailscale";
        engine.state.devices = {laptop, unnamed};
        fv::remote::ConnectedClient connected;
        connected.device_id = "unnamed-id";
        engine.state.clients = {connected};
        // Whatever the host's own switch says: what is kept is counted.
        engine.state.keep_records = false;
        engine.summary = Json{{"dictation", {{"count", 2}, {"bytes", 2048}}}};
    };
    Harness harness(options);
    REQUIRE(harness.show());
    REQUIRE(harness.settle());
    CHECK(deviceRows(*harness.page) ==
          QStringList{
              QStringLiteral("Laptop · paired Nov 14, 2026 · keeps 2 dictations (2.0 KB) here"),
              QStringLiteral("Unnamed computer · connected now · paired Nov 14, 2026 over "
                             "Tailscale · keeps 2 dictations (2.0 KB) here")});
    QPushButton* rename = Harness::button(harness.page->devicesList(), QStringLiteral("Rename"));
    REQUIRE(rename != nullptr);
    CHECK(rename->toolTip() == QStringLiteral("Change the name this computer shows for it"));

    harness.engine.state.devices.clear();
    harness.engine.emit_changed("devices");
    CHECK(pumpUntil([&] {
        return deviceRows(*harness.page) ==
               QStringList{QStringLiteral("No computers are paired yet.")};
    }));
}

// ---- A18-29, A18-50: removing a paired computer ----

TEST_CASE("a18 rows: removing a computer keeps, cancels or asks nothing as its records say",
          kTags) {
    const auto withLaptop = [](Json summary, std::string name = "Laptop") {
        Harness::Options options;
        options.script = [summary, name](auto& engine, auto&) {
            fv::remote::PairedDevice laptop;
            laptop.id = "laptop-id";
            laptop.name = name;
            engine.state.devices = {laptop};
            engine.summary = summary;
        };
        return options;
    };
    const Json stored = Json{{"dictation", {{"count", 2}, {"bytes", 2048}}}};
    const auto remove = [](Harness& harness) {
        harness.page->tabs()->buttons()[1]->click();
        QPushButton* button = Harness::button(harness.page->devicesList(),
                                              QStringLiteral("remoteRemoveDeviceButton"));
        REQUIRE(button != nullptr);
        button->click();
    };
    SECTION("Keep its records") {
        Harness harness(withLaptop(stored));
        harness.removeAnswer = fv::RemoteEnginePrompts::RemoveChoice::KeepRecords;
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        remove(harness);
        CHECK(harness.questions.size() == 1);
        CHECK(pumpUntil([&] { return harness.called("remove_device(laptop-id,false)"); }));
    }
    SECTION("Cancel") {
        Harness harness(withLaptop(stored));
        harness.removeAnswer = fv::RemoteEnginePrompts::RemoveChoice::Cancel;
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        remove(harness);
        CHECK(harness.questions.size() == 1);
        pumpFor(std::chrono::milliseconds(100));
        CHECK(countCalls(harness.engine.calls(), "remove_device(") == 0);
    }
    SECTION("nothing kept: no question") {
        Harness harness(withLaptop(Json::object()));
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        remove(harness);
        CHECK(harness.questions.empty());
        CHECK(pumpUntil([&] { return harness.called("remove_device(laptop-id,false)"); }));
    }
    SECTION("a removal that fails is said") {
        Harness harness(withLaptop(Json::object()));
        harness.engine.failRemove = true;
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        remove(harness);
        CHECK(pumpUntil([&] { return !harness.warnings.empty(); }));
        CHECK(harness.warnings == std::vector<QString>{QStringLiteral(
                                      "Could not remove computer: The device list is read-only.")});
    }
    SECTION("A18-50: a computer without a name is This computer") {
        Harness harness(withLaptop(stored, ""));
        harness.removeAnswer = fv::RemoteEnginePrompts::RemoveChoice::Cancel;
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        remove(harness);
        CHECK(harness.questions == std::vector<QString>{QStringLiteral(
                                       "Remove paired computer: This computer keeps 2 dictations "
                                       "(2.0 KB) here.")});
    }
    SECTION("the real question box: its wording and Keep as the default") {
        QString informative;
        QString defaultText;
        QString text;
        QString title;
        Harness::Options options = withLaptop(stored);
        options.customize = [](fv::RemoteEnginePageServices& services) {
            services.prompts.removeDevice = nullptr;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        // Answers the box once it is open, from the event loop its exec() runs.
        auto* poll = new QTimer(harness.page.get());
        poll->setInterval(10);
        QObject::connect(poll, &QTimer::timeout, [&, poll] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(widget);
                if (box == nullptr || !box->isVisible()) {
                    continue;
                }
                poll->stop();
                title = box->windowTitle();
                text = box->text();
                informative = box->informativeText();
                defaultText = box->defaultButton() ? box->defaultButton()->text() : QString();
                for (QAbstractButton* button : box->buttons()) {
                    if (button->text() == QStringLiteral("Delete its records")) {
                        CHECK(box->buttonRole(button) == QMessageBox::DestructiveRole);
                    }
                }
                box->defaultButton()->click();
                return;
            }
        });
        poll->start();
        remove(harness);
        CHECK(title == QStringLiteral("Remove paired computer"));
        CHECK(text == QStringLiteral("Laptop keeps 2 dictations (2.0 KB) here."));
        CHECK(informative ==
              QStringLiteral("Keep them in this computer's History and Past Meetings, or delete "
                             "them for good? Deleted records can't be recovered, and that "
                             "computer can't bring them back."));
        CHECK(defaultText == QStringLiteral("Keep its records"));
        CHECK(pumpUntil([&] { return harness.called("remove_device(laptop-id,false)"); }));
    }
}

// ---- A18-30, A18-51: recovering an old pairing's records ----

TEST_CASE("a18 rows: the recovery dialog says what each field is", kTags) {
    fv::remote::PairedDevice device;
    device.id = "new-laptop";
    device.name = "Laptop";
    fv::RecordRecoveryDialog dialog({{"old-laptop", "Laptop", {{"dictation", 2}}}}, {device});
    CHECK(dialog.windowTitle() == QStringLiteral("Recover stored records"));
    CHECK(dialog.minimumWidth() == 300);
    CHECK(dialog.size() == QSize(460, 280));
    CHECK(dialog.ownerCombo()->accessibleName() ==
          QStringLiteral("Stored records from old computer"));
    CHECK(dialog.deviceCombo()->objectName() == QStringLiteral("remoteRecoveryDeviceCombo"));
    CHECK(dialog.deviceCombo()->accessibleName() ==
          QStringLiteral("Paired computer to receive access"));
    QStringList labels;
    for (auto* label : dialog.findChildren<QLabel*>()) {
        labels.append(label->text());
    }
    CHECK(labels.contains(QStringLiteral(
        "Choose the old computer's records and its new pairing. The selected paired computer "
        "will be able to read, edit, and delete these records.")));
    CHECK(labels.contains(QStringLiteral("Stored records from")));
    CHECK(labels.contains(QStringLiteral("Assign to paired computer")));
    CHECK(dialog.recoverButton()->text() == QStringLiteral("Recover records"));
}

TEST_CASE("a18 rows: recovering says it is under way, then how it went", kTags) {
    const auto options = [](bool fail) {
        Harness::Options made;
        made.script = [fail](auto& engine, auto&) {
            fv::remote::PairedDevice device;
            device.id = "new-laptop";
            device.name = "Laptop";
            engine.state.devices = {device};
            engine.owners = {{"old-laptop", "Laptop", {{"dictation", 2}}}};
            engine.failRecover = fail;
            engine.recoverGate.close();
        };
        made.customize = [](fv::RemoteEnginePageServices& services) {
            services.prompts.recoverRecords =
                [](QWidget*, const auto&,
                   const auto&) -> std::optional<std::pair<std::string, std::string>> {
                return std::make_pair(std::string("old-laptop"), std::string("new-laptop"));
            };
        };
        return made;
    };
    for (const bool fail : {false, true}) {
        CAPTURE(fail);
        Harness harness(options(fail));
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        auto& page = *harness.page;
        CHECK(page.recoverRecordsButton()->toolTip() ==
              QStringLiteral("Reconnect records from an old pairing to a paired computer"));
        page.recoverRecordsButton()->click();
        REQUIRE(pumpUntil([&] { return harness.engine.recoverGate.entered() == 1; }));
        CHECK(page.recoveryMessage()->text() == QStringLiteral("Recovering stored records…"));
        CHECK_FALSE(page.recoveryMessage()->isHidden());
        CHECK(page.recoveryBusy());
        CHECK(pumpUntil([&] { return !page.recoverRecordsButton()->isEnabled(); }));
        harness.engine.recoverGate.open();
        REQUIRE(pumpUntil([&] { return !page.recoveryBusy() && harness.settle(); }));
        CHECK(page.recoveryMessage()->text() ==
              (fail ? QStringLiteral("Could not recover records: The record store is locked.")
                    : QStringLiteral("Stored records are available to the paired computer. "
                                     "Refresh its history to see them.")));
        CHECK(page.recoverRecordsButton()->isEnabled());
    }
}

TEST_CASE("a18 rows: recovery says when nothing needs it, or that it couldn't look", kTags) {
    SECTION("paired computers, but no old records") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            fv::remote::PairedDevice device;
            device.id = "new-laptop";
            device.name = "Laptop";
            engine.state.devices = {device};
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        harness.page->recoverRecordsButton()->click();
        CHECK(pumpUntil([&] { return !harness.informations.empty(); }));
        CHECK(harness.informations ==
              std::vector<QString>{QStringLiteral(
                  "Recover stored records: No records from unpaired computers need recovery.")});
    }
    SECTION("the stored records can't be read") {
        Harness::Options options;
        options.script = [](auto& engine, auto&) {
            engine.failOwners = true;
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        harness.page->recoverRecordsButton()->click();
        CHECK(pumpUntil([&] { return !harness.warnings.empty(); }));
        CHECK(harness.warnings ==
              std::vector<QString>{
                  QStringLiteral("Could not load stored records: The record store is locked.")});
        CHECK(harness.informations.empty());
    }
}

// ---- A18-31: what the page hears ----

TEST_CASE("a18 rows: activity reads nothing, other changes read the state again", kTags) {
    Harness harness;
    REQUIRE(harness.show());
    REQUIRE(harness.settle());
    pumpFor(std::chrono::milliseconds(50));
    const int before = countCalls(harness.engine.calls(), "host_state()");
    std::thread([&] { harness.engine.emit_changed("activity"); }).join();
    pumpFor(std::chrono::milliseconds(150));
    REQUIRE(harness.settle());
    CHECK(countCalls(harness.engine.calls(), "host_state()") == before);
    std::thread([&] { harness.engine.emit_changed("devices"); }).join();
    CHECK(pumpUntil(
        [&] { return countCalls(harness.engine.calls(), "host_state()") == before + 1; }));
}

TEST_CASE("a18 rows: a records job's progress replaces its busy line", kTags) {
    SECTION("an outbox event") {
        RecordsHarness harness([](HeldEngine& engine, HeldRecords& records) {
            pairedRecords(engine, records);
            records.status_value.location = "host";
            records.jobGate.close();
        });
        REQUIRE(harness.base.show());
        REQUIRE(harness.base.settle());
        auto& page = harness.page();
        page.sendExistingButton()->click();
        REQUIRE(pumpUntil([&] { return harness.records.jobGate.entered() == 1; }));
        std::thread([&] { harness.records.changed.emit("progress:Sending 2 of 4…"); }).join();
        CHECK(pumpUntil(
            [&] { return page.storageStatus()->text() == QStringLiteral("Sending 2 of 4…"); }));
        harness.records.jobGate.open();
        REQUIRE(pumpUntil([&] { return !page.recordsBusy(); }));
    }
    SECTION("A18-53: bringing back from here asks nothing and shows its progress") {
        RecordsHarness harness([](HeldEngine& engine, HeldRecords& records) {
            pairedRecords(engine, records);
            records.status_value.location = "local";
            records.status_value.stored = {{"dictation", {3, 0}}};
            records.progress_lines = {"Bringing back 3…"};
            records.bring_back_result = 3;
            records.jobGate.close();
        });
        REQUIRE(harness.base.show());
        REQUIRE(harness.base.settle());
        auto& page = harness.page();
        page.bringBackButton()->click();
        CHECK(harness.base.questions.empty());
        REQUIRE(pumpUntil([&] { return harness.records.jobGate.entered() == 1; }));
        CHECK(pumpUntil([&] {
            return page.storageStatus()->text().startsWith(QStringLiteral("Bringing back 3…"));
        }));
        harness.records.jobGate.open();
        REQUIRE(pumpUntil([&] { return !page.recordsBusy(); }));
        CHECK(harness.recordsCalled("bring_back()"));
        CHECK_FALSE(harness.recordsCalled("set_location(local)"));
        CHECK(pumpUntil([&] {
            return page.storageStatus()->text().endsWith(
                QStringLiteral("Brought 3 records back from devbox."));
        }));
    }
}

// ---- A18-50: a records job that fails ----

TEST_CASE("a18 rows: a failed records job shows its error, or its class without one", kTags) {
    struct Case {
        std::exception_ptr error;
        QString shown;
    };
    const std::vector<Case> cases = {
        {std::make_exception_ptr(std::runtime_error("The host is full.")),
         QStringLiteral("The host is full.")},
        {std::make_exception_ptr(fv::remote::RecordsUnavailable("")),
         QStringLiteral("RecordsUnavailable")},
    };
    for (const Case& item : cases) {
        CAPTURE(item.shown);
        Harness::Options options;
        options.script = [&item](auto& engine, auto& records) {
            pairedRecords(engine, records);
            records.status_value.location = "host";
        };
        Harness harness(options);
        REQUIRE(harness.show());
        REQUIRE(harness.settle());
        // After showing the page, whose refresh_summary would take the failure otherwise.
        harness.records.fail_next(item.error);
        harness.page->sendExistingButton()->click();
        REQUIRE(pumpUntil([&] { return !harness.page->recordsBusy(); }));
        CHECK(pumpUntil([&] { return harness.page->storageStatus()->text() == item.shown; }));
    }
}

// ---- A18-53: choosing where records go ----

TEST_CASE("a18 rows: choosing the location already chosen saves nothing", kTags) {
    Harness::Options options;
    options.script = [](auto& engine, auto& records) {
        pairedRecords(engine, records);
        records.status_value.location = "both";
    };
    Harness harness(options);
    REQUIRE(harness.show());
    REQUIRE(harness.settle());
    harness.page->locationButton("both")->click();
    pumpFor(std::chrono::milliseconds(100));
    REQUIRE(harness.settle());
    CHECK(countCalls(harness.records.calls(), "set_location(") == 0);
    harness.page->locationButton("host")->click();
    CHECK(pumpUntil([&] { return harness.recordsCalled("set_location(host)"); }));
}
