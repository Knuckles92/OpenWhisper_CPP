// The Upload File tab's widgets, the reading window, the "Describe These Files" dialog and the
// microphone check against the references OpenWhisper rendered for them
// (tools/parity/builders/a08.py and a05.py, tests/reference/<widget>/<variant>/
// <theme>-windows11-dpr1-s<scale>.{png,json}): the widget tree first, then the pixels, with the
// application styled as OpenWhisper styles it.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QFontMetrics>
#include <QMimeData>
#include <QUrl>

#include "app_theme.h"
#include "microphone_support.h"
#include "screenshot/screenshot.h"
#include "ui_record/batch_relation_dialog.h"
#include "ui_record/drop_zone.h"
#include "ui_record/file_info_card.h"
#include "ui_record/microphone_check.h"
#include "ui_record/transcript_viewer_dialog.h"
#include "ui_record/transcription_progress.h"
#include "ui_record/upload_queue.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/engine_field.h"
#include "upload_widgets_support.h"

namespace {

using fv::Json;

// What a builder returns: the widget, the widget to focus, and what keeps the widget's data
// alive (declared first, so it outlives the widget).
struct Built {
    std::shared_ptr<void> keep;
    std::unique_ptr<QWidget> widget;
    QWidget* focus = nullptr;
};

const char* const kFiles[] = {"interview-1.mp3", "interview-2.mp3", "interview-3.mp3",
                              "interview-4.mp3", "interview-5.mp3", "interview-6.mp3",
                              "interview-7.mp3"};

// a08.py _preview: 12.4 MB, 14 min 5 s, 44.1 kHz stereo, one pass.
fv::audio::AudioFilePreview preview(const char* name = "interview.mp3", double sizeMb = 12.4,
                                    double seconds = 845.0) {
    fv::audio::AudioFilePreview value = fvtest::samplePreview();
    value.file_path = name;
    value.file_name = name;
    value.file_size_mb = sizeMb;
    value.duration_seconds = seconds;
    return value;
}

Built dropZone(bool reject) {
    auto zone = std::make_unique<fv::DropZoneWidget>();
    auto mime = std::make_shared<QMimeData>();
    if (reject) {
        // A drag arrives at a shown zone (the builder does the same).
        zone->setFixedSize(420, 190);
        zone->show();
        QApplication::processEvents();
        mime->setUrls({QUrl::fromLocalFile(QStringLiteral("/path/to/notes.txt"))});
        QDragEnterEvent event(QPoint(10, 10), Qt::CopyAction, mime.get(), Qt::NoButton,
                              Qt::NoModifier);
        fvtest::deliver(*zone, &event);
        QApplication::processEvents();
    }
    return {mime, std::move(zone), nullptr};
}

std::unique_ptr<fv::FileInfoCard> loadedCard() {
    auto card = std::make_unique<fv::FileInfoCard>();
    card->setPreview(preview());
    return card;
}

Built resultCard() {
    auto card = loadedCard();
    // The reveal is a timer; the reference is its finished text.
    card->setResult(62.0, 845.0, std::nullopt, std::nullopt, false);
    card->setCopyEnabled(true);
    return {nullptr, std::move(card), nullptr};
}

Built queueOfThree() {
    auto queue = std::make_unique<fv::UploadQueueWidget>();
    const struct {
        const char* name;
        double sizeMb;
        double seconds;
        const char* state;
    } files[] = {{"interview-a.mp3", 12.4, 845.0, "done"},
                 {"interview-b.mp3", 8.2, 563.0, "active"},
                 {"interview-c.mp3", 3.1, 211.0, "pending"}};
    QList<fv::QueueItem> items;
    for (const auto& file : files) {
        fv::QueueItem item;
        item.path = QString::fromLatin1(file.name);
        item.preview = preview(file.name, file.sizeMb, file.seconds);
        item.state = QString::fromLatin1(file.state);
        if (item.state == QLatin1String("done")) {
            item.transcript = QStringLiteral("Good morning, thanks for joining.");
        }
        items.append(item);
    }
    queue->setItems(items);
    return {nullptr, std::move(queue), nullptr};
}

Built batchPanel() {
    auto now = std::make_shared<double>(100.0);
    auto panel = std::make_unique<fv::TranscriptionProgressPanel>();
    panel->setClock([now] { return *now; });
    panel->start(true, 3);
    panel->setBatchPosition(2, 3, QStringLiteral("interview-b.mp3"));
    panel->setStage(fv::dictation::ProgressStage::Transcribing);
    *now = 142.0;
    panel->tick();
    // The bar eases toward its fraction on a timer; the reference is where it settles.
    panel->bar()->setFraction(panel->bar()->fraction(), false);
    return {now, std::move(panel), nullptr};
}

Built batchDialog() {
    QStringList files;
    for (const char* name : kFiles) {
        files.append(QString::fromLatin1(name));
    }
    auto dialog = std::make_unique<fv::BatchRelationDialog>(files, QString(), false);
    QWidget* focus = dialog->instructionsEdit();
    return {nullptr, std::move(dialog), focus};
}

Built viewerBatch() {
    using namespace fv::dictation;
    BatchResult result;
    result.request.items = {BatchItem{"interview-a.mp3", std::nullopt},
                            BatchItem{"interview-b.mp3", std::nullopt}};
    result.request.relation = BatchRelation::Separate;
    BatchItemResult first;
    first.item = result.request.items[0];
    first.text = "Good morning, thanks for joining.";
    BatchItemResult second;
    second.item = result.request.items[1];
    second.text = "Let's pick up where we left off.";
    result.items = {first, second};
    auto dialog = std::make_unique<fv::TranscriptViewerDialog>();
    dialog->setBatchResult(result,
                           QStringLiteral("## interview-a.mp3\n\nGood morning, thanks for "
                                          "joining.\n\n## interview-b.mp3\n\nLet's pick up where "
                                          "we left off."),
                           {}, QStringLiteral("2 files"));
    return {nullptr, std::move(dialog), nullptr};
}

Built hearingMicrophone() {
    auto rig = std::make_shared<fvtest::MicrophoneRig>();
    auto check = std::make_unique<fv::MicrophoneCheck>(rig->factory());
    check->start();
    check->applyLevel(static_cast<float>(0.004 + 0.36 * 0.196));
    return {rig, std::move(check), nullptr};
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

// Python class names that differ from the port's: private classes the port names (a Python
// `_StepChip` is `UploadStepChip`, `_ReaderView` is `ReaderView`).
std::string portClass(const std::string& pythonClass) {
    if (pythonClass == "_StepChip") {
        return "UploadStepChip";
    }
    if (pythonClass == "_ReaderView") {
        return "ReaderView";
    }
    return pythonClass;
}

// The port's tree as the reference's schema has it. Differences that are the port's by design,
// each named here:
//  - a widget-local sheet with neither a token nor a font size is not source for OpenWhisper's
//    style filter, so the reference has no sheet properties on it, while setLocalStyleSheet
//    always keeps the source;
//  - the geometry of a widget that is hidden in the reference is not compared;
//  - private Python classes (see portClass).
void normalize(Json& node, const Json& expected) {
    // A Python class that starts with Q and has no Qt class of its own in its chain has
    // qtClass equal to itself.
    const auto& expectedClass = expected["class"].get_ref<const std::string&>();
    if (portClass(expectedClass) != expectedClass) {
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
    Built built = build();
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    const Json pins = fv::screenshot::load_reference(widget, variant);

    // Laid out as expect_matches lays it out, then compared with the names mapped.
    built.widget->setFixedSize(pins["logical_size"][0].get<int>(),
                               pins["logical_size"][1].get<int>());
    matchQtMinorMinimumWidths(*built.widget);
    built.widget->show();
    if (built.focus != nullptr) {
        built.focus->setFocus();
    }
    QApplication::processEvents();
    Json actual = fv::screenshot::dump_tree(*built.widget);
    normalize(actual, pins["tree"]);
    for (const auto& difference : fv::screenshot::compare_trees(pins["tree"], actual)) {
        FAIL_CHECK(widget << "/" << variant << " tree: " << difference);
    }

    fv::screenshot::Options options;
    options.focus = built.focus;
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

REFERENCE_CASE("drop_zone/idle", [] { return dropZone(false); })

REFERENCE_CASE("drop_zone/reject", [] { return dropZone(true); })

REFERENCE_CASE_130("upload_file_card/single", [] { return Built{nullptr, loadedCard(), nullptr}; })

REFERENCE_CASE_130("upload_file_card/result", [] { return resultCard(); })

REFERENCE_CASE("upload_queue/three", [] { return queueOfThree(); })

REFERENCE_CASE("upload_progress_panel/batch", [] { return batchPanel(); })

REFERENCE_CASE("batch_relation_dialog/default", [] { return batchDialog(); })

REFERENCE_CASE("transcript_viewer_dialog/batch", [] { return viewerBatch(); })

REFERENCE_CASE("microphone_check/hearing", [] { return hearingMicrophone(); })
