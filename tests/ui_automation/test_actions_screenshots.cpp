// The Actions page, the action editor and the Agent hotkey card against the references
// OpenWhisper rendered for them (tools/parity/builders/a21.py, the M5.13 part): the widget tree
// first, then the pixels, with the application styled as OpenWhisper styles it.
//
// The page body is built inside the Settings window over the builder's settings, laid out at the
// builder's width, then taken out of the window, as the builder takes OpenWhisper's out of its
// dialog; its class is compared on its own (OpenWhisper's is the private _SettingsPage). Route by
// voice's status, which the page works out off the GUI thread, has arrived first. The editor and
// the card are shown once at their width and hidden again, as the builder does to learn their
// height.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QLineEdit>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>

#include "actions_page_support.h"
#include "automation_support.h"
#include "base/testing/manual_executor.h"
#include "screenshot/screenshot.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"

using fv::Json;

namespace {

constexpr int kPageWidth = 900;
constexpr int kEditorWidth = 640;
constexpr int kAgentCardWidth = 640;
constexpr int kMeasureHeight = 600;

const std::map<std::string, bool> kScan{
    {"claude_code", true}, {"codex", true}, {"opencode_cli", false}};
const std::map<std::string, bool> kNoneFound{
    {"claude_code", false}, {"codex", false}, {"opencode_cli", false}};

fv::automation::Action agentAction() {
    fv::automation::Action action;
    action.id = "agent";
    action.name = "Agent hotkey";
    action.kind = "agent";
    action.params = Json{{"agent", "claude_code"}, {"mode", "handoff"}, {"surface", "auto"}};
    action.hotkey = "ctrl+alt+shift+space";
    action.spoken_names = {"Claude Code"};
    action.description = "Coding tasks for Claude Code";
    return action;
}

// The builder's SETTINGS and ROUTE_SETTINGS, with the agent hotkey or not.
Json builderSettings(bool withAgent) {
    Json actions = Json::array();
    if (withAgent) {
        actions.push_back(agentAction().to_dict());
    }
    for (const fv::automation::Action& action : fvtest::fixedActions()) {
        actions.push_back(action.to_dict());
    }
    return Json{
        {"transcript_cleanup_profiles",
         Json::array({Json{
             {"id", "email"}, {"name", "Email"}, {"instructions", "Write it as a short email."}}})},
        {"text_transforms",
         Json::array(
             {Json{{"id", "formal"}, {"name", "Formal"}, {"instruction", "Make it formal."}}})},
        {"voice_routing_enabled", true},
        {"typesafe_enabled", true},
        {"hotkeys", Json{{"route_record", "ctrl+alt+v"}}},
        {"actions", std::move(actions)},
    };
}

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

// The port's tree as the reference's schema has it, where the port differs by design (as the
// M5.10 widgets' test normalizes it):
//  - a widget-local sheet with neither a token nor a font size (the status dot's) is not source
//    for OpenWhisper's style filter, so the reference has no sheet properties on it, while
//    setLocalStyleSheet always keeps the source;
//  - the geometry of a widget that is hidden in the reference is not compared: OpenWhisper's
//    segmented bars lay out their choices once more while hidden, which draws nothing;
//  - the page body's class is OpenWhisper's private _SettingsPage.
void normalize(Json& node, const Json& expected) {
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
    }
    if (expected.value("class", std::string()) == "_SettingsPage" &&
        node.value("class", std::string()) == "ActionsPage") {
        node["class"] = "_SettingsPage";
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

// ---- the page ----

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

void pageShot(const std::string& theme, int scale, const char* name, bool withAgent,
              const std::map<std::string, bool>& scan) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    fvtest::ActionsRig rig(builderSettings(withAgent));
    rig.setFound(scan);
    fv::SettingsDialog& dialog = rig.dialog(false);
    fv::SettingsPage* page = dialog.ensurePage(fvtest::dest::actions);
    REQUIRE(page != nullptr);
    fv::ActionsPage& actions = rig.page();
    REQUIRE(fvtest::waitFor(
        [&] { return actions.routingStatusLine()->state() == QStringLiteral("ready"); }));
    QScrollArea* scroll = dialog.pageScroll(fvtest::dest::actions);
    REQUIRE(scroll != nullptr);
    std::unique_ptr<QWidget> holder(scroll->takeWidget());
    REQUIRE(holder.get() == page);
    holder->setParent(nullptr);
    QCoreApplication::processEvents();
    const QSize old = page->size();
    page->resize(kPageWidth, 4000);
    QResizeEvent resized(page->size(), old);
    QCoreApplication::sendEvent(page, &resized);
    std::unique_ptr<QWidget> body(dialog.pageContent(fvtest::dest::actions));
    REQUIRE(body != nullptr);
    body->setParent(nullptr);
    settle(*body);
    // The status that a show asks for again has come back.
    REQUIRE(fvtest::waitFor(
        [&] { return actions.routingStatusLine()->state() == QStringLiteral("ready"); }));
    body->ensurePolished();
    expectMatches(*body, name, variantName(theme, scale));
}

// ---- the editor and the card ----

// The editor's and the card's surroundings: the builder's settings, a relay whose work never
// runs, the usual scan.
struct Standalone {
    explicit Standalone(bool withAgent) : store(builderSettings(withAgent)), relay(io, {}) {
        agents = std::make_shared<fv::agents::FakeInstalledAgents>();
        for (const auto& [id, found] : kScan) {
            if (found) {
                fv::agents::InstalledAgent agent;
                agent.id = id;
                agent.path = std::filesystem::path("C:/fake") / id;
                agent.version = "1.0.0";
                agents->set_agent(id, agent);
            } else {
                agents->set_agent(id, std::nullopt);
            }
        }
        std::thread scan([this] { agents->scan_agents(true); });
        scan.join();
        fv::agents::set_installed_agents(agents);
    }
    ~Standalone() { fv::agents::set_installed_agents(nullptr); }
    Standalone(const Standalone&) = delete;
    Standalone& operator=(const Standalone&) = delete;

    fv::MemorySettingsStore store;
    fv::ManualExecutor io;
    fv::ActionRelay relay;
    std::shared_ptr<fv::agents::FakeInstalledAgents> agents;
};

void noTry(const fv::automation::Action&, const QString&,
           std::function<void(fv::automation::ActionResult)>) {}

// _fit: shown once at its width to learn its height, then hidden.
void fit(QWidget& widget, int width) {
    widget.resize(width, kMeasureHeight);
    widget.show();
    QApplication::processEvents();
    widget.hide();
}

void webhookNew(const std::string& theme, int scale) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    Standalone around(false);
    fv::ActionEditor editor(around.store, around.relay, noTry);
    fv::automation::Preset preset;
    for (const fv::automation::Preset& entry : fv::automation::PRESETS()) {
        if (entry.id == "webhook") {
            preset = entry;
        }
    }
    fv::automation::Action action;
    action.id = "new-webhook";
    action.name = preset.name;
    action.kind = preset.kind;
    action.params = preset.params;
    action.shaping = preset.shaping;
    editor.open(action, true, fvtest::qs(preset.note));
    editor.urlEdit()->setText(QStringLiteral("http://example.com/hook"));
    editor.urlEdit()->setCursorPosition(0);
    fit(editor, kEditorWidth);
    expectMatches(editor, "action_editor/webhook-new", variantName(theme, scale));
}

void linkPreview(const std::string& theme, int scale) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    Standalone around(false);
    fv::ActionEditor editor(around.store, around.relay, noTry);
    editor.open(fvtest::askClaude(), false);
    fit(editor, kEditorWidth);
    expectMatches(editor, "action_editor/link-preview", variantName(theme, scale));
}

void agentCard(const std::string& theme) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), 100);
    Standalone around(true);
    fv::AgentHotkeyCard card(around.store, around.relay, noTry);
    card.load(*around.store.snapshot());
    fit(card, kAgentCardWidth);
    expectMatches(card, "action_agent_card/configured", variantName(theme, 100));
}

} // namespace

TEST_CASE("reference screenshot settings_actions_page invite dark 100", "[screenshot]") {
    pageShot("dark", 100, "settings_actions_page/invite", false, kNoneFound);
}

TEST_CASE("reference screenshot settings_actions_page invite light 100", "[screenshot]") {
    pageShot("light", 100, "settings_actions_page/invite", false, kNoneFound);
}

TEST_CASE("reference screenshot settings_actions_page invite dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_actions_page/invite", false, kNoneFound);
}

TEST_CASE("reference screenshot settings_actions_page configured dark 100", "[screenshot]") {
    pageShot("dark", 100, "settings_actions_page/configured", true, kScan);
}

TEST_CASE("reference screenshot settings_actions_page configured light 100", "[screenshot]") {
    pageShot("light", 100, "settings_actions_page/configured", true, kScan);
}

TEST_CASE("reference screenshot settings_actions_page configured dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_actions_page/configured", true, kScan);
}

TEST_CASE("reference screenshot action_editor webhook-new dark 100", "[screenshot]") {
    webhookNew("dark", 100);
}

TEST_CASE("reference screenshot action_editor webhook-new light 100", "[screenshot]") {
    webhookNew("light", 100);
}

TEST_CASE("reference screenshot action_editor webhook-new dark 130", "[screenshot]") {
    webhookNew("dark", 130);
}

TEST_CASE("reference screenshot action_editor link-preview dark 100", "[screenshot]") {
    linkPreview("dark", 100);
}

TEST_CASE("reference screenshot action_editor link-preview light 100", "[screenshot]") {
    linkPreview("light", 100);
}

TEST_CASE("reference screenshot action_editor link-preview dark 130", "[screenshot]") {
    linkPreview("dark", 130);
}

TEST_CASE("reference screenshot action_agent_card configured dark 100", "[screenshot]") {
    agentCard("dark");
}

TEST_CASE("reference screenshot action_agent_card configured light 100", "[screenshot]") {
    agentCard("light");
}
