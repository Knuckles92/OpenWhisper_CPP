// The Downloads page inside the Settings window against the references OpenWhisper rendered for
// it (tools/parity/builders/a15.py, tests/reference/settings_downloads/<variant>/): the real page
// among stand-ins for every other destination, with Parakeet TDT 0.6B v3 downloaded, Orukeet
// not, the CPU runtime installed and the CUDA runtime available. The widget tree first, then the
// pixels, with the application styled as OpenWhisper styles it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <QAbstractScrollArea>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFontMetrics>
#include <QImage>
#include <QList>
#include <QRect>
#include <QScrollBar>
#include <QSize>
#include <QtGlobal>

#include "appkit/destinations.h"
#include "appkit/registries.h"
#include "downloads_page_support.h"
#include "models/catalog.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_downloads/downloads_page.h"
#include "ui_downloads/downloads_presenter.h"
#include "ui_settings/settings_dialog.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "voice_model_support.h"

namespace {

using fv::Json;

const QString kDestination = QStringLiteral("C:/Users/test/AppData/Local/FioriVoce/speech-models");

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

// A stand-in for every destination the rail lists but Downloads: built on first selection,
// which never happens here.
void registerStandIns(fv::AppContext& context) {
    const auto add = [&context](const fv::destinations::RailPage& page) {
        if (page.key == fv::destinations::downloads) {
            return;
        }
        fv::SettingsPageSpec spec;
        spec.destination = page.key;
        spec.title = QString::fromUtf8(page.name.data(), static_cast<qsizetype>(page.name.size()));
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

// Qt 6.11, which rendered the references, sizes a combo's minimum differently from 6.10, which
// takes it from the longest item (architecture amendment 21). The narrow reference shows the
// catalog's toolbar shrinking its three combos by the same 15 px, so none of them sits at a
// minimum there, while 6.10's longest-item minimum stops the backend and sort combos at their full
// width. The kit's minimums are lowered to each combo's chrome, below any width the page gives
// them; at the default width they are at their size hints either way.
void lowerToolbarComboMinimums(fv::DownloadsPage& page) {
    if (QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)) {
        return;
    }
    for (QComboBox* combo : {static_cast<QComboBox*>(page.backendFilterCombo()),
                             static_cast<QComboBox*>(page.statusFilterCombo()),
                             static_cast<QComboBox*>(page.sortCombo())}) {
        combo->ensurePolished();
        const QFontMetrics metrics = combo->fontMetrics();
        int longest = 0;
        for (int i = 0; i < combo->count(); ++i) {
            longest = std::max(longest, metrics.boundingRect(combo->itemText(i)).width());
        }
        combo->setMinimumWidth(combo->sizeHint().width() - longest);
    }
}

enum class Shown { Models, Components };

void downloads(const std::string& theme, int scale, const char* name, Shown shown) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    const std::string variant = theme + "-windows11-dpr1-s" + std::to_string(scale);
    fvtest::WindowRig rig(Json{{"selected_model", "parakeet"},
                               {"local_asr_models", Json::object({{"parakeet", "parakeet-v3"}})},
                               {"hf_access_policy", "ask"}});
    // What the builder's patches say is on this computer.
    fvtest::DownloadsRig computer;
    computer.cache(fvtest::kParakeetRepo, fvtest::kParakeetBytes);
    computer.components = {
        fvtest::runtimeInfo(fvtest::kCpuRuntime, fv::models::ComponentState::Installed),
        fvtest::runtimeInfo(fvtest::kCudaRuntime)};
    fv::DownloadsPresenter presenter(rig.settings(), rig.ui(), rig.io());
    presenter.setPageOptions([&computer] {
        fv::DownloadsPageOptions options = computer.options();
        options.modelsFolder = kDestination;
        return options;
    });
    registerStandIns(rig.context());
    fv::registerDownloadsPage(rig.context(), presenter);
    fv::SettingsDialog dialog(rig.context());
    dialog.availableScreenRect = [] {
        return QRect(0, 0, 1920, 1080);
    };
    dialog.selectDestination(QString::fromUtf8(fv::destinations::downloads));
    auto* page = qobject_cast<fv::DownloadsPage*>(dialog.pageContent(fv::destinations::downloads));
    REQUIRE(page != nullptr);
    page->refresh();
    if (shown == Shown::Components) {
        page->tabs()->setCurrentIndex(1);
    } else {
        page->selectModel(QString::fromStdString(fvtest::kParakeet));
    }
    dialog.refreshRailValues();

    const Json pins = fv::screenshot::load_reference(name, variant);
    dialog.setFixedSize(pins.at("logical_size")[0].get<int>(),
                        pins.at("logical_size")[1].get<int>());
    lowerToolbarComboMinimums(*page);
    dialog.show();
    QCoreApplication::processEvents();
    // render_reference.py dumps the tree after its grab, which lays out the hidden pages and rail
    // rows the stack and the rail hold; the tree compared here is taken after one as well.
    (void)dialog.grab();
    sizeHiddenScrollBarsAsQt611(dialog);

    Json actual = fv::screenshot::dump_tree(dialog);
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

TEST_CASE("reference screenshot settings_downloads models dark", "[screenshot]") {
    downloads("dark", 100, "settings_downloads/models", Shown::Models);
}

TEST_CASE("reference screenshot settings_downloads models light", "[screenshot]") {
    downloads("light", 100, "settings_downloads/models", Shown::Models);
}

TEST_CASE("reference screenshot settings_downloads models dark 130", "[screenshot]") {
    downloads("dark", 130, "settings_downloads/models", Shown::Models);
}

TEST_CASE("reference screenshot settings_downloads components dark", "[screenshot]") {
    downloads("dark", 100, "settings_downloads/components", Shown::Components);
}

TEST_CASE("reference screenshot settings_downloads components light", "[screenshot]") {
    downloads("light", 100, "settings_downloads/components", Shown::Components);
}

TEST_CASE("reference screenshot settings_downloads floating-inspector dark", "[screenshot]") {
    downloads("dark", 100, "settings_downloads/floating-inspector", Shown::Models);
}
