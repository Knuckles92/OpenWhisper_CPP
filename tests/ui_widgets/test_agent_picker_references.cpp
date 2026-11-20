// The agent picker against the references OpenWhisper rendered for it (tools/parity/builders/
// a21_picker.py, tests/reference/agent_picker/<variant>/<theme>-windows11-dpr1-s<scale>.{png,
// json}): the widget tree first, then the pixels, with the application styled as OpenWhisper
// styles it. The picker is built as the builder builds it: the same cached scan and model list,
// the choice made before it is first shown, then shown once at 760 px and hidden again to learn
// its height. The cached scan shows without an arrival or shimmer, so every animation is at its
// end value.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEvent>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "agents/installed_agents.h"
#include "app_theme.h"
#include "base/identity.h"
#include "base/json.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_widgets/agent_picker.h"

namespace agents = fv::agents;
namespace picker = fv::agent_picker;
using fv::Json;

namespace {

constexpr int kWidth = 760;
constexpr int kMeasureHeight = 900;

agents::ScanResult scan() {
    agents::InstalledAgent claude;
    claude.id = "claude_code";
    claude.path = "C:/bin/claude.exe";
    claude.version = "2.1.3";
    claude.account = "Claude Max";
    claude.signed_in = true;
    agents::InstalledAgent codex;
    codex.id = "codex";
    codex.path = "C:/bin/codex.exe";
    codex.version = "0.100.0";
    codex.problem = std::string(fv::identity().product) +
                    " needs Codex 0.120.0 or newer. Run `npm install -g @openai/codex` or update "
                    "the Codex app.";
    return {{"claude_code", claude}, {"codex", codex}, {"opencode_cli", std::nullopt}};
}

std::vector<agents::AgentModel> models(const agents::InstalledAgent& agent) {
    if (agent.id != "claude_code") {
        return {};
    }
    return {{"", "Claude Code default"}, {"haiku", "Haiku"}, {"sonnet", "Sonnet"}};
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

// The port's tree as the reference's schema has it, where the port differs by design:
//  - Python's private _TileGrid is TileGrid here;
//  - the geometry of a widget that is hidden in the reference is not compared: OpenWhisper lays
//    out the hidden notice and model card once more while hidden, which draws nothing.
void normalize(Json& node, const Json& expected) {
    if (expected.value("class", std::string()) == "_TileGrid" &&
        node.value("class", std::string()) == "TileGrid") {
        node["class"] = "_TileGrid";
    }
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
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

void check(const char* widget, const QString& purpose, const QString& choice,
           const std::string& theme, int scale) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    picker::ScanSeam& seam = picker::scanSeam();
    const agents::ScanResult cached = scan();
    seam.cached_agents = [cached] {
        return std::optional(cached);
    };
    seam.list_agent_models = models;
    seam.scan_installed_agents = [](bool) -> agents::ScanResult {
        throw std::logic_error("no scan while a reference is compared");
    };
    auto built = std::make_unique<fv::AgentPicker>(purpose);
    built->setChoice(choice);
    // _fit: shown once at its width to learn its height, then hidden.
    built->resize(kWidth, kMeasureHeight);
    built->show();
    QCoreApplication::processEvents();
    built->hide();
    picker::resetScanSeam();
    expectMatches(*built, widget, theme + "-windows11-dpr1-s" + std::to_string(scale));
}

QString builtin() {
    return QString::fromUtf8(fv::legacy_builtin_agent_id.data(),
                             static_cast<qsizetype>(fv::legacy_builtin_agent_id.size()));
}

} // namespace

#define REFERENCE_CASE(name, purpose, choice)                                                      \
    TEST_CASE("reference screenshot " name " dark", "[screenshot]") {                              \
        check(name, purpose, choice, "dark", 100);                                                 \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " light", "[screenshot]") {                             \
        check(name, purpose, choice, "light", 100);                                                \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " dark 130", "[screenshot]") {                          \
        check(name, purpose, choice, "dark", 130);                                                 \
    }

REFERENCE_CASE("agent_picker/insights-builtin", QStringLiteral("AI insights"), builtin())

REFERENCE_CASE("agent_picker/cleanup-claude", QStringLiteral("AI cleanup"),
               QStringLiteral("claude_code"))
