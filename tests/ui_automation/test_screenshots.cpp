// The action widgets against the references OpenWhisper rendered for them
// (tools/parity/builders/a21.py, tests/reference/<widget>/<variant>/<theme>-windows11-dpr1-
// s100.{png,json}): the widget tree first, then the pixels, with the application styled as
// OpenWhisper styles it. Each widget is shown once at its width and hidden again first, as the
// builder does to learn its height.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QImage>
#include <QTimer>
#include <QWidget>

#include "automation_support.h"
#include "base/json.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_gallery.h"
#include "ui_automation/action_list.h"

namespace {

using fv::Json;

constexpr int kMeasureHeight = 600;

const std::map<std::string, bool> kScan{
    {"claude_code", true}, {"codex", true}, {"opencode_cli", false}};

struct Built {
    std::unique_ptr<QWidget> widget;
    int width;
};

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

// The port's tree as the reference's schema has it, where the port differs by design:
//  - a widget-local sheet with neither a token nor a font size (the status dot's) is not source
//    for OpenWhisper's style filter, so the reference has no sheet properties on it, while
//    setLocalStyleSheet always keeps the source;
//  - the geometry of a widget that is hidden in the reference is not compared: OpenWhisper's
//    segmented bars lay out their choices once more while hidden, which draws nothing.
void normalize(Json& node, const Json& expected) {
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
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

// expect_matches with the tree normalized first.
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

void check(const std::string& widget, const std::string& theme,
           const std::function<Built()>& build) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme));
    Built built = build();
    // _fit: shown once at its width to learn its height, then hidden.
    built.widget->resize(built.width, kMeasureHeight);
    built.widget->show();
    QApplication::processEvents();
    built.widget->hide();
    expectMatches(*built.widget, widget, theme + "-windows11-dpr1-s100");
}

#define REFERENCE_CASE(name, ...)                                                                  \
    TEST_CASE("reference screenshot " name " dark", "[screenshot]") {                              \
        check(name, "dark", __VA_ARGS__);                                                          \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " light", "[screenshot]") {                             \
        check(name, "light", __VA_ARGS__);                                                         \
    }

Built card(const fv::automation::Action& action) {
    return {std::make_unique<fv::ActionCard>(action), 340};
}

Built agentFields(const Json& params) {
    auto fields = std::make_unique<fv::AgentFields>();
    fields->setParams(params);
    fields->setAvailable(kScan, true);
    return {std::move(fields), 560};
}

} // namespace

REFERENCE_CASE("action_card/enabled", [] { return card(fvtest::askClaude()); })

REFERENCE_CASE("action_card/off", [] { return card(fvtest::homeAutomation()); })

REFERENCE_CASE("action_gallery/two-columns",
               [] { return Built{std::make_unique<fv::PresetGallery>(), 680}; })

REFERENCE_CASE("agent_fields/handoff-claude", [] {
    return agentFields(Json{{"agent", "claude_code"}, {"mode", "handoff"}, {"surface", "auto"}});
})

REFERENCE_CASE("agent_fields/ask-codex", [] {
    return agentFields(Json{{"agent", "codex"}, {"mode", "ask"}, {"output", "scratchpad"}});
})

REFERENCE_CASE("shaping_field/profile", [] {
    auto field = std::make_unique<fv::ShapingField>();
    field->setChoices(Json{
        {"transcript_cleanup_profiles",
         Json::array({Json{
             {"id", "email"}, {"name", "Email"}, {"instructions", "Write it as a short email."}}})},
        {"text_transforms",
         Json::array(
             {Json{{"id", "formal"}, {"name", "Formal"}, {"instruction", "Make it formal."}}})}});
    field->setValue(QStringLiteral("profile:email"));
    return Built{std::move(field), 480};
})

REFERENCE_CASE("action_status_line/attention", [] {
    auto line = std::make_unique<fv::StatusLine>(
        QStringLiteral("Fast judgments"),
        QStringLiteral(u"Open Settings → Meeting Mode → Fast judgments"));
    line->showState(QString::fromLatin1(fv::StatusLine::kAttention),
                    QStringLiteral(u"Route by voice needs Fast judgments. Turn them on in "
                                   u"Settings → Meeting Mode → Fast judgments."),
                    true);
    return Built{std::move(line), 560};
})
