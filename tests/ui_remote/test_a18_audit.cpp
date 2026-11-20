// The a18 behaviour audit's checks of Settings → Remote engine
// (docs/port/map/a18-remote-engine-ui.md): the verbatim wording and control properties the page
// tests did not pin yet.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QPushButton>

#include "base/identity.h"
#include "remote/errors.h"
#include "remote/protocol.h"
#include "ui_remote/remote_engine_page.h"
#include "ui_remote_support.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using fv::remote::NearbyHost;
using fvtest::PageHarness;
using fvtest::pumpUntil;

namespace {

constexpr auto kTags = "[remote_page][a18_audit]";

const std::string kFingerprint = std::string(32, 'A') + std::string(32, 'B');

QString product() {
    return QString::fromStdString(std::string(fv::identity().product));
}

NearbyHost lanHost(std::string name, bool approval, std::string address) {
    NearbyHost host;
    host.host_name = name;
    host.fingerprint = kFingerprint;
    host.engine = Json{{"label", "Whisper"}, {"available", true}, {"device", "cpu"}};
    host.compatible = true;
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

fv::net::TailscaleStatus tailnet(std::string owner) {
    fv::net::TailscaleStatus status;
    status.state = "running";
    status.name = "laptop";
    status.address = "100.64.0.1";
    status.owner = std::move(owner);
    return status;
}

} // namespace

// A18-09: the firewall sentence, whole.
TEST_CASE("a18 audit: a found host that won't connect blames its firewall in full", kTags) {
    const QString message = fv::RemoteEnginePage::pairErrorText(
        std::make_exception_ptr(fv::remote::RemoteUnreachable("Couldn't reach 192.168.1.9:47821.")),
        lanHost("jed", true, "192.168.1.9"));
    CHECK(message == QStringLiteral("jed answered the search but didn't accept the connection. Its "
                                    "firewall may be blocking %1: on jed, open Settings → Remote "
                                    "engine → Share this computer to see what to change.")
                         .arg(product()));
}

// A18-10: "Use a code" says where the code is, whole.
TEST_CASE("a18 audit: a host too old to ask is paired by a code it shows", kTags) {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.scan.status = fv::net::TailscaleStatus{.state = "not_installed"};
        engine.scan.hosts = {lanHost("oldbox", false, "127.0.0.1")};
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    QPushButton* useCode =
        PageHarness::button(harness.page->findList(), QStringLiteral("Use a code"));
    REQUIRE(useCode != nullptr);
    useCode->click();
    CHECK(
        harness.page->findMessage()->text() ==
        QStringLiteral("On oldbox, click \"Show a pairing code\" under Settings → Remote engine → "
                       "Share this computer (\"Pair a device\" in older versions), then enter the "
                       "code here and click Pair."));
}

// A18-47, A18-28, A18-26: the host tiles' descriptions and the port field.
TEST_CASE("a18 audit: the host tiles say what they allow, word for word", kTags) {
    PageHarness harness;
    REQUIRE(harness.show());
    auto& page = *harness.page;
    CHECK(page.shareTile()->titleLabel()->text() == QStringLiteral("Share this computer's engine"));
    CHECK(page.shareTile()->descriptionLabel()->text() ==
          QStringLiteral("Paired computers can dictate or transcribe meetings with the engine "
                         "selected here, and switch it to any model downloaded here. The "
                         "connection is encrypted and only computers you pair can use it. Windows "
                         "may ask once to allow private networks."));
    CHECK(page.managementTile()->descriptionLabel()->text() ==
          QStringLiteral("Paired computers can browse the model catalog and download models and "
                         "verified speech runtimes here, using this computer's network and "
                         "storage. Its Hugging Face download policy still applies. Turning this "
                         "off blocks new requests; downloads already started continue. Choosing "
                         "an already-downloaded model never needs it."));
    CHECK(page.keepRecordsTile()->titleLabel()->text() ==
          QStringLiteral("Keep records for paired computers"));
    CHECK(page.keepRecordsTile()->descriptionLabel()->text() ==
          QStringLiteral("Paired computers can keep their dictation history, recordings and "
                         "meetings here. They show in History and Past Meetings, marked with the "
                         "computer they came from, and each computer can see and delete only its "
                         "own. Turning this off stops new ones; what's kept stays until that "
                         "computer brings it back or you remove it below."));
    CHECK(page.devicesTile()->descriptionLabel()->text() ==
          QStringLiteral("Computers that can use this engine. One that finds this computer can "
                         "ask to pair, and you're asked here to allow it. Removing one cuts it off "
                         "at once."));
    CHECK(page.portTile()->descriptionLabel()->text() ==
          QStringLiteral("Clients connect to this port. 47821 unless something else already "
                         "uses it."));
    auto* port = page.portSpin();
    CHECK(port->minimum() == 1024);
    CHECK(port->maximum() == 65535);
    CHECK(port->value() == 47821);
    CHECK_FALSE(port->keyboardTracking());
    CHECK(port->minimumHeight() >= 40);
    CHECK(port->minimumWidth() >= 120);
}

// A18-22: the identity line, only while sharing.
TEST_CASE("a18 audit: the sharing host shows its identity to compare", kTags) {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = true;
        engine.state.running = true;
        engine.state.host_name = "devbox";
        engine.state.engine = fvtest::fakeEngine();
        engine.state.fingerprint = kFingerprint;
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    auto* identity = harness.page->hostIdentity();
    REQUIRE(pumpUntil([&] { return identity->isVisibleTo(harness.page.get()); }));
    CHECK(identity->text() ==
          QStringLiteral("This computer's identity: %1. A computer that pairs should show the "
                         "same.")
              .arg(QString::fromStdString(fv::remote::short_fingerprint(kFingerprint))));
    CHECK(identity->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
    harness.page->shareTile()->checkbox()->setChecked(false);
    CHECK(pumpUntil([&] { return !identity->isVisibleTo(harness.page.get()); }));
}

// A18-25: the code-free trust tile names the owner.
TEST_CASE("a18 audit: the Tailscale trust tile names whose computers pair without a code", kTags) {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = true;
        engine.state.running = true;
        engine.state.host_name = "laptop";
        engine.state.engine = fvtest::fakeEngine();
        engine.state.tailscale = tailnet("owner@example.com");
        engine.state.tailscale_trust = true;
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    auto* tile = harness.page->tailscaleTile();
    REQUIRE(pumpUntil([&] { return tile->isVisibleTo(harness.page.get()); }));
    CHECK(tile->descriptionLabel()->text() ==
          QStringLiteral("Computers signed in to Tailscale as owner@example.com can pair by "
                         "picking this one from their tailnet. Anyone else on your tailnet still "
                         "asks first, and you allow it here."));
}

// A18-46: the unbound page disables every tile and hides the pairing box.
TEST_CASE("a18 audit: the unbound page offers nothing to click", kTags) {
    PageHarness::Options options;
    options.bound = false;
    PageHarness harness(options);
    harness.page->show();
    auto& page = *harness.page;
    CHECK(page.pairingBox()->isHidden());
    for (QWidget* tile :
         {static_cast<QWidget*>(page.findTile()), static_cast<QWidget*>(page.managementTile()),
          static_cast<QWidget*>(page.keepRecordsTile()),
          static_cast<QWidget*>(page.manageMcpTile()), static_cast<QWidget*>(page.tailscaleTile()),
          static_cast<QWidget*>(page.portTile()), static_cast<QWidget*>(page.devicesTile())}) {
        CHECK_FALSE(tile->isEnabled());
    }
}

// A18-25: a tagged server has no owner, so nothing pairs without a code.
TEST_CASE("a18 audit: without a Tailscale owner there is no code-free tile", kTags) {
    PageHarness::Options options;
    options.script = [](auto& engine, auto&) {
        engine.state.enabled = true;
        engine.state.running = true;
        engine.state.host_name = "laptop";
        engine.state.engine = fvtest::fakeEngine();
        engine.state.tailscale = tailnet("");
    };
    PageHarness harness(options);
    REQUIRE(harness.show());
    REQUIRE(pumpUntil([&] { return harness.page->hostStatus()->isVisibleTo(harness.page.get()); }));
    harness.settle();
    CHECK_FALSE(harness.page->tailscaleTile()->isVisibleTo(harness.page.get()));
}
