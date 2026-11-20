// The Hooks page and two of its editors against the references OpenWhisper rendered for them
// (tools/parity/builders/a21.py, the M5.17 part): the widget tree first, then the pixels, with the
// application styled as OpenWhisper styles it.
//
// The page body is built inside the Settings window over the builder's settings, laid out at the
// builder's width, then taken out of the window, as the builder takes OpenWhisper's out of its
// dialog; its class is compared on its own (OpenWhisper's is the private _SettingsPage). The
// editors are shown once at their width and hidden again, as the builder does to learn their
// height. The clock is pinned to 2026-03-15 15:00 UTC, zone UTC.
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
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>

#include "automation_support.h"
#include "hooks_support.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"

using fv::Json;
using namespace fvtest;

namespace {

constexpr int kPageWidth = 900;
constexpr int kEditorWidth = 640;
constexpr int kMeasureHeight = 600;

const Json kWebhook{{"id", "hook01"},
                    {"name", "Home automation"},
                    {"url", "https://example.com:8443/hooks"},
                    {"events", Json::array({"transcription.completed", "action.ran"})},
                    {"enabled", true}};
const Json kProgram{{"id", "prog01"},
                    {"name", "Log my dictations"},
                    {"program", "C:\\Tools\\log.exe"},
                    {"args", Json::array()},
                    {"events", Json::array({"transcription.completed"})},
                    {"enabled", false},
                    {"timeout_s", 30}};
const Json kFilterRow{{"id", "filt01"},
                      {"name", "Fix product names"},
                      {"kind", "http"},
                      {"stage", "before_cleanup"},
                      {"url", "http://127.0.0.1:8080/filter"},
                      {"timeout_ms", 1500},
                      {"enabled", true}};

// The two deliveries of the builder, newest first.
std::vector<fv::automation::DeliveryRecord> deliveries() {
    return {
        {"evt_2", "transcription.completed", "2026-03-15T14:58:20.000Z", true, 200, 120, "", 1},
        {"evt_1", "action.ran", "2026-03-15T14:41:02.000Z", false, 502, 1430, "Bad gateway", 2}};
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

// The classes OpenWhisper names with a leading underscore.
const std::map<std::string, std::string>& privateClasses() {
    static const std::map<std::string, std::string> names{
        {"HookRow", "_HookRow"},
        {"AdaptiveButtons", "_AdaptiveButtons"},
        {"LeaseList", "_LeaseList"},
        {"HooksPage", "_SettingsPage"},
    };
    return names;
}

// The port's tree as the reference's schema has it, where the port differs by design (as the
// Actions page's test normalizes it):
//  - a widget-local sheet with neither a token nor a font size (the status dot's) is not source
//    for OpenWhisper's style filter, so the reference has no sheet properties on it, while
//    setLocalStyleSheet always keeps the source;
//  - the geometry of a widget that is hidden in the reference is not compared: OpenWhisper's
//    segmented bars lay out their choices once more while hidden, which draws nothing;
//  - OpenWhisper's private classes (_HookRow, _AdaptiveButtons, _LeaseList, the page body's
//    _SettingsPage) are named without the underscore here.
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

void pageShot(const std::string& theme, int scale, const char* name, bool filled) {
    AppTheme appTheme(QString::fromStdString(theme), scale);
    Json saved = Json::object();
    if (filled) {
        saved["automation_webhooks"] = Json::array({kWebhook});
        saved["automation_programs"] = Json::array({kProgram});
        saved["automation_filters"] = Json::array({kFilterRow});
    }
    HooksPageRig rig(saved);
    if (filled) {
        rig.webhooks.set_deliveries("hook01", deliveries());
        rig.leases.register_lease("http://127.0.0.1:7070/f", "after_cleanup");
    }
    fv::SettingsDialog& dialog = rig.dialog(false);
    fv::SettingsPage* page = dialog.ensurePage(dest::hooks);
    REQUIRE(page != nullptr);
    QScrollArea* scroll = dialog.pageScroll(dest::hooks);
    REQUIRE(scroll != nullptr);
    std::unique_ptr<QWidget> holder(scroll->takeWidget());
    REQUIRE(holder.get() == page);
    holder->setParent(nullptr);
    QCoreApplication::processEvents();
    const QSize old = page->size();
    page->resize(kPageWidth, 4000);
    QResizeEvent resized(page->size(), old);
    QCoreApplication::sendEvent(page, &resized);
    std::unique_ptr<QWidget> body(dialog.pageContent(dest::hooks));
    REQUIRE(body != nullptr);
    body->setParent(nullptr);
    settle(*body);
    body->ensurePolished();
    expectMatches(*body, name, variantName(theme, scale));
}

// _fit: shown once at its width to learn its height, then hidden.
void fit(QWidget& widget, int width) {
    widget.resize(width, kMeasureHeight);
    widget.show();
    QApplication::processEvents();
    widget.hide();
}

void webhookEditorShot(const std::string& theme) {
    AppTheme appTheme(QString::fromStdString(theme), 100);
    HooksRig rig;
    rig.webhooks.set_deliveries("hook01", deliveries());
    fv::WebhookEditor editor(rig.services());
    editor.showItem(
        fv::automation::hooks::load_webhooks(Json{{"automation_webhooks", Json::array({kWebhook})}})
            .front(),
        true);
    fit(editor, kEditorWidth);
    expectMatches(editor, "hooks_webhook_editor/saved", variantName(theme, 100));
}

void filterEditorShot(const std::string& theme) {
    AppTheme appTheme(QString::fromStdString(theme), 100);
    HooksRig rig;
    fv::FilterEditor editor(rig.services());
    fv::automation::hooks::TextFilter filter;
    filter.id = "filt02";
    filter.name = "Fix product names";
    filter.kind = "program";
    filter.stage = "before_cleanup";
    filter.program = "C:\\Tools\\fix.exe";
    filter.args = {"--strict"};
    filter.timeout_ms = 1500;
    editor.showItem(filter, true);
    fit(editor, kEditorWidth);
    expectMatches(editor, "hooks_filter_editor/program", variantName(theme, 100));
}

} // namespace

TEST_CASE("reference screenshot settings_hooks_page empty dark 100", "[screenshot]") {
    pageShot("dark", 100, "settings_hooks_page/empty", false);
}

TEST_CASE("reference screenshot settings_hooks_page empty light 100", "[screenshot]") {
    pageShot("light", 100, "settings_hooks_page/empty", false);
}

TEST_CASE("reference screenshot settings_hooks_page empty dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_hooks_page/empty", false);
}

TEST_CASE("reference screenshot settings_hooks_page filled dark 100", "[screenshot]") {
    pageShot("dark", 100, "settings_hooks_page/filled", true);
}

TEST_CASE("reference screenshot settings_hooks_page filled light 100", "[screenshot]") {
    pageShot("light", 100, "settings_hooks_page/filled", true);
}

TEST_CASE("reference screenshot settings_hooks_page filled dark 130", "[screenshot]") {
    pageShot("dark", 130, "settings_hooks_page/filled", true);
}

TEST_CASE("reference screenshot hooks_webhook_editor saved dark 100", "[screenshot]") {
    webhookEditorShot("dark");
}

TEST_CASE("reference screenshot hooks_webhook_editor saved light 100", "[screenshot]") {
    webhookEditorShot("light");
}

TEST_CASE("reference screenshot hooks_filter_editor program dark 100", "[screenshot]") {
    filterEditorShot("dark");
}

TEST_CASE("reference screenshot hooks_filter_editor program light 100", "[screenshot]") {
    filterEditorShot("light");
}
