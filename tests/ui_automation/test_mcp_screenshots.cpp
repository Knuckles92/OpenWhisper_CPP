// The API & MCP page's local part and its live section against the references OpenWhisper
// rendered for them (tools/parity/builders/a21.py, the M5.21 part): the widget tree first, then
// the pixels, with the application styled as OpenWhisper styles it.
//
// Each widget is shown at the builder's width and hidden again, as the builder does to learn its
// height; the reference's size then fixes the widget's. The listener is a FakeMcpControl on 8767
// with the builder's token, and the event bus holds the builder's two event-stream listeners and
// one channel session.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QTimer>

#include "agentapi/testing/fake_mcp_control.h"
#include "automation_support.h"
#include "mcp_support.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_automation/live_automation_section.h"
#include "ui_automation/mcp_page.h"
#include "ui_automation/mcp_widgets.h"
#include "ui_widgets/segmented_bar.h"

using fv::Json;
using namespace fvtest;

namespace {

constexpr int kPageWidth = 900;
const char* const kShotToken = "fv-test-token-0123456789abcdefghijklmnopqrstu";

// What the builder saved: the page's permissions, and the live section's two.
const Json kPageSaved{{"mcp_enabled", true},
                      {"mcp_retitle_transcriptions", true},
                      {"mcp_settings_access", true},
                      {"mcp_writable_settings", Json{{"auto_paste", true}}}};

// Keeps the widget's timers from firing between show() and the grab, as expect_matches does.
class HeldTimers : public QObject {
public:
    explicit HeldTimers(QWidget& root) {
        for (QTimer* timer : root.findChildren<QTimer*>()) {
            timer->installEventFilter(this);
        }
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override { return event->type() == QEvent::Timer; }
};

bool hasToken(const std::string& sheet) {
    return sheet.find('@') != std::string::npos || sheet.find("font-size") != std::string::npos;
}

// The classes OpenWhisper names with a leading underscore.
const std::map<std::string, std::string>& privateClasses() {
    static const std::map<std::string, std::string> names{
        {"McpActionButton", "_ActionButton"},   {"McpAdaptiveRow", "_AdaptiveRow"},
        {"McpDisclosure", "_Disclosure"},       {"McpCopyField", "_CopyField"},
        {"McpPermissionRow", "_PermissionRow"},
    };
    return names;
}

// The port's tree as the reference's schema has it, where the port differs by design (as the
// Hooks page's test normalizes it):
//  - a widget-local sheet with neither a token nor a font size (the status dot's) is not source
//    for OpenWhisper's style filter, so the reference has no sheet properties on it, while
//    setLocalStyleSheet always keeps the source;
//  - the geometry of a widget that is hidden in the reference is not compared;
//  - OpenWhisper's private classes (_ActionButton and the others) are named with an Mcp prefix
//    instead of the underscore here.
void normalize(Json& node, const Json& expected) {
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
    }
    const std::string cls = node.value("class", std::string());
    const auto renamed = privateClasses().find(cls);
    if (renamed != privateClasses().end() &&
        expected.value("class", std::string()) == renamed->second) {
        node["class"] = renamed->second;
    }
    auto& properties = node["properties"];
    if (properties.contains("unscaledStyleSheet") &&
        !hasToken(properties["unscaledStyleSheet"].get<std::string>())) {
        properties.erase("unscaledStyleSheet");
        properties.erase("scaledStyleSheet");
    }
    const auto& want = expected["children"];
    auto& have = node["children"];
    for (std::size_t i = 0; i < std::min(want.size(), have.size()); ++i) {
        normalize(have[i], want[i]);
    }
}

void expectMatches(QWidget& widget, const std::string& name, const std::string& variant) {
    const Json pins = fv::screenshot::load_reference(name, variant);
    widget.setFixedSize(pins.at("logical_size")[0].get<int>(),
                        pins.at("logical_size")[1].get<int>());
    HeldTimers held(widget);
    widget.show();
    QCoreApplication::processEvents();

    Json actual = fv::screenshot::dump_tree(widget);
    normalize(actual, pins.at("tree"));
    for (const std::string& difference : fv::screenshot::compare_trees(pins.at("tree"), actual)) {
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
    if (diff.differing > 0) {
        CHECK(diff.max_channel_delta <= tolerance.max_channel_delta);
    }
}

std::string variantName(const std::string& theme, int scale) {
    return theme + "-windows11-dpr1-s" + std::to_string(scale);
}

// _settled_height: shown at its width, events processed, hidden again; a grab lays out what the
// tree dump then reads, as render_reference.py's grab does.
void settle(QWidget& widget) {
    widget.resize(kPageWidth, 4000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
    (void)widget.grab();
    widget.hide();
}

// The listener and settings of a shot.
struct ShotRig {
    explicit ShotRig(const Json& saved, bool runningServer) : store(saved) {
        server.running_token = kShotToken;
        if (runningServer) {
            server.current.state = "running";
            server.current.message = "Ready for agent connections.";
        }
        sse1 = bus.listen("sse");
        sse2 = bus.listen("sse");
        session = bus.listen("claude-channel");
    }

    fv::MemorySettingsStore store;
    fv::agentapi::FakeMcpControl server;
    ScopedBus bus;
    std::shared_ptr<fv::automation::EventSubscription> sse1, sse2, session;
};

void pageShot(const std::string& theme, int scale, const std::string& name, bool runningServer) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    ShotRig rig(runningServer ? kPageSaved : Json::object(), runningServer);
    fv::McpSettingsPage page(rig.store, rig.server);
    if (runningServer) {
        page.setupKind()->setCurrentIndex(1);
        page.preview()->toggle()->setChecked(true);
    }
    settle(page);
    page.ensurePolished();
    expectMatches(page, name, variantName(theme, scale));
}

void sectionShot(const std::string& theme, int scale) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    Json saved = kPageSaved;
    saved["api_run_actions"] = true;
    saved["mcp_events_enabled"] = true;
    ShotRig rig(saved, true);
    fv::LiveServices services;
    services.subscriptionCount = [] {
        return std::size_t{1};
    };
    fv::LiveAutomationSection section(rig.store, rig.server, services);
    section.refreshLive();
    settle(section);
    section.ensurePolished();
    expectMatches(section, "live_automation_section/running", variantName(theme, scale));
}

} // namespace

TEST_CASE("reference screenshot settings_mcp_page stopped dark 100", "[screenshot]") {
    pageShot("dark", 100, "settings_mcp_page/stopped", false);
}

TEST_CASE("reference screenshot settings_mcp_page stopped light 100", "[screenshot]") {
    pageShot("light", 100, "settings_mcp_page/stopped", false);
}

TEST_CASE("reference screenshot settings_mcp_page stopped dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_mcp_page/stopped", false);
}

TEST_CASE("reference screenshot settings_mcp_page running dark 100", "[screenshot]") {
    pageShot("dark", 100, "settings_mcp_page/running", true);
}

TEST_CASE("reference screenshot settings_mcp_page running light 100", "[screenshot]") {
    pageShot("light", 100, "settings_mcp_page/running", true);
}

TEST_CASE("reference screenshot settings_mcp_page running dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_mcp_page/running", true);
}

TEST_CASE("reference screenshot live_automation_section running dark 100", "[screenshot]") {
    sectionShot("dark", 100);
}

TEST_CASE("reference screenshot live_automation_section running light 100", "[screenshot]") {
    sectionShot("light", 100);
}

TEST_CASE("reference screenshot live_automation_section running dark 130", "[screenshot]") {
    sectionShot("dark", 130);
}
