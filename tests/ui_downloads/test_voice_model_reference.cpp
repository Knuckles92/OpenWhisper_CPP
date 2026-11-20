// The Voice model page inside the Settings window against the references OpenWhisper rendered for
// it (tools/parity/builders/a15.py, tests/reference/settings_voice_model/<variant>/): the real page
// among stand-ins for every other destination, at 1315x850. The widget tree first, then the
// pixels, with the application styled as OpenWhisper styles it.
//
// OpenWhisper builds the page's controls straight into the Settings page's column; FioriVoce's
// page is one widget (voiceModelPage) that holds them. The tree is compared with that wrapper
// taken out, which is the same widgets at the same geometry.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <QAbstractScrollArea>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEvent>
#include <QFontMetrics>
#include <QImage>
#include <QList>
#include <QRect>
#include <QScrollBar>
#include <QSize>

#include "appkit/destinations.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_downloads_support.h"
#include "ui_widgets/engine_field.h"
#include "voice_model_support.h"

namespace {

using fv::Json;

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start; Qt 6.10 leaves them at QWidget's default
// 100 x 30. They are never drawn, so only the trees differ, and here they are given 6.11's sizes
// (architecture amendment 17, as tests/ui_settings does). A bar already sized is left alone.
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    const QList<QAbstractScrollArea*> areas = root.findChildren<QAbstractScrollArea*>();
    for (QAbstractScrollArea* area : areas) {
        for (QScrollBar* bar : {area->horizontalScrollBar(), area->verticalScrollBar()}) {
            QWidget* container = bar->parentWidget();
            if (container == nullptr || container->size() != kUnsized || bar->size() != kUnsized) {
                continue;
            }
            const QSize hint = bar->sizeHint();
            const QSize sized = bar->orientation() == Qt::Horizontal
                                    ? QSize(kUnsized.width(), hint.height())
                                    : QSize(hint.width(), kUnsized.height());
            container->resize(sized);
            bar->resize(sized);
        }
    }
}

// Qt 6.11, which rendered the references, takes a combo's minimum width from its current text; 6.10
// takes it from its longest item, which shares the engine row differently by a few pixels. The
// widgets keep Qt's own rule; the kit's is made the one the references show (architecture
// amendment 21, as tests/ui_record does).
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

bool hasToken(const std::string& sheet) {
    return sheet.find('@') != std::string::npos || sheet.find("font-size") != std::string::npos;
}

// The port's tree as the reference's schema has it, where the port differs by design:
//  - a widget-local sheet with neither a token nor a font size is not source for OpenWhisper's
//    style filter, so the reference has no sheet properties on it, while setLocalStyleSheet always
//    keeps the source;
//  - the geometry of a widget that is hidden in the reference is not compared.
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

// Replaces the node named `wrapper` by its children in its parent's list: the page's controls
// where OpenWhisper has them, in the Settings page's column.
void flatten(Json& tree, const std::string& wrapper) {
    if (!tree.contains("children")) {
        return;
    }
    Json& children = tree["children"];
    for (std::size_t index = 0; index < children.size(); ++index) {
        if (children[index].value("objectName", std::string()) == wrapper) {
            Json inner =
                children[index].contains("children") ? children[index]["children"] : Json::array();
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(index));
            for (std::size_t at = 0; at < inner.size(); ++at) {
                children.insert(children.begin() + static_cast<std::ptrdiff_t>(index + at),
                                inner[at]);
            }
            return;
        }
    }
    for (Json& child : children) {
        flatten(child, wrapper);
    }
}

// A stand-in for every destination the rail lists but the Voice model page: built on first
// selection, which never happens here.
void registerStandIns(fv::AppContext& context) {
    const auto add = [&context](const fv::destinations::RailPage& page) {
        if (page.key == fv::destinations::voice_model) {
            return;
        }
        fv::SettingsPageSpec spec;
        spec.destination = page.key;
        spec.title = QString::fromUtf8(page.name.data(), static_cast<qsizetype>(page.name.size()));
        spec.scroll = page.key != fv::destinations::downloads;
        spec.create = [](QWidget* parent) {
            return parent;
        };
        context.settingsPages().addPage(std::move(spec));
    };
    add(fv::destinations::home);
    for (const fv::destinations::RailChapter& chapter : fv::destinations::rail) {
        for (const fv::destinations::RailPage& page : chapter.pages) {
            add(page);
        }
    }
}

// What the builder's patches say is on this computer: Parakeet TDT 0.6B v3 downloaded and the CPU
// runtime installed.
fv::VoiceModelPageOptions computerOptions() {
    fv::VoiceModelPageOptions options;
    options.cachedModels = [] {
        return std::vector<std::string>{"parakeet-v3"};
    };
    options.runtimeLabel = [](const std::string& backend, const std::string& device) {
        return fv::engine_runtime_label(backend, device, fvtest::hostWith({"asr-nvidia-cpu"}));
    };
    return options;
}

Json settings(Json engine) {
    Json saved{{"dictation_languages", Json::array({"en", "ru"})},
               {"dictation_active_language", "en"},
               {"local_asr_models", Json::object({{"parakeet", "parakeet-v3"}})},
               {"local_asr_devices", Json::object({{"parakeet", "auto"}})}};
    for (auto it = engine.begin(); it != engine.end(); ++it) {
        saved[it.key()] = it.value();
    }
    return saved;
}

void voiceModel(const std::string& theme, int scale, const char* name, const Json& saved) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    fvtest::WindowRig rig(saved);
    registerStandIns(rig.context());
    fv::registerVoiceModelPage(rig.context(), computerOptions);
    fv::SettingsDialog dialog(rig.context());
    dialog.availableScreenRect = [] {
        return QRect(0, 0, 1920, 1080);
    };
    dialog.selectDestination(QString::fromUtf8(fv::destinations::voice_model));
    // The page's worker and the model scan: what is downloaded is known before the first show.
    rig.io().run_all();
    QCoreApplication::processEvents();
    // The language chips are rebuilt on every refresh and the old ones deleted later; the
    // reference shows the chips that stay.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    const Json pins = fv::screenshot::load_reference(name, variant);
    dialog.setFixedSize(pins.at("logical_size")[0].get<int>(),
                        pins.at("logical_size")[1].get<int>());
    matchQtMinorMinimumWidths(dialog);
    dialog.show();
    QCoreApplication::processEvents();
    // render_reference.py dumps the tree after its grab, which lays out the hidden pages and rail
    // rows the stack and the rail hold; the tree compared here is taken after one as well.
    (void)dialog.grab();
    sizeHiddenScrollBarsAsQt611(dialog);

    Json actual = fv::screenshot::dump_tree(dialog);
    flatten(actual, "voiceModelPage");
    normalize(actual, pins.at("tree"));
    fv::screenshot::Options options;
    // OpenWhisper's page container, rail rows and chapter rows are private classes (_SettingsPage,
    // _RailItem, _ChapterRow): their names lose the underscore here.
    options.ignore_keys = {"class"};
    for (const std::string& difference :
         fv::screenshot::compare_trees(pins.at("tree"), actual, options.ignore_keys)) {
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
    const QImage grab = dialog.grab().toImage().convertToFormat(QImage::Format_ARGB32);
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

} // namespace

TEST_CASE("reference screenshot settings_voice_model parakeet dark", "[screenshot]") {
    voiceModel("dark", 100, "settings_voice_model/parakeet",
               settings(Json{{"selected_model", "parakeet"}}));
}

TEST_CASE("reference screenshot settings_voice_model parakeet light", "[screenshot]") {
    voiceModel("light", 100, "settings_voice_model/parakeet",
               settings(Json{{"selected_model", "parakeet"}}));
}

TEST_CASE("reference screenshot settings_voice_model parakeet dark 130", "[screenshot]") {
    voiceModel("dark", 130, "settings_voice_model/parakeet",
               settings(Json{{"selected_model", "parakeet"}}));
}

TEST_CASE("reference screenshot settings_voice_model groq dark", "[screenshot]") {
    voiceModel("dark", 100, "settings_voice_model/groq",
               settings(Json{{"selected_model", "api"}, {"api_transcription_endpoint", "groq"}}));
}
