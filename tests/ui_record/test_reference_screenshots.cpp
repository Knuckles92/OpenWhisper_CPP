// The Quick Record tab, the compact controller and the stats strip against the references
// OpenWhisper rendered for them (tools/parity/builders/a08.py, tests/reference/<widget>/
// <variant>/<theme>-windows11-dpr1-s<scale>.{png,json}): the widget tree first, then the pixels,
// with the application styled as OpenWhisper styles it.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QFontMetrics>

#include "app_theme.h"
#include "record_fixture.h"
#include "reference_detach.h"
#include "screenshot/screenshot.h"
#include "settings/testing/memory_store.h"
#include "ui_record/cleanup_profile_card.h"
#include "ui_record/compact_record_controller.h"
#include "ui_record/stats_display.h"
#include "ui_widgets/engine_field.h"

namespace {

using fv::Json;

// The settings of a fresh Parakeet install, as the builders save them.
Json freshSettings() {
    return Json{{"selected_model", "parakeet"},
                {"local_asr_models", {{"parakeet", "parakeet-v3"}}},
                {"local_asr_devices", {{"parakeet", "auto"}}},
                {"transcript_cleanup_enabled", false},
                {"streaming_enabled", false},
                {"hotkeys", {{"command_mode", ""}}}};
}

// What a builder returns: the widget, and what keeps it alive (the settings the tab reads).
struct Built {
    std::shared_ptr<fv::MemorySettingsStore> settings; // declared first: outlives the widget
    std::unique_ptr<QWidget> widget;
};

Built quickRecord(const std::function<void(fv::QuickRecordTab&)>& prepare) {
    auto settings = std::make_shared<fv::MemorySettingsStore>(freshSettings());
    auto tab = std::make_unique<fv::QuickRecordTab>(*settings, fv::speechEngineCatalog());
    tab->syncEngineSelection();
    tab->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | CPU"), true);
    tab->updateHotkeys(QStringLiteral("Ctrl+Alt+R"), QStringLiteral("Esc"));
    prepare(*tab);
    // The cleanup profile card is hidden here and taken out of the reference's tree, as the
    // builder's _prune does; only quick_record_tab/profile shows it.
    delete tab->profileCard();
    fvtest::detachRemoteEngine(*tab);
    return {settings, std::move(tab)};
}

// AI cleanup on, two profiles with Email chosen and given a shortcut, a Command Mode shortcut and
// a provider that is ready (the builder's quick_record_profile).
Built quickRecordProfile() {
    Json saved = freshSettings();
    saved["transcript_cleanup_enabled"] = true;
    saved["transcript_cleanup_profiles"] = Json::array(
        {Json{{"id", "support-ticket"},
              {"name", "Support ticket"},
              {"instructions", "Format the dictation as a support ticket."},
              {"hotkey", ""},
              {"use_learned_rules", true}},
         Json{{"id", "email"},
              {"name", "Email"},
              {"instructions", "Turn the dictation into a concise, professional email."},
              {"hotkey", "ctrl+alt+e"},
              {"use_learned_rules", true}}});
    saved["quick_record_profile"] = "email";
    saved["hotkeys"] = Json{{"command_mode", "ctrl+alt+k"}};
    auto settings = std::make_shared<fv::MemorySettingsStore>(saved);
    auto tab = std::make_unique<fv::QuickRecordTab>(*settings, fv::speechEngineCatalog());
    tab->setCommandModeReadiness([](const Json&) { return std::optional<bool>(true); });
    tab->syncEngineSelection();
    tab->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | CPU"), true);
    tab->updateHotkeys(QStringLiteral("Ctrl+Alt+R"), QStringLiteral("Esc"));
    fvtest::detachRemoteEngine(*tab);
    return {settings, std::move(tab)};
}

// Qt 6.11, which rendered the references, takes a combo's minimum width from its current text;
// 6.10 takes it from its longest item, which gives the Language field 96 px where OpenWhisper's
// gets 80 and shares the engine row differently by 2 px. The widget keeps Qt's own rule; the
// kit's is made the one the references show.
void matchQtMinorMinimumWidths(QWidget& widget) {
    if (QT_VERSION < QT_VERSION_CHECK(6, 11, 0)) {
        widget.ensurePolished();
        for (QComboBox* combo : widget.findChildren<QComboBox*>()) {
            combo->ensurePolished();
            const QFontMetrics metrics = combo->fontMetrics();
            int longest = 0;
            for (int i = 0; i < combo->count(); ++i) {
                longest = std::max(longest, metrics.boundingRect(combo->itemText(i)).width());
            }
            const int chrome = combo->sizeHint().width() - longest;
            combo->setMinimumWidth(
                std::min(chrome + metrics.boundingRect(combo->currentText()).width(),
                         fv::kFieldMinimumWidth));
        }
    }
}

// The tree keys a mapping below compares itself.
const std::vector<std::string> kTreeKeys{
    "class", "qtClass",   "objectName", "geometry",  "visible",  "hasFocus",   "properties",
    "text",  "isChecked", "isDefault",  "alignment", "wordWrap", "styleSheet", "font"};

bool hasToken(const std::string& sheet) {
    return sheet.find('@') != std::string::npos || sheet.find("font-size") != std::string::npos;
}

// The port's tree as the reference's schema has it. Differences that are the port's by design,
// each named here:
//  - a widget-local sheet with neither a token nor a font size is not source for OpenWhisper's
//    style filter, so the reference has no sheet properties on it, while setLocalStyleSheet
//    always keeps the source;
//  - the geometry of a widget that is hidden in the reference is not compared;
//  - the Python class names that start with Q make the first Qt class of their chain the class
//    itself (QuickRecordTab), and a private Python class (_QuickRecordTranscript) or a widget of
//    a later package (FieldHelp) is a Qt class here.
void normalize(Json& node, const Json& expected) {
    // render_reference.py's qt_base_class takes the first class whose name starts with Q, which
    // for QuickRecordTab is the tab itself; only that case is mapped.
    const auto& expectedClass = expected["class"].get_ref<const std::string&>();
    if (expectedClass.starts_with('Q') && expected["qtClass"] == expectedClass &&
        node["class"] == expectedClass) {
        node["qtClass"] = expected["qtClass"];
    }
    // A widget that is not shown keeps the geometry of whichever layout pass last reached it (a
    // fresh widget's 640 x 480 until one does), which no one sees and Qt makes no promise about.
    if (!expected["visible"].get<bool>()) {
        node["geometry"] = expected["geometry"];
    }
    for (const char* cls : {"FieldHelp", "_QuickRecordTranscript"}) {
        if (expected["class"] == cls) {
            node["class"] = cls;
        }
    }
    // OpenWhisper's profile card is a bare QFrame; here it is the CleanupProfileCard that builds
    // its row.
    if (expected["class"] == "QFrame" && node["class"] == "CleanupProfileCard") {
        node["class"] = "QFrame";
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

void check(const std::string& widget, const std::string& theme, int scale,
           const std::function<Built()>& build) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    Built built = build();
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const Json pins = fv::screenshot::load_reference(widget, variant);

    // Laid out as expect_matches lays it out, then compared with the names mapped.
    built.widget->setFixedSize(pins["logical_size"][0].get<int>(),
                               pins["logical_size"][1].get<int>());
    matchQtMinorMinimumWidths(*built.widget);
    built.widget->show();
    QApplication::processEvents();
    Json actual = fv::screenshot::dump_tree(*built.widget);
    normalize(actual, pins["tree"]);
    for (const auto& difference : fv::screenshot::compare_trees(pins["tree"], actual)) {
        FAIL_CHECK(widget << "/" << variant << " tree: " << difference);
    }

    fv::screenshot::Options options;
    options.ignore_keys = kTreeKeys;
    fv::screenshot::expect_matches(*built.widget, widget, variant, options);
}

#define REFERENCE_CASE(name, ...)                                                                  \
    TEST_CASE("reference screenshot " name " dark", "[screenshot]") {                              \
        check(name, "dark", 100, __VA_ARGS__);                                                     \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " light", "[screenshot]") {                             \
        check(name, "light", 100, __VA_ARGS__);                                                    \
    }

#define REFERENCE_CASE_130(name, ...)                                                              \
    REFERENCE_CASE(name, __VA_ARGS__)                                                              \
    TEST_CASE("reference screenshot " name " dark 130 percent", "[screenshot]") {                  \
        check(name, "dark", 130, __VA_ARGS__);                                                     \
    }

} // namespace

REFERENCE_CASE_130("quick_record_tab/idle", [] { return quickRecord([](fv::QuickRecordTab&) {}); })

REFERENCE_CASE_130("quick_record_tab/transcript", [] {
    return quickRecord([](fv::QuickRecordTab& tab) {
        tab.setTranscriptionCollapsed(false);
        tab.setTranscript(QStringLiteral("Hello from OpenWhisper_CPP."),
                          QStringLiteral("hello from openwhisper-cpp"));
        tab.setTranscriptionStats(2.4, 12.7, 403200);
    });
})

REFERENCE_CASE_130("quick_record_tab/recording", [] {
    return quickRecord([](fv::QuickRecordTab& tab) { tab.setRecordingActive(true); });
})

REFERENCE_CASE_130("quick_record_tab/profile", [] { return quickRecordProfile(); })

REFERENCE_CASE("compact_record_controller/idle", [] {
    auto controller = std::make_unique<fv::CompactRecordController>();
    controller->updateHotkeys(QStringLiteral("Ctrl+Alt+R"), QStringLiteral("Esc"));
    return Built{nullptr, std::move(controller)};
})

REFERENCE_CASE("compact_record_controller/recording", [] {
    auto controller = std::make_unique<fv::CompactRecordController>();
    controller->updateHotkeys(QStringLiteral("Ctrl+Alt+R"), QStringLiteral("Esc"));
    controller->setRecordingState(true);
    controller->setStatus(QStringLiteral("Recording in progress..."));
    return Built{nullptr, std::move(controller)};
})

REFERENCE_CASE("transcription_stats/filled", [] {
    auto stats = std::make_unique<fv::TranscriptionStatsWidget>();
    stats->setStats(2.4, 12.7, 403200);
    fvtest::detachRemoteDetail(*stats);
    return Built{nullptr, std::move(stats)};
})
