// Settings → Remote engine, the rename dialog and the record recovery dialog against the
// references OpenWhisper rendered for them (tools/parity/builders/a18.py,
// tests/reference/<widget>/<theme>-windows11-dpr1-s<scale>): the widget tree first, then the
// pixels, with the application styled as OpenWhisper styles it and the same fake data.
//
// The trees are compared without class names, then the classes on their own: the page body is
// OpenWhisper's private _SettingsPage (RemoteEnginePage here) and each tab's section its private
// _TileSection (a plain QWidget here).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEvent>
#include <QImage>
#include <QLayout>

#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_remote/record_recovery_dialog.h"
#include "ui_remote/remote_name_dialog.h"
#include "ui_remote_support.h"

using fv::Json;
using fvtest::AppTheme;
using fvtest::PageHarness;
using fvtest::pumpUntil;

namespace {

const std::string kFingerprint = [] {
    std::string text;
    for (int i = 0; i < 16; ++i) {
        text += "ABCD";
    }
    return text;
}();

void checkClasses(const Json& expected, const Json& actual, const std::string& path) {
    std::string wanted = expected.value("class", std::string());
    if (wanted == "_SettingsPage") {
        wanted = "RemoteEnginePage";
    } else if (wanted == "_TileSection") {
        wanted = "QWidget";
    } else if (wanted == "QDialog" && path.find('/') == std::string::npos) {
        // OpenWhisper builds both dialogs from a plain QDialog in a function.
        wanted = actual.value("class", std::string());
    }
    INFO(path);
    CHECK(actual.value("class", std::string()) == wanted);
    const Json empty = Json::array();
    const Json& want = expected.contains("children") ? expected["children"] : empty;
    const Json& have = actual.contains("children") ? actual["children"] : empty;
    REQUIRE(want.size() == have.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        const std::string name = want[i].value("objectName", std::string());
        checkClasses(want[i], have[i],
                     path + "/" + (name.empty() ? wanted + "[" + std::to_string(i) + "]" : name));
    }
}

void check(QWidget& widget, const std::string& name, const std::string& variant) {
    widget.ensurePolished();
    checkClasses(fv::screenshot::load_reference(name, variant).at("tree"),
                 fv::screenshot::dump_tree(widget), name);
    fv::screenshot::Options options;
    options.ignore_keys = {"class"};
    fv::screenshot::expect_matches(widget, name, variant, options);
}

// The geometry of a widget hidden in the reference is not compared: a hidden widget keeps
// whatever size its last layout pass (or none) gave it, which nothing shows.
void normalize(Json& node, const Json& expected) {
    if (!expected.value("visible", true)) {
        node["geometry"] = expected["geometry"];
    }
    const Json empty = Json::array();
    const Json& want = expected.contains("children") ? expected["children"] : empty;
    if (!node.contains("children")) {
        return;
    }
    Json& have = node["children"];
    for (std::size_t i = 0; i < std::min(want.size(), have.size()); ++i) {
        normalize(have[i], want[i]);
    }
}

// The page as render_reference.py takes it: sized, shown, one round of events, grabbed, and the
// tree dumped after the grab; then the tree (hidden geometry aside) and the pixels.
void checkPage(QWidget& widget, const std::string& name, const std::string& variant) {
    const Json pins = fv::screenshot::load_reference(name, variant);
    widget.resize(pins.at("logical_size")[0].get<int>(), pins.at("logical_size")[1].get<int>());
    widget.show();
    QCoreApplication::processEvents();
    (void)widget.grab();
    Json actual = fv::screenshot::dump_tree(widget);
    normalize(actual, pins.at("tree"));
    checkClasses(pins.at("tree"), actual, name);
    for (const std::string& difference :
         fv::screenshot::compare_trees(pins.at("tree"), actual, {"class"})) {
        FAIL_CHECK(name << "/" << variant << " tree: " << difference);
    }
    const QByteArray sheet =
        QCryptographicHash::hash(qApp->styleSheet().toUtf8(), QCryptographicHash::Sha1).toHex();
    CHECK(sheet.toStdString() == pins.value("stylesheet_sha1", std::string()));
    if (!fv::screenshot::pixel_comparison_enabled()) {
        SKIP(name << "/" << variant << ": pixels are not compared; the tree was");
    }
    std::string whyNot;
    if (!fv::screenshot::qt_kit_matches_reference(&whyNot)) {
        SKIP(name << "/" << variant << ": " << whyNot);
    }
    const QImage reference(QString::fromStdString(
        (fv::testkit::reference_path() / name / (variant + ".png")).string()));
    REQUIRE_FALSE(reference.isNull());
    const QImage grab = widget.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const fv::screenshot::PixelDiff diff = fv::screenshot::compare_images(reference, grab);
    if (!diff.same_size || diff.differing > 0) {
        const auto artifacts = fv::testkit::artifact_path() / "screenshots" / name;
        std::filesystem::create_directories(artifacts);
        grab.save(QString::fromStdString((artifacts / (variant + "-actual.png")).string()));
        if (diff.same_size) {
            diff.diff.save(QString::fromStdString((artifacts / (variant + "-diff.png")).string()));
        }
    }
    REQUIRE(diff.same_size);
    const fv::screenshot::Tolerance tolerance = fv::screenshot::load_tolerance(name, variant);
    INFO(diff.differing << " pixels differ, max channel delta " << diff.max_channel_delta
                        << "; tolerance " << tolerance.max_differing_pixels);
    CHECK(diff.differing <= tolerance.max_differing_pixels);
}

std::string variantName(const std::string& theme, int scale) {
    return theme + "-windows11-dpr1-s" + std::to_string(scale);
}

void flush() {
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

// As the builder measures the page: shown at 900 px, events processed, hidden again.
void settle(QWidget& page) {
    page.resize(900, 4000);
    page.show();
    flush();
    (void)page.layout()->minimumSize();
    page.hide();
    flush();
}

fv::net::TailscaleStatus tailnet(std::string name, std::string address) {
    fv::net::TailscaleStatus status;
    status.state = "running";
    status.name = name;
    status.dns_name = name + ".tail.ts.net";
    status.address = std::move(address);
    status.owner = "owner@example.com";
    status.tailnet = "example.com";
    return status;
}

Json servedEngine() {
    return Json{{"family", "parakeet"}, {"model", "parakeet-v3"}, {"label", "Parakeet TDT 0.6B v3"},
                {"device", "cuda"},     {"streaming", false},     {"available", true},
                {"status", ""}};
}

// devbox on this network and the tailnet, and an older studio box on this network.
fv::remote::NearbyScan found() {
    const Json engine{{"label", "Parakeet TDT 0.6B v3"},
                      {"device", "cuda"},
                      {"available", true},
                      {"family", "parakeet"}};
    fv::remote::NearbyHost devbox;
    devbox.host_name = "devbox";
    devbox.fingerprint = kFingerprint;
    devbox.engine = engine;
    devbox.compatible = true;
    devbox.approval = true;
    devbox.lan =
        fv::remote::LanHost{"192.168.1.20", 47821, "devbox", kFingerprint, engine, 1, true};
    fv::remote::TailnetHost peer;
    peer.peer = {"devbox", "devbox.tail.ts.net", "100.64.0.2", "windows",
                 true,     "owner@example.com",  false,        true};
    peer.port = 47821;
    peer.host_name = "devbox";
    peer.engine = engine;
    peer.tailscale_pairing = true;
    peer.compatible = true;
    peer.fingerprint = kFingerprint;
    peer.approval = true;
    devbox.tailnet = peer;
    const Json old{
        {"label", "Whisper turbo"}, {"device", "cpu"}, {"available", true}, {"family", "whisper"}};
    std::string studioPrint;
    for (int i = 0; i < 16; ++i) {
        studioPrint += "EF01";
    }
    fv::remote::NearbyHost studio;
    studio.host_name = "studio";
    studio.fingerprint = studioPrint;
    studio.engine = old;
    studio.compatible = true;
    studio.approval = false;
    studio.lan = fv::remote::LanHost{"192.168.1.31", 47821, "studio", studioPrint, old, 1, false};
    fv::remote::NearbyScan scan;
    scan.status = tailnet("laptop", "100.64.0.1");
    scan.hosts = {devbox, studio};
    return scan;
}

enum class Scene { ClientUnpaired, ClientPaired, HostSharing, Unbound };

// What each paired computer keeps here, by device.
class SummaryEngine : public fv::remote::testing::FakeRemoteEngine {
public:
    Json records_summary(std::string_view device_id) override {
        FakeRemoteEngine::records_summary(device_id);
        const auto found = summaries.find(std::string(device_id));
        return found == summaries.end() ? Json::object() : found->second;
    }
    std::map<std::string, Json> summaries;
};

using SceneHarness = fvtest::BasicPageHarness<SummaryEngine>;

SceneHarness::Options sceneOptions(Scene scene) {
    SceneHarness::Options options;
    options.bound = scene != Scene::Unbound;
    options.script = [scene](auto& engine, auto& records) {
        engine.state.host_name = "devbox";
        switch (scene) {
        case Scene::ClientUnpaired:
            engine.tailscale_value = tailnet("laptop", "100.64.0.1");
            engine.scan = found();
            break;
        case Scene::ClientPaired: {
            fv::remote::ClientPairing pairing;
            pairing.host = "192.168.1.20";
            pairing.port = 47821;
            pairing.fingerprint = kFingerprint;
            pairing.host_name = "devbox";
            pairing.device_id = "laptop-device";
            pairing.paired_at = "2026-11-12T18:00:00+00:00";
            pairing.via = "code";
            engine.pairing = pairing;
            records.status_value.location = "both";
            records.status_value.paired = true;
            records.status_value.host_name = "devbox";
            records.status_value.host_keeps = true;
            records.status_value.host_supports = true;
            records.status_value.stored = {{"dictation", {12, 1288490188}}, {"meeting", {3, 0}}};
            break;
        }
        case Scene::HostSharing: {
            auto& state = engine.state;
            state.enabled = true;
            state.running = true;
            state.address = "192.168.1.20";
            state.address_kind = "lan";
            state.fingerprint = kFingerprint;
            state.pairing = std::make_pair(std::string("482913"), 54.0);
            state.clients = {{"laptop-device", "laptop", "192.168.1.31", false, 0.0, false}};
            fv::remote::PairedDevice laptop;
            laptop.id = "laptop-device";
            laptop.name = "laptop";
            laptop.paired_at = "2026-11-12T18:00:00+00:00";
            laptop.last_seen = "2026-11-14T09:58:00+00:00";
            laptop.via = "approval";
            fv::remote::PairedDevice studio;
            studio.id = "studio-device";
            studio.name = "Studio PC";
            studio.paired_at = "2026-11-13T17:30:00+00:00";
            studio.last_seen = "2026-11-13T20:00:00+00:00";
            studio.via = "tailscale";
            studio.paired_name = "DESKTOP-4K2J9";
            state.devices = {laptop, studio};
            state.engine = servedEngine();
            state.tailscale = tailnet("devbox", "100.64.0.2");
            state.keep_records = true;
            engine.summaries["laptop-device"] =
                Json{{"dictation", {{"count", 4}, {"bytes", 2048}}}};
            state.notes = {{"public_network",
                            "Windows treats \"Home\" as a public network, so computers on it may "
                            "not reach this one.",
                            "Open network settings", "ms-settings:network-status"}};
            break;
        }
        case Scene::Unbound:
            break;
        }
    };
    return options;
}

void page(Scene scene, const std::string& name, const std::string& theme, int scale) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    SceneHarness harness(sceneOptions(scene));
    if (scene != Scene::Unbound) {
        REQUIRE(harness.settle());
    }
    if (scene == Scene::ClientUnpaired) {
        harness.page->scanNearby(true);
        REQUIRE(pumpUntil([&] { return harness.page->hasScan() && !harness.page->scanBusy(); }));
        REQUIRE(harness.settle());
    }
    settle(*harness.page);
    checkPage(*harness.page, "remote_engine_page/" + name, variantName(theme, scale));
}

} // namespace

TEST_CASE("reference screenshot remote_engine_page client-unpaired dark", "[screenshot]") {
    page(Scene::ClientUnpaired, "client-unpaired", "dark", 100);
}
TEST_CASE("reference screenshot remote_engine_page client-unpaired light", "[screenshot]") {
    page(Scene::ClientUnpaired, "client-unpaired", "light", 100);
}
TEST_CASE("reference screenshot remote_engine_page client-unpaired dark 130", "[screenshot]") {
    page(Scene::ClientUnpaired, "client-unpaired", "dark", 130);
}
TEST_CASE("reference screenshot remote_engine_page client-paired dark", "[screenshot]") {
    page(Scene::ClientPaired, "client-paired", "dark", 100);
}
TEST_CASE("reference screenshot remote_engine_page client-paired light", "[screenshot]") {
    page(Scene::ClientPaired, "client-paired", "light", 100);
}
TEST_CASE("reference screenshot remote_engine_page client-paired dark 130", "[screenshot]") {
    page(Scene::ClientPaired, "client-paired", "dark", 130);
}
TEST_CASE("reference screenshot remote_engine_page host-sharing dark", "[screenshot]") {
    page(Scene::HostSharing, "host-sharing", "dark", 100);
}
TEST_CASE("reference screenshot remote_engine_page host-sharing light", "[screenshot]") {
    page(Scene::HostSharing, "host-sharing", "light", 100);
}
TEST_CASE("reference screenshot remote_engine_page host-sharing dark 130", "[screenshot]") {
    page(Scene::HostSharing, "host-sharing", "dark", 130);
}
TEST_CASE("reference screenshot remote_engine_page unbound dark", "[screenshot]") {
    page(Scene::Unbound, "unbound", "dark", 100);
}
TEST_CASE("reference screenshot remote_engine_page unbound light", "[screenshot]") {
    page(Scene::Unbound, "unbound", "light", 100);
}
TEST_CASE("reference screenshot remote_engine_page unbound dark 130", "[screenshot]") {
    page(Scene::Unbound, "unbound", "dark", 130);
}

namespace {

void nameDialog(const std::string& theme) {
    AppTheme appTheme(QString::fromStdString(theme), 100);
    fv::RemoteNameDialog dialog(
        QStringLiteral("Rename host"),
        QStringLiteral("The name this computer shows for its host. Leave it empty to go back to "
                       "DESKTOP-4K2J9, the name it gave when pairing."),
        QStringLiteral("devbox"), QStringLiteral("DESKTOP-4K2J9"));
    dialog.adjustSize();
    check(dialog, "remote_name_dialog", variantName(theme, 100));
}

void recoveryDialog(const std::string& theme) {
    AppTheme appTheme(QString::fromStdString(theme), 100);
    fv::remote::PairedDevice studio;
    studio.id = "9b1e44c07a2f4d55";
    studio.name = "Studio PC";
    fv::RecordRecoveryDialog dialog(
        {{"0f3c9a7e21d84b6c", "laptop", {{"dictation", 12}, {"meeting", 3}}}}, {studio});
    check(dialog, "record_recovery_dialog", variantName(theme, 100));
}

} // namespace

TEST_CASE("reference screenshot remote_name_dialog dark", "[screenshot]") {
    nameDialog("dark");
}
TEST_CASE("reference screenshot remote_name_dialog light", "[screenshot]") {
    nameDialog("light");
}
TEST_CASE("reference screenshot record_recovery_dialog dark", "[screenshot]") {
    recoveryDialog("dark");
}
TEST_CASE("reference screenshot record_recovery_dialog light", "[screenshot]") {
    recoveryDialog("light");
}
