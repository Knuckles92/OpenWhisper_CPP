// The Upload File tab against the references OpenWhisper rendered for it
// (tools/parity/builders/a08.py upload_file_tab_*, tests/reference/upload_file_tab/<variant>/
// <theme>-windows11-dpr1-s<scale>.{png,json}): the widget tree first, then the pixels, with the
// application styled as OpenWhisper styles it.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QFontMetrics>

#include "app_theme.h"
#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "http/testing/fake_http_client.h"
#include "reference_detach.h"
#include "screenshot/screenshot.h"
#include "settings/testing/memory_store.h"
#include "ui_record/file_info_card.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/upload_file_tab.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/speech_server_controls.h"
#include "upload_widgets_support.h"

namespace {

using fv::Json;

// What a builder returns: the widget, and what keeps it alive (declared first, so it outlives
// the widget).
struct Built {
    std::shared_ptr<void> keep;
    std::unique_ptr<QWidget> widget;
};

// The settings of a fresh Parakeet install, as the builders save them.
Json freshSettings() {
    return Json{{"selected_model", "parakeet"},
                {"local_asr_models", {{"parakeet", "parakeet-v3"}}},
                {"local_asr_devices", {{"parakeet", "auto"}}},
                {"transcript_cleanup_enabled", false},
                {"streaming_enabled", false},
                {"hotkeys", {{"command_mode", ""}}}};
}

struct Keep {
    explicit Keep(const Json& saved) : settings(saved) {}
    fv::MemorySettingsStore settings;
    fv::http::FakeHttpClient client;
    fv::ManualExecutor io;
};

// a08.py _preview: 44.1 kHz stereo, one pass.
fv::audio::AudioFilePreview preview(const char* name, double sizeMb, double seconds) {
    fv::audio::AudioFilePreview value = fvtest::samplePreview();
    value.file_path = name;
    value.file_name = name;
    value.file_size_mb = sizeMb;
    value.duration_seconds = seconds;
    return value;
}

struct File {
    const char* name;
    double sizeMb;
    double seconds;
};

Built uploadTab(const std::function<void(fv::UploadFileTab&, Keep&)>& prepare,
                const Json& extra = Json::object(), bool api = false) {
    Json saved = freshSettings();
    saved.update(extra);
    auto keep = std::make_shared<Keep>(saved);
    auto tab = std::make_unique<fv::UploadFileTab>(keep->settings, fv::speechEngineCatalog());
    if (api) {
        tab->setApiServer(new fv::SpeechServerControls(keep->settings, keep->client, keep->io));
    }
    tab->syncEngineSelection();
    tab->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | CPU"), true);
    prepare(*tab, *keep);
    fvtest::detachRemoteEngine(*tab);
    return {keep, std::move(tab)};
}

void queueFiles(fv::UploadFileTab& tab, std::vector<File> files) {
    tab.setPreviewRunner([](std::function<void()> worker) { worker(); });
    tab.setPreviewReader([files](const std::filesystem::path& path, bool) {
        const std::string name = fv::base_utf8::path_to_utf8(path);
        for (const File& file : files) {
            if (name == file.name) {
                return preview(file.name, file.sizeMb, file.seconds);
            }
        }
        throw fv::audio::AudioFileNotFound("Audio file not found: " + name);
    });
    QStringList paths;
    for (const File& file : files) {
        paths.append(QString::fromLatin1(file.name));
    }
    tab.addFiles(paths);
    QApplication::processEvents();
}

// Qt 6.11, which rendered the references, takes a combo's minimum width from its current text;
// 6.10 takes it from its longest item. The widget keeps Qt's own rule; the kit's is made the one
// the references show.
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

// Python's private classes and the port's names for them.
const std::map<std::string, std::string> kPortClasses{{"_StepChip", "UploadStepChip"}};

// The port's tree as the reference's schema has it. Differences that are the port's by design:
//  - a widget-local sheet with neither a token nor a font size is not source for OpenWhisper's
//    style filter, so the reference has no sheet properties on it, while setLocalStyleSheet
//    always keeps the source;
//  - the geometry of a widget that is hidden in the reference is not compared;
//  - private Python classes, the first Qt class of a Python class that starts with Q, and a
//    widget the port builds from a plain Qt class (FieldHelp).
void normalize(Json& node, const Json& expected) {
    const auto& expectedClass = expected["class"].get_ref<const std::string&>();
    if (const auto found = kPortClasses.find(expectedClass);
        found != kPortClasses.end() && node["class"] == found->second) {
        node["class"] = expectedClass;
    }
    if (expectedClass.starts_with('Q') && expected["qtClass"] == expectedClass &&
        node["class"] == expectedClass) {
        node["qtClass"] = expected["qtClass"];
    }
    if (expectedClass == "FieldHelp") {
        node["class"] = expectedClass;
    }
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

void check(const std::string& widget, const std::string& theme, int scale,
           const std::function<Built()>& build) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    fv::SpeechServerControls::resetCatalogs();
    Built built = build();
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const Json pins = fv::screenshot::load_reference(widget, variant);

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
    built.widget.reset();
    fv::SpeechServerControls::resetCatalogs();
}

#define REFERENCE_CASE_130(name, ...)                                                              \
    TEST_CASE("reference screenshot " name " dark", "[screenshot]") {                              \
        check(name, "dark", 100, __VA_ARGS__);                                                     \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " light", "[screenshot]") {                             \
        check(name, "light", 100, __VA_ARGS__);                                                    \
    }                                                                                              \
    TEST_CASE("reference screenshot " name " dark 130 percent", "[screenshot]") {                  \
        check(name, "dark", 130, __VA_ARGS__);                                                     \
    }

} // namespace

REFERENCE_CASE_130("upload_file_tab/empty",
                   [] { return uploadTab([](fv::UploadFileTab&, Keep&) {}); })

REFERENCE_CASE_130("upload_file_tab/queue", [] {
    return uploadTab([](fv::UploadFileTab& tab, Keep&) {
        queueFiles(tab, {{"interview-a.mp3", 12.4, 845.0},
                         {"interview-b.mp3", 8.2, 563.0},
                         {"interview-c.mp3", 3.1, 211.0}});
    });
})

REFERENCE_CASE_130("upload_file_tab/result", [] {
    return uploadTab([](fv::UploadFileTab& tab, Keep&) {
        queueFiles(tab, {{"interview.mp3", 12.4, 845.0}});
        tab.setTranscriptionCollapsed(false);
        tab.setTranscript(QStringLiteral("## Interview\n\nGood morning, thanks for **joining**."
                                         "\n\n- First point\n- Second point"));
        tab.setTranscriptionStats(62.0, 845.0, 13002342);
        // The reveal is a timer; the reference is its finished text.
        tab.fileInfoCard()->setResult(62.0, 845.0, std::nullopt, std::nullopt, false);
    });
})

REFERENCE_CASE_130("upload_file_tab/api", [] {
    return uploadTab(
        [](fv::UploadFileTab& tab, Keep&) { tab.setDeviceInfo(QString(), std::nullopt); },
        Json{{"selected_model", "api"}, {"api_transcription_endpoint", "groq"}}, true);
})
