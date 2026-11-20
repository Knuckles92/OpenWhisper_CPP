// Settings → Remote engine over the remote fakes: finding and pairing, the paired host, sharing,
// the paired computers, the name dialog and the credential-store recovery. The OpenWhisper tests
// drive the real service over loopback; these drive the page's side of the same flows through
// FakeRemoteEngine, whose calls run on the page's io pool as the service's would.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTimer>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/registries.h"
#include "base/testing/manual_executor.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "remote/errors.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "speech/testing/fake_backend.h"
#include "ui_remote/remote_feature.h"
#include "ui_remote/remote_name_dialog.h"
#include "ui_remote/remote_text.h"
#include "ui_remote_support.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"

using fv::Json;
using fv::remote::ClientPairing;
using fv::remote::NearbyHost;
using fvtest::PageHarness;
using fvtest::pumpFor;
using fvtest::pumpUntil;

namespace {

const std::string kFingerprint = std::string(32, 'A') + std::string(32, 'B');

// A computer that answered the search on this network.
NearbyHost lanHost(std::string name = "devbox", bool approval = true,
                   Json engine = fvtest::fakeEngine(), std::string address = "192.168.1.20") {
    NearbyHost host;
    host.host_name = name;
    host.fingerprint = kFingerprint;
    host.engine = engine;
    host.compatible = true;
    host.approval = approval;
    fv::remote::LanHost lan;
    lan.address = std::move(address);
    lan.port = 47821;
    lan.host_name = name;
    lan.fingerprint = kFingerprint;
    lan.engine = engine;
    lan.protocol = 1;
    lan.approval = approval;
    host.lan = lan;
    return host;
}

fv::net::TailscaleStatus tailscaleState(std::string state) {
    fv::net::TailscaleStatus status;
    status.state = std::move(state);
    return status;
}

// Pairing requests wait until the test answers for the host's owner, or the page cancels; the
// pairing allowed is the fake's pair_result.
class AskingEngine : public fv::remote::testing::FakeRemoteEngine {
public:
    ClientPairing request_pairing(std::string_view address,
                                  std::function<void(std::string sas)> on_code,
                                  fv::CancelToken cancel) override {
        {
            std::lock_guard lock(gate_);
            asking_ = true;
        }
        if (on_code) {
            on_code("123456");
        }
        while (!cancel.wait_for(std::chrono::milliseconds(20))) {
            std::lock_guard lock(gate_);
            if (allowed_) {
                asking_ = false;
                return FakeRemoteEngine::request_pairing(address, {}, cancel);
            }
        }
        std::lock_guard lock(gate_);
        asking_ = false;
        throw fv::remote::PairingCanceled("Pairing canceled.");
    }
    void allow() {
        std::lock_guard lock(gate_);
        allowed_ = true;
    }
    bool asking() const {
        std::lock_guard lock(gate_);
        return asking_;
    }

private:
    mutable std::mutex gate_;
    bool allowed_ = false;
    bool asking_ = false;
};
// The Settings page over an engine of the test's own type.
template <class Engine> struct OwnEngine {
    Engine engine;
    fv::remote::testing::FakeRecordSync records;
    fv::FakeClock clock{fvtest::referenceTime(), "America/Los_Angeles"};
    fv::ThreadPool pool{"remote-page-test-io", 4};
    fv::SettingsPage container;
    std::unique_ptr<fv::RemoteEnginePage> page;

    void build() {
        fv::RemoteEnginePageServices services;
        services.service = &engine;
        services.records = &records;
        services.io = &pool;
        services.clock = &clock;
        services.openUrl = [](const QUrl&) {
        };
        page = std::make_unique<fv::RemoteEnginePage>(container, std::move(services));
        page->resize(900, 1200);
        page->show();
        page->onShown();
    }
    ~OwnEngine() { page.reset(); }
    bool settled() const { return !page->refreshing() && !page->scanBusy(); }
    bool called(const std::string& call) const {
        const auto calls = engine.calls();
        return std::find(calls.begin(), calls.end(), call) != calls.end();
    }
    QStringList rows() const {
        QStringList texts;
        for (fv::WrappedLabel* label : page->findList()->findChildren<fv::WrappedLabel*>()) {
            if (label->objectName() == QStringLiteral("remoteFindLabel") && !label->isHidden()) {
                texts.append(label->text());
            }
        }
        return texts;
    }
};

QPushButton* connectButton(fv::RemoteEnginePage& page) {
    for (auto* button : page.findList()->findChildren<fv::PrimaryButton*>()) {
        if (button->text() == QStringLiteral("Connect") && !button->isHidden()) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

// from
// tests/test_remote_pairing_ux.py::test_page_pairs_with_a_computer_on_this_network_once_it_is_allowed
TEST_CASE("page pairs with a computer on this network once it is allowed",
          "[test_remote_pairing_ux]") {
    OwnEngine<AskingEngine> harness;
    harness.engine.scan.status = tailscaleState("not_installed");
    harness.engine.scan.hosts = {lanHost()};
    ClientPairing allowed = fvtest::pairedWith();
    allowed.via = "approval";
    harness.engine.pair_result = allowed;
    harness.build();
    REQUIRE(pumpUntil([&] { return harness.page->hasScan() && harness.settled(); }));
    CHECK(harness.rows() ==
          QStringList{QStringLiteral("devbox · Fake Parakeet on cuda · on this network")});
    QPushButton* connect = connectButton(*harness.page);
    REQUIRE(connect != nullptr);
    connect->click();

    REQUIRE(pumpUntil([&] { return !harness.page->waitBox()->isHidden(); }));
    CHECK(harness.page->waitCodeLabel()->text() == QStringLiteral("123 456"));
    CHECK(harness.page->waitLabel()->text().contains(
        QStringLiteral("On devbox, click Allow if it shows this number")));
    CHECK_FALSE(harness.page->searchButton()->isEnabled());
    CHECK(harness.engine.asking());

    harness.engine.allow();
    REQUIRE(pumpUntil([&] { return !harness.page->pairingBusy() && harness.settled(); }));
    const auto pairing = harness.engine.client_pairing();
    REQUIRE(pairing.has_value());
    CHECK(pairing->via == "approval");
    CHECK(harness.page->clientMessage()->text() ==
          QStringLiteral("Paired with devbox. Click \"Use for dictation\" to start using it."));
    CHECK(harness.page->findTile()->isHidden());
    CHECK(harness.page->waitBox()->isHidden());
    // The paired tile replaces the search.
    CHECK_FALSE(harness.page->clientTile()->isHidden());
    CHECK(harness.page->clientTile()->descriptionLabel()->text().startsWith(
        QStringLiteral("Paired with devbox at 192.168.1.20:47821.")));
    CHECK(harness.page->pairedRow()->isVisibleTo(harness.page->clientTile()));
    // Paired on this network only: Tailscale is how it works away from home.
    CHECK_FALSE(harness.page->tailscaleHint()->isHidden());
    CHECK(harness.page->tailscaleHint()->text().contains(QStringLiteral("away from home")));
    CHECK(harness.page->tailscaleHint()->text().contains(
        QStringLiteral("https://tailscale.com/download")));
}

// from tests/test_remote_pairing_ux.py::test_page_cancels_a_request_still_waiting
TEST_CASE("page cancels a request still waiting", "[test_remote_pairing_ux]") {
    OwnEngine<AskingEngine> harness;
    harness.engine.scan.status = tailscaleState("not_installed");
    harness.engine.scan.hosts = {lanHost()};
    harness.build();
    REQUIRE(pumpUntil([&] { return harness.page->hasScan() && harness.settled(); }));
    REQUIRE(connectButton(*harness.page) != nullptr);
    connectButton(*harness.page)->click();
    REQUIRE(pumpUntil([&] { return !harness.page->waitBox()->isHidden(); }));
    harness.page->waitCancelButton()->click();
    CHECK(harness.page->findMessage()->text() == QStringLiteral("Canceling..."));
    REQUIRE(pumpUntil([&] { return !harness.page->pairingBusy(); }));
    CHECK(harness.page->findMessage()->text() == QStringLiteral("Pairing canceled."));
    CHECK_FALSE(harness.engine.client_pairing().has_value());
    CHECK(harness.page->waitBox()->isHidden());
    CHECK(pumpUntil([&] { return !harness.engine.asking(); }, std::chrono::milliseconds(3000)));
}

// Closing Settings while the host's owner has not answered ends the request: it does not keep
// an io thread waiting on the host (open item "Remote engine page: a pairing request waiting").
TEST_CASE("closing the page cancels a pairing request still waiting", "[remote_page]") {
    OwnEngine<AskingEngine> harness;
    harness.engine.scan.status = tailscaleState("not_installed");
    harness.engine.scan.hosts = {lanHost()};
    harness.build();
    REQUIRE(pumpUntil([&] { return harness.page->hasScan() && harness.settled(); }));
    REQUIRE(connectButton(*harness.page) != nullptr);
    connectButton(*harness.page)->click();
    REQUIRE(pumpUntil([&] { return !harness.page->waitBox()->isHidden(); }));
    REQUIRE(harness.engine.asking());

    harness.page.reset();

    CHECK(pumpUntil([&] { return !harness.engine.asking(); }, std::chrono::milliseconds(3000)));
    CHECK_FALSE(harness.engine.client_pairing().has_value());
    harness.engine.allow(); // without the cancel, ends the wait so the io pool can join
}

// from tests/test_remote_pairing_ux.py::test_page_offers_a_code_for_a_host_too_old_to_ask
TEST_CASE("page offers a code for a host too old to ask", "[test_remote_pairing_ux]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.scan.status = tailscaleState("not_installed");
        engine.scan.hosts = {lanHost(
            "oldbox", false, Json{{"label", "Whisper"}, {"available", true}, {"device", "cpu"}},
            "127.0.0.1")};
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    CHECK(harness.rows() ==
          QStringList{QStringLiteral("oldbox · Whisper on cpu · on this network")});
    QPushButton* useCode =
        PageHarness::button(harness.page->findList(), QStringLiteral("Use a code"));
    REQUIRE(useCode != nullptr);
    useCode->click();
    CHECK_FALSE(harness.page->pairRow()->isHidden());
    CHECK(harness.page->addressEdit()->text() == QStringLiteral("127.0.0.1:47821"));
    CHECK(harness.page->findMessage()->text().contains(
        QStringLiteral("On oldbox, click \"Show a pairing code\"")));
}

// from
// tests/test_remote_pairing_ux.py::test_page_blames_the_firewall_when_a_found_host_wont_connect
TEST_CASE("page blames the firewall when a found host wont connect", "[test_remote_pairing_ux]") {
    NearbyHost found = lanHost("jed", true, Json::object(), "192.168.1.9");
    const QString message = fv::RemoteEnginePage::pairErrorText(
        std::make_exception_ptr(fv::remote::RemoteUnreachable("Couldn't reach 192.168.1.9:47821.")),
        found);
    CHECK(message.startsWith(
        QStringLiteral("jed answered the search but didn't accept the connection")));
    CHECK(message.contains(QStringLiteral("may be blocking FioriVoce")));
    const fv::remote::RemoteUnreachable refused(
        "Couldn't reach 192.168.1.9:47821: nothing there...", true);
    CHECK(fv::RemoteEnginePage::pairErrorText(std::make_exception_ptr(refused), found) ==
          QString::fromUtf8(refused.what()));
    CHECK(fv::RemoteEnginePage::pairErrorText(
              std::make_exception_ptr(fv::remote::RemoteUnreachable("Couldn't reach x.")),
              std::nullopt) == QStringLiteral("Couldn't reach x."));
    CHECK(
        fv::RemoteEnginePage::pairErrorText(
            std::make_exception_ptr(fv::CredentialStoreError("denied")), std::nullopt) ==
        QStringLiteral("Couldn't save the connection key. Retry access below to finish pairing."));
    CHECK(fv::RemoteEnginePage::pairErrorText(
              std::make_exception_ptr(fv::remote::PairingCanceled("Pairing canceled.")),
              std::nullopt) == QStringLiteral("Pairing canceled."));
    // An empty message falls back to the class name.
    CHECK(fv::RemoteEnginePage::pairErrorText(
              std::make_exception_ptr(fv::remote::RemoteEngineError("")), std::nullopt) ==
          QStringLiteral("RemoteEngineError"));
}

namespace {

// Finds a host only when every address is tried.
class SweepingEngine : public fv::remote::testing::FakeRemoteEngine {
public:
    fv::remote::NearbyScan scan_nearby(bool sweep) override {
        fv::remote::NearbyScan result = FakeRemoteEngine::scan_nearby(sweep);
        if (sweep) {
            result.hosts = {lanHost()};
        }
        return result;
    }
};

} // namespace

// from
// tests/test_remote_pairing_ux.py::test_page_suggests_checking_every_address_when_nothing_answers
TEST_CASE("page suggests checking every address when nothing answers", "[test_remote_pairing_ux]") {
    OwnEngine<SweepingEngine> harness;
    harness.engine.scan.status = tailscaleState("not_installed");
    harness.build();
    REQUIRE(pumpUntil([&] { return harness.page->hasScan() && harness.settled(); }));
    CHECK(harness.rows().isEmpty());
    CHECK_FALSE(harness.page->sweepButton()->isHidden());
    harness.page->sweepButton()->click();
    CHECK(harness.page->findTile()->descriptionLabel()->text() ==
          QStringLiteral("Checking every address on this network..."));
    REQUIRE(pumpUntil([&] { return harness.settled() && harness.page->scanSwept(); }));
    CHECK(harness.rows() ==
          QStringList{QStringLiteral("devbox · Fake Parakeet on cuda · on this network")});
    CHECK(harness.page->sweepButton()->isHidden());
}

// from tests/test_remote_pairing_ux.py::test_host_page_shows_what_may_keep_others_out
TEST_CASE("host page shows what may keep others out", "[test_remote_pairing_ux]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.host_name = "devbox";
        engine.state.engine = fvtest::fakeEngine();
        engine.state.notes = {{"public_network", "Windows treats \"Home\" as a public network.",
                               "Open network settings", "ms-settings:network-wifi"}};
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    harness.page->shareTile()->checkbox()->setChecked(true);
    REQUIRE(pumpUntil([&] { return !harness.page->hostNotes()->isHidden(); }));
    QStringList texts;
    for (auto* label : harness.page->hostNotes()->findChildren<fv::WrappedLabel*>()) {
        texts.append(label->text());
    }
    CHECK(texts == QStringList{QStringLiteral("Windows treats \"Home\" as a public network.")});
    QPushButton* open =
        PageHarness::button(harness.page->hostNotes(), QStringLiteral("Open network settings"));
    REQUIRE(open != nullptr);
    open->click();
    CHECK(harness.opened == std::vector<QString>{QStringLiteral("ms-settings:network-wifi")});
    harness.page->shareTile()->checkbox()->setChecked(false);
    CHECK(pumpUntil([&] { return harness.page->hostNotes()->isHidden(); }));
}

namespace {

// A search that waits until the test lets it finish.
class SlowSearchEngine : public fv::remote::testing::FakeRemoteEngine {
public:
    fv::remote::NearbyScan scan_nearby(bool sweep) override {
        {
            std::unique_lock lock(mutex_);
            searching_ = true;
            released_.wait_for(lock, std::chrono::seconds(5), [this] { return release_; });
        }
        return FakeRemoteEngine::scan_nearby(sweep);
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            release_ = true;
        }
        released_.notify_all();
    }
    bool searching() const {
        std::lock_guard lock(mutex_);
        return searching_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable released_;
    bool release_ = false;
    bool searching_ = false;
};

} // namespace

// from tests/test_remote_pairing_ux.py::test_a_page_closed_mid_search_is_not_held_by_its_threads
TEST_CASE("a page closed mid search is not held by its threads", "[test_remote_pairing_ux]") {
    // The search worker and the service's listeners used to reach the page from their own
    // threads, which raced Settings closing; the listeners also kept a closed page alive.
    SlowSearchEngine engine;
    fv::remote::testing::FakeRecordSync records;
    fv::FakeClock clock(fvtest::referenceTime(), "America/Los_Angeles");
    {
        fv::ThreadPool pool("remote-page-test-io", 4);
        {
            fv::SettingsPage container;
            fv::RemoteEnginePageServices services;
            services.service = &engine;
            services.records = &records;
            services.io = &pool;
            services.clock = &clock;
            auto* parent = new QWidget;
            auto* page = new fv::RemoteEnginePage(container, std::move(services), parent);
            page->scanNearby(true);
            REQUIRE(pumpUntil([&] { return engine.searching(); }));
            CHECK(engine.changed.subscriber_count() == 1);
            CHECK(records.changed.subscriber_count() == 1);
            delete parent;
            // The closed page's listeners are gone with it.
            CHECK(engine.changed.subscriber_count() == 0);
            CHECK(records.changed.subscriber_count() == 0);
        }
        engine.release();
        std::thread told([&] {
            engine.emit_changed("state");
            records.changed.emit("records");
        });
        told.join();
        pumpFor(std::chrono::milliseconds(100));
    }
    QCoreApplication::processEvents();
    SUCCEED("nothing reached the closed page");
}

namespace {

// Pairing with this computer's own host lists this computer among its paired computers, as the
// real service paired with itself does.
class SelfPairingEngine : public fv::remote::testing::FakeRemoteEngine {
public:
    ClientPairing pair(std::string_view address, std::string_view code, bool tailscale) override {
        ClientPairing made = FakeRemoteEngine::pair(address, code, tailscale);
        fv::remote::PairedDevice device;
        device.id = "this-device";
        device.name = "laptop";
        device.paired_at = "2026-11-14T09:00:00+00:00";
        device.via = tailscale ? "tailscale" : "code";
        {
            std::lock_guard lock(own_);
            state.devices.push_back(device);
        }
        changed.emit("devices");
        return made;
    }
    // Reads of the devices wait for a pairing adding one.
    fv::remote::HostState host_state() override {
        std::lock_guard lock(own_);
        return FakeRemoteEngine::host_state();
    }

private:
    std::mutex own_;
};

} // namespace

// from tests/test_remote_engine.py::test_settings_page_pairs_with_a_host_and_lists_the_device
TEST_CASE("settings page pairs with a host and lists the device", "[test_remote_engine]") {
    OwnEngine<SelfPairingEngine> harness;
    harness.engine.state.host_name = "devbox";
    harness.engine.state.engine = fvtest::fakeEngine();
    harness.engine.state.address = "192.168.1.20";
    harness.engine.scan.status = tailscaleState("not_installed");
    harness.build();
    REQUIRE(pumpUntil([&] { return harness.page->hasScan() && harness.settled(); }));
    QSignalSpy selected(harness.page.get(), &fv::RemoteEnginePage::useForDictationRequested);
    fv::RemoteEnginePage& page = *harness.page;
    // Unpaired: the search comes first, typing an address second.
    CHECK_FALSE(page.findTile()->isHidden());
    CHECK(page.clientTile()->isHidden());
    CHECK(page.pairRow()->isHidden());
    page.manualButton()->click();
    CHECK_FALSE(page.pairRow()->isHidden());
    CHECK(page.manualButton()->isHidden());
    CHECK_FALSE(page.pairDeviceButton()->isEnabled());

    page.shareTile()->checkbox()->setChecked(true);
    REQUIRE(pumpUntil([&] {
        return page.hostStatus()->text().contains(QStringLiteral("Sharing Fake Parakeet on cuda"));
    }));
    // Nothing is paired yet, so turning sharing on has a code ready.
    REQUIRE(pumpUntil([&] { return !page.pairingCodeLabel()->text().isEmpty(); }));
    CHECK(page.pairingCodeLabel()->text() == QStringLiteral("123 456"));
    CHECK(page.pairingExpiryLabel()->text() ==
          QStringLiteral("Enter this code on the other computer. Expires in 5:00."));
    CHECK(page.pairDeviceButton()->isHidden());

    page.addressEdit()->setText(QStringLiteral("127.0.0.1:47821"));
    page.codeEdit()->setText(QStringLiteral("123456"));
    page.pairButton()->click();
    REQUIRE(pumpUntil([&] { return !page.pairingBusy() && harness.settled(); }));
    CHECK(harness.called("pair(127.0.0.1:47821,123456,false)"));
    CHECK(page.clientMessage()->text().startsWith(QStringLiteral("Paired with ")));
    CHECK(harness.engine.client_pairing().has_value());
    CHECK(page.pairedRow()->isVisibleTo(page.clientTile()));
    CHECK(page.findTile()->isHidden());
    CHECK_FALSE(page.clientTile()->isHidden());
    bool listed = false;
    for (auto* label : page.devicesList()->findChildren<fv::WrappedLabel*>()) {
        listed = listed || label->text().contains(QStringLiteral("paired"));
    }
    CHECK(listed);

    page.useButton()->click();
    CHECK(selected.count() == 1);
    CHECK(page.clientMessage()->text() == QStringLiteral("Dictation now uses 127.0.0.1's engine."));
    CHECK(page.railValue().startsWith(QStringLiteral("Sharing · 1 paired")));

    page.forgetButton()->click();
    REQUIRE(pumpUntil([&] { return !harness.engine.client_pairing().has_value(); }));
    CHECK(pumpUntil([&] {
        return page.clientMessage()->text() ==
               QStringLiteral("Forgot the host. Pair again to use it.");
    }));
    page.shareTile()->checkbox()->setChecked(false);
    CHECK(pumpUntil([&] { return !harness.engine.host_state().running; }));
}

// from
// tests/test_remote_engine.py::test_settings_page_connects_to_a_tailnet_computer_without_a_code
TEST_CASE("settings page connects to a tailnet computer without a code", "[test_remote_engine]") {
    OwnEngine<SelfPairingEngine> harness;
    fv::net::TailscaleStatus tailnet = tailscaleState("running");
    tailnet.name = "laptop";
    tailnet.address = "100.64.0.1";
    tailnet.owner = "owner@example.com";
    auto& engine = harness.engine;
    engine.state.enabled = true;
    engine.state.running = true;
    engine.state.host_name = "laptop";
    engine.state.address = "192.168.1.30";
    engine.state.engine = fvtest::fakeEngine();
    engine.state.tailscale = tailnet;
    engine.state.tailscale_trust = true;
    engine.scan.status = tailnet;
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
    engine.scan.hosts = {devbox};
    ClientPairing result = fvtest::pairedWith("100.64.0.2", "devbox");
    result.via = "tailscale";
    engine.pair_result = result;
    harness.build();
    fv::RemoteEnginePage& page = *harness.page;
    REQUIRE(pumpUntil([&] { return page.hasScan() && harness.settled(); }));
    CHECK_FALSE(page.findTile()->isHidden());
    CHECK(page.findTile()->descriptionLabel()->text().contains(
        QStringLiteral("Signed in to Tailscale as owner@example.com")));
    CHECK(harness.rows() ==
          QStringList{QStringLiteral("devbox · Fake Parakeet on cuda · your computer")});
    CHECK_FALSE(page.tailscaleTile()->isHidden());
    CHECK(page.tailscaleTile()->checkbox()->isChecked());
    CHECK(page.hostStatus()->text().contains(
        QStringLiteral("and on Tailscale as laptop (100.64.0.1)")));

    QPushButton* connect = connectButton(page);
    REQUIRE(connect != nullptr);
    connect->click();
    REQUIRE(pumpUntil([&] { return !page.pairingBusy() && harness.settled(); }));
    CHECK(harness.called("pair(100.64.0.2:47821,,true)"));
    const auto pairing = engine.client_pairing();
    REQUIRE(pairing.has_value());
    CHECK(pairing->via == "tailscale");
    CHECK(page.clientMessage()->text().contains(QStringLiteral("through your Tailscale account")));
    CHECK(page.findTile()->isHidden());
    // Paired over Tailscale already works away from home.
    CHECK(page.tailscaleHint()->isHidden());
    bool overTailscale = false;
    for (auto* label : page.devicesList()->findChildren<fv::WrappedLabel*>()) {
        overTailscale = overTailscale || label->text().contains(QStringLiteral("over Tailscale"));
    }
    CHECK(overTailscale);

    page.tailscaleTile()->checkbox()->setChecked(false);
    CHECK(pumpUntil([&] { return !engine.host_state().tailscale_trust; }));
    CHECK(harness.called("set_tailscale_trust(false)"));
}

// from tests/test_remote_engine.py::test_settings_page_explains_how_to_get_tailscale
TEST_CASE("settings page explains how to get tailscale", "[test_remote_engine]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.scan.status = tailscaleState("not_installed");
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    CHECK(harness.page->findTile()->descriptionLabel()->text() ==
          QStringLiteral("Computers on this network that share their engine."));
    const QStringList notes = harness.findTexts();

    REQUIRE_FALSE(notes.isEmpty());
    CHECK(notes.front().startsWith(QStringLiteral("None found.")));
    bool offered = false;
    for (const QString& note : notes) {
        offered = offered || (note.contains(QStringLiteral("On a different network?")) &&
                              note.contains(QStringLiteral("https://tailscale.com/download")));
    }
    CHECK(offered);
    CHECK_FALSE(harness.page->sweepButton()->isHidden());
    CHECK(harness.page->tailscaleTile()->isHidden());
}

// from
// tests/test_remote_model_management.py::test_settings_tile_is_off_by_default_and_only_changes_host_permission
TEST_CASE("settings tile is off by default and only changes host permission",
          "[test_remote_model_management]") {
    PageHarness harness;
    REQUIRE(harness.show());
    CHECK_FALSE(harness.page->managementTile()->checkbox()->isChecked());
    harness.page->managementTile()->checkbox()->setChecked(true);
    CHECK(pumpUntil([&] { return harness.engine.host_state().model_management; }));
    // Permission alone never starts sharing.
    CHECK_FALSE(harness.engine.host_state().enabled);
    CHECK_FALSE(harness.called("set_host_enabled(true)"));
    CHECK(harness.page->manageButton()->text() == QStringLiteral("Manage host models"));
    harness.page->managementTile()->checkbox()->setChecked(false);
    CHECK(pumpUntil([&] { return harness.called("set_model_management(false)"); }));
    CHECK_FALSE(harness.engine.host_state().model_management);
}

// from tests/test_remote_rename.py::test_the_name_dialog_starts_from_the_current_name
TEST_CASE("the name dialog starts from the current name", "[test_remote_rename]") {
    struct Seen {
        QString text;
        QString placeholder;
        int maxLength = 0;
    };
    std::vector<Seen> seen;
    const auto type = [&](const QString& name, bool accept) {
        QTimer::singleShot(0, [&seen, name, accept] {
            auto* dialog = qobject_cast<fv::RemoteNameDialog*>(QApplication::activeModalWidget());
            if (dialog == nullptr) {
                return;
            }
            seen.push_back({dialog->edit()->text(), dialog->edit()->placeholderText(),
                            dialog->edit()->maxLength()});
            dialog->edit()->setText(name);
            if (accept) {
                dialog->accept();
            } else {
                dialog->reject();
            }
        });
    };
    type(QStringLiteral("Studio PC"), true);
    CHECK(fv::RemoteNameDialog::ask(nullptr, QStringLiteral("Rename host"), QString(),
                                    QStringLiteral("Old name"), QStringLiteral("DESKTOP-4K2J9")) ==
          QStringLiteral("Studio PC"));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].text == QStringLiteral("Old name"));
    CHECK(seen[0].placeholder == QStringLiteral("DESKTOP-4K2J9"));
    CHECK(seen[0].maxLength == static_cast<int>(fv::remote::MAX_NAME));
    type(QStringLiteral("ignored"), false);
    CHECK_FALSE(fv::RemoteNameDialog::ask(nullptr, QStringLiteral("Rename host"), QString(),
                                          QStringLiteral("Old name"),
                                          QStringLiteral("DESKTOP-4K2J9"))
                    .has_value());

    fv::RemoteNameDialog dialog(QStringLiteral("Rename host"), QStringLiteral("text"),
                                QStringLiteral("Old name"), QStringLiteral("own"));
    CHECK(dialog.objectName() == QStringLiteral("remoteRenameDialog"));
    CHECK(dialog.edit()->accessibleName() == QStringLiteral("Name"));
    CHECK(dialog.edit()->selectedText() == QStringLiteral("Old name"));
    CHECK(dialog.renameButton()->isDefault());
    CHECK(dialog.renameButton()->objectName() == QStringLiteral("primaryButton"));
}

// from tests/test_remote_rename.py::test_settings_renames_a_paired_computer
TEST_CASE("settings renames a paired computer", "[test_remote_rename]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        fv::remote::PairedDevice device;
        device.id = "laptop-id";
        device.name = "DESKTOP-LAPTOP";
        device.paired_at = "2026-11-14T09:00:00+00:00";
        device.via = "code";
        engine.state.devices = {device};
    };
    PageHarness harness(options);
    harness.nameAnswer = QStringLiteral("Kitchen laptop");
    REQUIRE(harness.show());
    QPushButton* rename = PageHarness::button(harness.page->devicesList(),
                                              QStringLiteral("remoteRenameDeviceButton"));
    REQUIRE(rename != nullptr);
    rename->click();
    REQUIRE(harness.asked.size() == 1);
    CHECK(harness.asked[0].title == QStringLiteral("Rename paired computer"));
    CHECK(harness.asked[0].current == QStringLiteral("DESKTOP-LAPTOP"));
    CHECK(harness.asked[0].own == QStringLiteral("DESKTOP-LAPTOP"));
    REQUIRE(pumpUntil([&] {
        for (auto* label : harness.page->devicesList()->findChildren<fv::WrappedLabel*>()) {
            if (label->objectName() == QStringLiteral("remoteDeviceLabel") && !label->isHidden() &&
                label->text().startsWith(QStringLiteral("Kitchen laptop · paired "))) {
                return label->text().contains(QStringLiteral(" as DESKTOP-LAPTOP"));
            }
        }
        return false;
    }));
    CHECK(harness.engine.host_state().devices.at(0).name == "Kitchen laptop");
}

// from tests/test_remote_rename.py::test_settings_renames_the_paired_host
TEST_CASE("settings renames the paired host", "[test_remote_rename]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.pairing = fvtest::pairedWith("192.168.1.20", "DESKTOP-4K2J9");
    };
    PageHarness harness(options);
    harness.nameAnswer = QStringLiteral("Studio PC");
    REQUIRE(harness.show());
    CHECK(harness.page->renameHostButton()->isVisibleTo(harness.page.get()));
    harness.page->renameHostButton()->click();
    REQUIRE(harness.asked.size() == 1);
    CHECK(harness.asked[0].current == QStringLiteral("DESKTOP-4K2J9"));
    CHECK(harness.asked[0].own == QStringLiteral("DESKTOP-4K2J9"));
    REQUIRE(pumpUntil([&] {
        return harness.page->clientMessage()->text() ==
               QStringLiteral("This computer now calls the host Studio PC.");
    }));
    CHECK(harness.engine.client_pairing()->host_name == "Studio PC");
    CHECK(harness.settle());
    CHECK(harness.page->clientTile()->descriptionLabel()->text().startsWith(
        QStringLiteral("Paired with Studio PC (DESKTOP-4K2J9) at 192.168.1.20")));
}

namespace {

// The credential store refuses to save the key until the test lets it.
class DeniedEngine : public fv::remote::testing::FakeRemoteEngine {
public:
    ClientPairing pair(std::string_view address, std::string_view code, bool tailscale) override {
        note();
        {
            std::lock_guard lock(denied_);
            ++requests;
            pending_name = "Test host";
        }
        (void)address, (void)code, (void)tailscale;
        throw fv::CredentialStoreError("access denied");
    }
    ClientPairing retry_pairing_save() override {
        note();
        bool allow = false;
        {
            std::lock_guard lock(denied_);
            allow = write_allowed;
        }
        if (!allow) {
            throw fv::CredentialStoreError("access denied");
        }
        ClientPairing made = fvtest::pairedWith("192.0.2.10", "Test host");
        {
            std::lock_guard lock(denied_);
            pairing = made;
            pending_name.clear();
        }
        changed.emit("client");
        return made;
    }
    // Reads of what a denied pairing changed wait for it.
    std::optional<ClientPairing> client_pairing() override {
        std::lock_guard lock(denied_);
        return FakeRemoteEngine::client_pairing();
    }
    std::string pending_pairing_name() override {
        std::lock_guard lock(denied_);
        return FakeRemoteEngine::pending_pairing_name();
    }
    void note() {
        std::lock_guard lock(denied_);
        threads.insert(std::this_thread::get_id());
    }

    std::mutex denied_;
    int requests = 0;
    bool write_allowed = false;
    std::set<std::thread::id> threads;
};

} // namespace

// from tests/test_remote_keychain_recovery.py::test_page_recovers_after_repeated_denials
TEST_CASE("page recovers after repeated denials", "[test_remote_keychain_recovery]") {
    OwnEngine<DeniedEngine> harness;
    harness.engine.scan.status = tailscaleState("not_installed");
    harness.build();
    fv::RemoteEnginePage& page = *harness.page;
    REQUIRE(pumpUntil([&] { return page.hasScan() && harness.settled(); }));
    CHECK(page.credentialRecovery()->isHidden());
    page.manualButton()->click();
    page.addressEdit()->setText(QStringLiteral("192.0.2.10"));
    page.codeEdit()->setText(QStringLiteral("123456"));
    page.pairButton()->click();
    REQUIRE(pumpUntil([&] { return !page.pairingBusy() && harness.settled(); }));
    CHECK_FALSE(page.credentialRecovery()->isHidden());
    CHECK(page.findMessage()->text().contains(QStringLiteral("Retry access below")));
    CHECK(page.credentialHelp()->text().contains(QStringLiteral("Test host")));
#ifdef _WIN32
    // The Windows wording, which is the product's; other platforms name their own store.
    CHECK(page.credentialHelp()->text() ==
          QStringLiteral("Finish pairing with Test host. Unlock or enable Windows Credential "
                         "Manager, then click Retry saving key."));
#else
    CHECK(page.credentialHelp()->text() ==
          QStringLiteral("Finish pairing with Test host. Unlock or enable %1, then click Retry "
                         "saving key.")
              .arg(QString::fromStdString(std::string(fv::platform_backend_name()))));
#endif
    CHECK(page.openKeychainButton()->isHidden());
    CHECK(page.retryKeyButton()->text() == QStringLiteral("Retry saving key"));

    page.retryKeyButton()->click();
    CHECK_FALSE(page.retryKeyButton()->isEnabled());
    REQUIRE(pumpUntil([&] { return !page.pairingBusy() && harness.settled(); }));
    CHECK_FALSE(page.credentialRecovery()->isHidden());
    CHECK(page.retryKeyButton()->isEnabled());
    CHECK_FALSE(harness.engine.client_pairing().has_value());

    {
        std::lock_guard lock(harness.engine.denied_);
        harness.engine.write_allowed = true;
    }
    page.retryKeyButton()->click();
    REQUIRE(pumpUntil([&] { return !page.pairingBusy() && harness.settled(); }));
    CHECK(page.credentialRecovery()->isHidden());
    CHECK(page.findTile()->isHidden());
    CHECK_FALSE(page.clientTile()->isHidden());
    CHECK(harness.engine.client_pairing()->host_name == "Test host");
    std::lock_guard lock(harness.engine.denied_);
    CHECK(harness.engine.requests == 1);
    CHECK_FALSE(harness.engine.threads.empty());
    CHECK_FALSE(harness.engine.threads.contains(std::this_thread::get_id()));
}

// from tests/test_remote_keychain_recovery.py::test_reopening_settings_keeps_the_recovery_action
TEST_CASE("reopening settings keeps the recovery action", "[test_remote_keychain_recovery]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.pending_name = "Test host";
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    CHECK_FALSE(harness.page->credentialRecovery()->isHidden());
}

// from
// tests/test_settings_polish.py::test_remote_engine_splits_using_another_computer_from_sharing_this_one
TEST_CASE("remote engine splits using another computer from sharing this one",
          "[test_settings_polish]") {
    PageHarness harness;
    REQUIRE(harness.show());
    fv::RemoteEnginePage& page = *harness.page;
    const auto& buttons = page.tabs()->buttons();
    REQUIRE(buttons.size() == 2);
    CHECK(buttons[0]->titleLabel()->text() == QStringLiteral("Use another computer"));
    CHECK(buttons[1]->titleLabel()->text() == QStringLiteral("Share this computer"));
    CHECK(page.tabs()->currentIndex() == 0);
    CHECK_FALSE(page.clientContainer()->isHidden());
    CHECK(page.hostContainer()->isHidden());
    CHECK(buttons[0]->detailLabel()->text() == QStringLiteral("Not paired"));
    CHECK(buttons[1]->detailLabel()->text() == QStringLiteral("Off"));
    buttons[1]->click();
    CHECK(page.clientContainer()->isHidden());
    CHECK_FALSE(page.hostContainer()->isHidden());
    // The host-side opt-ins live on the sharing tab, as switches.
    for (fv::SettingTile* tile :
         {page.managementTile(), page.keepRecordsTile(), page.manageMcpTile()}) {
        CHECK(qobject_cast<fv::SettingsSwitch*>(tile->checkbox()) != nullptr);
        CHECK(page.hostContainer()->isAncestorOf(tile));
    }
}

// from tests/test_settings_polish.py::test_remote_engine_opens_on_the_side_that_is_in_use
TEST_CASE("remote engine opens on the side that is in use", "[test_settings_polish]") {
    PageHarness harness;
    REQUIRE(harness.show());
    harness.page->shareTile()->checkbox()->setChecked(true);
    REQUIRE(pumpUntil([&] { return harness.engine.host_state().running; }));
    REQUIRE(pumpUntil([&] { return harness.page->tabs()->currentIndex() == 1; }));
    CHECK(pumpUntil([&] {
        return harness.page->tabs()->buttons()[1]->detailLabel()->text() ==
               QStringLiteral("Sharing · 0 paired");
    }));
    CHECK_FALSE(harness.page->hostContainer()->isHidden());
}

// from tests/test_settings_polish.py::test_port_arrows_restart_sharing_once_on_the_last_port
TEST_CASE("port arrows restart sharing once on the last port", "[test_settings_polish]") {
    PageHarness harness;
    REQUIRE(harness.show());
    const int start = harness.page->portSpin()->value();
    for (int step : {1, 2, 3}) {
        harness.page->portSpin()->setValue(start + step);
    }
    const auto ports = [&] {
        std::vector<std::string> set;
        for (const std::string& call : harness.engine.calls()) {
            if (call.starts_with("set_host_port(")) {
                set.push_back(call);
            }
        }
        return set;
    };
    // Not on the Qt thread, and not once per click.
    CHECK(ports().empty());
    const std::string last = "set_host_port(" + std::to_string(start + 3) + ")";
    REQUIRE(pumpUntil([&] { return ports() == std::vector<std::string>{last}; }));
    pumpFor(std::chrono::milliseconds(300));
    CHECK(ports() == std::vector<std::string>{last});
}

// from tests/test_settings_polish.py::test_remote_engine_keeps_the_tab_a_person_chose
TEST_CASE("remote engine keeps the tab a person chose", "[test_settings_polish]") {
    PageHarness harness;
    REQUIRE(harness.show());
    harness.page->tabs()->buttons()[0]->click(); // chose to look at the client side
    harness.page->shareTile()->checkbox()->setChecked(true);
    REQUIRE(pumpUntil([&] { return harness.engine.host_state().running; }));
    pumpFor(std::chrono::milliseconds(500));
    CHECK(harness.page->tabs()->currentIndex() == 0);
    CHECK_FALSE(harness.page->clientContainer()->isHidden());
}

// from tests/test_settings_polish.py::test_remote_engine_tiles_keep_the_theme_card_style
TEST_CASE("remote engine tiles keep the theme card style", "[test_settings_polish]") {
    // Renaming a tile once stripped its card styling; identifiers go in a property.
    PageHarness harness;
    fv::RemoteEnginePage& page = *harness.page;
    for (QWidget* tile : std::initializer_list<QWidget*>{
             page.clientTile(), page.shareTile(), page.managementTile(), page.keepRecordsTile(),
             page.manageMcpTile(), page.devicesTile()}) {
        CHECK(tile->objectName() == QStringLiteral("settingsTile"));
    }
    CHECK(page.manageMcpTile()->property(fv::ui::prop::tileId).toString() ==
          QStringLiteral("remoteManageMcpTile"));
}

// from tests/test_settings_polish.py::test_paired_computer_remove_buttons_keep_the_danger_style
TEST_CASE("paired computer remove buttons keep the danger style", "[test_settings_polish]") {
    // Renaming Remove once turned it into a grey block; its identifier goes in a property.
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        fv::remote::PairedDevice a;
        a.id = "dev-a";
        a.name = "Laptop";
        fv::remote::PairedDevice b;
        b.id = "dev-b";
        b.name = "Desk";
        engine.state.devices = {a, b};
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    int removes = 0;
    for (auto* button : harness.page->devicesTile()->findChildren<QAbstractButton*>()) {
        if (button->property(fv::ui::prop::buttonId).toString() ==
                QStringLiteral("remoteRemoveDeviceButton") &&
            !button->isHidden()) {
            ++removes;
            CHECK(button->objectName() == QStringLiteral("dangerButton"));
        }
    }
    CHECK(removes == 2);
}

// ---- beyond the ported tests ----

TEST_CASE("the unbound page says the remote engine isn't available", "[remote_page]") {
    PageHarness::Options options;
    options.bound = false;
    PageHarness harness(options);
    harness.page->show();
    fv::RemoteEnginePage& page = *harness.page;
    CHECK(page.clientTile()->isVisibleTo(&page));
    CHECK(page.clientTile()->descriptionLabel()->text() ==
          QStringLiteral("The remote engine isn't available in this window."));
    CHECK_FALSE(page.clientTile()->isEnabled());
    CHECK_FALSE(page.shareTile()->isEnabled());
    CHECK(page.findTile()->isHidden());
    CHECK(page.pairedRow()->isHidden());
    CHECK(page.storageTile()->isHidden());
    CHECK(page.tailscaleTile()->isHidden());
    page.onShown();
    page.scanNearby(true);
    CHECK_FALSE(page.scanBusy());
}

TEST_CASE("a failed search shows nothing and says nothing", "[remote_page]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.fail_next(std::make_exception_ptr(std::runtime_error("network down")));
    };
    PageHarness harness(options);
    harness.page->show();
    harness.page->onShown();
    REQUIRE(pumpUntil([&] { return harness.called("scan_nearby(false)") && harness.settle(); }));
    CHECK_FALSE(harness.page->hasScan());
    CHECK(harness.rows().isEmpty());
    CHECK(harness.findTexts().isEmpty());
    CHECK(harness.page->findMessage()->text().isEmpty());
}

TEST_CASE("a search in the last 20 seconds is reused unless forced", "[remote_page]") {
    PageHarness harness;
    REQUIRE(harness.show());
    const auto searches = [&] {
        int count = 0;
        for (const std::string& call : harness.engine.calls()) {
            count += call.starts_with("scan_nearby(") ? 1 : 0;
        }
        return count;
    };
    REQUIRE(searches() == 1);
    harness.page->onShown();
    REQUIRE(harness.settle());
    pumpFor(std::chrono::milliseconds(50));
    CHECK(searches() == 1);
    harness.clock.advance(std::chrono::seconds(21));
    harness.page->onShown();
    CHECK(pumpUntil([&] { return searches() == 2; }));
    REQUIRE(harness.settle());
    harness.page->searchButton()->click();
    CHECK(pumpUntil([&] { return searches() == 3; }));
}

TEST_CASE("the rail says what is going on", "[remote_page]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.pair_result = fvtest::pairedWith();
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    CHECK(harness.rail == QStringLiteral("Off"));
    harness.engine.pair("192.168.1.20", "123456", false);
    CHECK(pumpUntil([&] { return harness.rail == QStringLiteral("Paired · devbox"); }));
    harness.page->shareTile()->checkbox()->setChecked(true);
    CHECK(pumpUntil([&] { return harness.rail == QStringLiteral("Sharing · 0 paired"); }));
}

TEST_CASE("a pairing code counts down and Cancel takes it away", "[remote_page]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = true;
        engine.state.running = true;
        engine.state.pairing = std::make_pair(std::string("054321"), 54.9);
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    fv::RemoteEnginePage& page = *harness.page;
    CHECK(page.pairingCodeLabel()->text() == QStringLiteral("054 321"));
    CHECK(page.pairingExpiryLabel()->text() ==
          QStringLiteral("Enter this code on the other computer. Expires in 0:54."));
    CHECK(page.pairDeviceButton()->isHidden());
    page.cancelPairingButton()->click();
    REQUIRE(pumpUntil([&] { return harness.called("cancel_pairing()"); }));
    CHECK(pumpUntil([&] { return page.pairingBox()->isHidden(); }));
    CHECK_FALSE(page.pairDeviceButton()->isHidden());
    page.pairDeviceButton()->click();
    CHECK(pumpUntil([&] {
        return page.pairingCodeLabel()->text() == QStringLiteral("123 456") &&
               !page.pairingBox()->isHidden();
    }));
}

namespace {

// The next host_state() read waits, then answers with the state from before it waited, as a
// countdown read that started just before a switch was moved.
class HeldReadEngine : public fv::remote::testing::FakeRemoteEngine {
public:
    fv::remote::HostState host_state() override {
        fv::remote::HostState read = FakeRemoteEngine::host_state();
        std::unique_lock lock(held_);
        if (hold_) {
            hold_ = false;
            holding_ = true;
            changed_.notify_all();
            // Bounded, so a failed test still lets the pool finish.
            changed_.wait_for(lock, std::chrono::seconds(5), [this] { return released_; });
            holding_ = false;
            returned_ = true;
        }
        return read;
    }
    void holdNext() {
        std::lock_guard lock(held_);
        hold_ = true;
    }
    void release() {
        {
            std::lock_guard lock(held_);
            released_ = true;
        }
        changed_.notify_all();
    }
    bool holding() const {
        std::lock_guard lock(held_);
        return holding_;
    }
    bool returned() const {
        std::lock_guard lock(held_);
        return returned_;
    }
    ~HeldReadEngine() override { release(); }

private:
    mutable std::mutex held_;
    std::condition_variable changed_;
    bool hold_ = false;
    bool holding_ = false;
    bool released_ = false;
    bool returned_ = false;
};

} // namespace

TEST_CASE("a countdown read from before a switch moved does not move it back", "[remote_page]") {
    fvtest::BasicPageHarness<HeldReadEngine>::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = true;
        engine.state.running = true;
        engine.state.pairing = std::make_pair(std::string("054321"), 120.0);
    };
    fvtest::BasicPageHarness<HeldReadEngine> harness(options);
    REQUIRE(harness.show());
    fv::RemoteEnginePage& page = *harness.page;
    REQUIRE_FALSE(page.keepRecordsTile()->checkbox()->isChecked());
    // The next read is the countdown's (once a second while a code shows).
    harness.engine.holdNext();
    REQUIRE(pumpUntil([&] { return harness.engine.holding(); }, std::chrono::milliseconds(3000)));
    page.keepRecordsTile()->checkbox()->setChecked(true);
    REQUIRE(pumpUntil([&] { return harness.called("set_keep_records(true)"); }));
    REQUIRE(harness.settle());
    CHECK(page.keepRecordsTile()->checkbox()->isChecked());
    harness.engine.release();
    REQUIRE(pumpUntil([&] { return harness.engine.returned(); }));
    pumpFor(std::chrono::milliseconds(50));
    // A redraw from what the page knows keeps the switch where the person put it.
    page.refresh();
    CHECK(page.keepRecordsTile()->checkbox()->isChecked());
    CHECK(page.pairingCodeLabel()->text() == QStringLiteral("054 321"));
    REQUIRE(harness.settle());
    CHECK(page.keepRecordsTile()->checkbox()->isChecked());
}

TEST_CASE("an address the page can't read is said before pairing", "[remote_page]") {
    PageHarness harness;
    REQUIRE(harness.show());
    harness.page->manualButton()->click();
    harness.page->addressEdit()->setText(QStringLiteral("192.168.1.20:99999"));
    harness.page->codeEdit()->setText(QStringLiteral("123456"));
    harness.page->pairButton()->click();
    CHECK_FALSE(harness.page->findMessage()->text().isEmpty());
    CHECK_FALSE(harness.page->pairingBusy());
    harness.page->addressEdit()->setText(QStringLiteral("192.168.1.20"));
    harness.page->codeEdit()->setText(QStringLiteral("abc"));
    harness.page->pairButton()->click();
    CHECK(harness.page->findMessage()->text() ==
          QStringLiteral("Enter the pairing code shown on the host."));
    CHECK_FALSE(harness.page->pairingBusy());
    pumpFor(std::chrono::milliseconds(50));
    for (const std::string& call : harness.engine.calls()) {
        CHECK_FALSE(call.starts_with("pair("));
    }
}

TEST_CASE("removing a computer that keeps records asks what to do with them", "[remote_page]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        fv::remote::PairedDevice device;
        device.id = "laptop-id";
        device.name = "Laptop";
        engine.state.devices = {device};
        engine.summary = Json{{"dictation", {{"count", 2}, {"bytes", 2048}}}};
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    harness.removeAnswer = fv::RemoteEnginePrompts::RemoveChoice::DeleteRecords;
    QPushButton* remove = PageHarness::button(harness.page->devicesList(),
                                              QStringLiteral("remoteRemoveDeviceButton"));
    REQUIRE(remove != nullptr);
    remove->click();
    REQUIRE(harness.questions.size() == 1);
    CHECK(harness.questions[0] ==
          QStringLiteral("Remove paired computer: Laptop keeps 2 dictations (2.0 KB) here."));
    CHECK(pumpUntil([&] { return harness.called("remove_device(laptop-id,true)"); }));
}

TEST_CASE("Use for meetings asks for the meeting voice page", "[remote_page]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.pairing = fvtest::pairedWith();
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    QSignalSpy meetings(harness.page.get(), &fv::RemoteEnginePage::useForMeetingsRequested);
    QSignalSpy destinations(harness.page.get(),
                            &fv::RemoteEnginePage::settingsDestinationRequested);
    QSignalSpy models(harness.page.get(), &fv::RemoteEnginePage::manageHostModelsRequested);
    harness.page->meetingUseButton()->click();
    CHECK(meetings.count() == 1);
    REQUIRE(destinations.count() == 1);
    CHECK(destinations.at(0).at(0).toString() == QStringLiteral("meeting_voice"));
    harness.page->manageButton()->click();
    REQUIRE(models.count() == 1);
    CHECK(models.at(0).at(0).toString() == QStringLiteral("devbox"));
    // Without a controller to switch the engine, nothing is said.
    harness.page->useButton()->click();
    CHECK(harness.page->clientMessage()->text().isEmpty());
}

TEST_CASE("the feature registers the page under remote_engine", "[remote_page]") {
    fv::AppContext context(fv::AppServices{});
    fv::registerRemoteEnginePage(context);
    const fv::SettingsPageSpec* spec =
        context.settingsPages().page(fv::destinations::remote_engine);
    REQUIRE(spec != nullptr);
    CHECK(spec->title == QStringLiteral("Remote engine"));
    CHECK(spec->subtitle.startsWith(
        QStringLiteral("Transcribe dictation and meetings with another computer's engine")));
    CHECK(spec->railValue(Json::object()).isEmpty());
    // Nothing provides the remote engine: the built page is the unbound one.
    fv::SettingsPage container;
    QWidget* built = spec->create(&container);
    auto* page = qobject_cast<fv::RemoteEnginePage*>(built);
    REQUIRE(page != nullptr);
    CHECK(page->clientTile()->descriptionLabel()->text() ==
          QStringLiteral("The remote engine isn't available in this window."));
}

TEST_CASE("Retry reconnects remote dictation only when the user clicks it",
          "[remote_page][widget]") {
    fv::ManualExecutor notify;
    auto remote = std::make_shared<fv::speech::testing::FakeEngine>();
    auto script = remote->script();
    script.readiness = {fv::speech::ReadyState::Offline, {}, "Synthetic host is busy."};
    remote->set_script(script);
    fv::speech::SpeechService speech(remote->factory(), notify);
    speech.select(fv::speech::Slot::Dictation, {.backend = "remote"});
    REQUIRE(pumpUntil([&] {
        notify.run_all();
        return speech.readiness(fv::speech::Slot::Dictation).state ==
               fv::speech::ReadyState::Offline;
    }));

    int attempts = 0;
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.pairing = fvtest::pairedWith("127.0.0.1", "Synthetic host");
    };
    options.customize = [&](fv::RemoteEnginePageServices& services) {
        services.speech = &speech;
        services.usesRemoteDictation = [] {
            return true;
        };
        services.retryRemoteDictation = [&] {
            ++attempts;
            script.readiness = {fv::speech::ReadyState::Ready, "Synthetic host", {}};
            remote->set_script(script);
            speech.select(fv::speech::Slot::Dictation, {.backend = "remote"});
        };
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    auto* retry = harness.page->findChild<QPushButton*>(QStringLiteral("remoteRetryDictation"));
    REQUIRE(retry);
    REQUIRE_FALSE(retry->isHidden());
    CHECK(attempts == 0);
    retry->click();
    CHECK(attempts == 1);
    REQUIRE(pumpUntil([&] {
        notify.run_all();
        return retry->isHidden();
    }));
}

TEST_CASE("Sharing one finished meeting is a separate paired-host action",
          "[remote_page][widget]") {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.pairing = fvtest::pairedWith("127.0.0.1", "Synthetic host");
    };
    options.customize = [](fv::RemoteEnginePageServices& services) {
        services.canBrowseHeldMeetings = true;
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    QSignalSpy preview(harness.page.get(), &fv::RemoteEnginePage::heldMeetingsRequested);
    QSignalSpy send(harness.page.get(), &fv::RemoteEnginePage::shareMeetingRequested);
    auto* browse =
        harness.page->findChild<QPushButton*>(QStringLiteral("remoteBrowseHeldMeetings"));
    auto* share =
        harness.page->findChild<QPushButton*>(QStringLiteral("remoteShareFinishedMeeting"));
    REQUIRE(browse);
    REQUIRE(share);
    CHECK(send.isEmpty());
    browse->click();
    CHECK(preview.size() == 1);
    CHECK(send.isEmpty());
    share->click();
    REQUIRE(send.size() == 1);
    CHECK(send[0][0].toString() == QStringLiteral("Synthetic host"));
    CHECK_FALSE(send[0][1].toString().isEmpty());
}