// The Downloads rows and dialogs against the references OpenWhisper rendered for them
// (tools/parity/builders/a15.py, tests/reference/<widget>/<variant>/<theme>-windows11-dpr1-s100):
// the widget tree first, then the pixels, with the application styled as OpenWhisper styles it.
//
// The builder's data is the data here: the Parakeet model and the CUDA runtime, an RTX 2060, two
// models for the batch dialog, and a screen 900 px high for the consent notice.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QDialog>
#include <QList>
#include <QScrollBar>
#include <QSize>

#include "models/consent_text.h"
#include "models/downloads_model.h"
#include "screenshot/screenshot.h"
#include "ui_downloads/batch_download_dialog.h"
#include "ui_downloads/component_details_dialog.h"
#include "ui_downloads/component_row_widget.h"
#include "ui_downloads/hf_consent_dialog.h"
#include "ui_downloads/model_row_widget.h"
#include "ui_downloads/required_runtime_dialog.h"
#include "ui_downloads_support.h"

using fv::models::ComponentInfo;
using fv::models::ComponentState;
using fv::models::ModelRowInput;

namespace {

constexpr char kModel[] = "parakeet-v3";
constexpr char kCuda[] = "asr-nvidia-cuda";
constexpr char kCpu[] = "asr-nvidia-cpu";
constexpr std::int64_t kModelBytes = 713'975'456;
constexpr int kScreenHeight = 900;
const QString kDestination =
    QStringLiteral("C:/Users/test/AppData/Local/OpenWhisper_CPP/speech-models");

// Qt 6.11, the kit the references were rendered with, gives a scroll area's hidden scroll bars
// (and their containers) their thickness from the start. Qt 6.10 leaves them at QWidget's
// default 100 x 30. They are never drawn, so only the trees differ, and here they are given
// 6.11's sizes (as tests/ui_history does). A bar already sized is left alone.
void sizeHiddenScrollBarsAsQt611(QWidget& root) {
    constexpr QSize kUnsized(100, 30);
    root.ensurePolished();
    QList<QAbstractScrollArea*> areas = root.findChildren<QAbstractScrollArea*>();
    if (auto* self = qobject_cast<QAbstractScrollArea*>(&root)) {
        areas.prepend(self);
    }
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

struct Scene {
    Scene(const std::string& theme, const char* name)
        : appTheme(QString::fromStdString(theme)), variant(theme + "-windows11-dpr1-s100"),
          name(name) {}

    // As the builder does for a dialog: shown once and hidden again to learn the size it opens
    // at, which is the size recorded.
    void check(QDialog& dialog) {
        dialog.show();
        QCoreApplication::processEvents();
        dialog.hide();
        check(static_cast<QWidget&>(dialog));
    }

    void check(QWidget& widget) {
        sizeHiddenScrollBarsAsQt611(widget);
        fv::screenshot::expect_matches(widget, name, variant);
    }

    fvtest::AppTheme appTheme;
    std::string variant;
    const char* name;
};

ComponentInfo cudaInfo() {
    const auto sizes = fv::models::runtime_sizes(kCuda, fvtest::windowsHost());
    ComponentInfo info;
    info.component_id = kCuda;
    info.display_name = "NVIDIA Speech GPU";
    info.summary = "NVIDIA GPU runtime shared by Parakeet models.";
    info.state = ComponentState::NotInstalled;
    info.download_bytes = sizes.download;
    info.install_bytes = sizes.on_disk;
    return info;
}

fv::models::NvidiaGpu rtx2060() {
    fv::models::NvidiaGpu gpu;
    gpu.name = "NVIDIA GeForce RTX 2060";
    gpu.total_mib = 6144;
    gpu.compute_capability = std::pair{7, 5};
    return gpu;
}

// `shownFirst`: the row is on screen when its state arrives, as when a download starts on the
// page: the progress bar is laid over the row's bottom edge only while the row is visible.
void modelRow(const std::string& theme, const char* name, const ModelRowInput& input,
              bool shownFirst = false) {
    Scene scene(theme, name);
    fv::ModelRowWidget row(kModel);
    if (shownFirst) {
        const fv::Json pins = fv::screenshot::load_reference(name, scene.variant);
        const auto& size = pins.at("logical_size");
        row.setFixedSize(size[0].get<int>(), size[1].get<int>());
        // The page's rows have been given a state before they are shown.
        row.updateState(ModelRowInput{});
        row.show();
        QCoreApplication::processEvents();
    }
    row.updateState(input);
    scene.check(row);
}

ModelRowInput notDownloaded() {
    return {};
}

ModelRowInput downloaded() {
    ModelRowInput input;
    fv::models::CachedModelInfo info;
    info.repo_id = "nvidia/parakeet-tdt-0.6b-v3";
    info.size_on_disk = kModelBytes;
    input.info = info;
    return input;
}

ModelRowInput downloading() {
    ModelRowInput input;
    input.downloading = true;
    input.download_slot_busy = true;
    input.selection_enabled = false;
    return input;
}

void componentRow(const std::string& theme, const char* name, bool installing) {
    Scene scene(theme, name);
    fv::ComponentRowWidget row(kCuda);
    row.updateState(cudaInfo(), installing);
    if (installing) {
        row.setProgress(fv::models::install_phase::kDownloading, 40'000'000, 100'000'000);
    }
    scene.check(row);
}

void consentDialog(const std::string& theme) {
    Scene scene(theme, "hf_consent_dialog/ask");
    fv::models::RuntimeNeeds needs;
    needs.runtime = kCpu;
    fv::HuggingFaceConsentDialog dialog(kModel, "ask", false, needs);
    dialog.setAvailableHeight([] { return kScreenHeight; });
    scene.check(dialog);
}

void requiredRuntimeDialog(const std::string& theme) {
    Scene scene(theme, "required_runtime_dialog/cpu");
    fv::RequiredRuntimeDialog dialog(kModel, kCpu, fvtest::windowsHost());
    scene.check(dialog);
}

void gpuRuntimeDialog(const std::string& theme) {
    Scene scene(theme, "gpu_runtime_dialog/rtx-2060");
    fv::GpuRuntimeDialog dialog(kModel, kCuda, kCpu, rtx2060(), fvtest::windowsHost());
    scene.check(dialog);
}

void batchDialog(const std::string& theme) {
    Scene scene(theme, "batch_download_dialog/two");
    fv::BatchDownloadDialog dialog({kModel, "orukeet-v0.1"}, kDestination);
    scene.check(dialog);
}

void detailsDialog(const std::string& theme) {
    Scene scene(theme, "component_details_dialog/cuda");
    fv::ComponentDetailsDialog dialog(kCuda, cudaInfo());
    scene.check(dialog);
}

} // namespace

TEST_CASE("reference screenshot model_row not-downloaded dark", "[screenshot]") {
    modelRow("dark", "model_row/not-downloaded", notDownloaded());
}

TEST_CASE("reference screenshot model_row not-downloaded light", "[screenshot]") {
    modelRow("light", "model_row/not-downloaded", notDownloaded());
}

TEST_CASE("reference screenshot model_row downloaded dark", "[screenshot]") {
    modelRow("dark", "model_row/downloaded", downloaded());
}

TEST_CASE("reference screenshot model_row downloaded light", "[screenshot]") {
    modelRow("light", "model_row/downloaded", downloaded());
}

TEST_CASE("reference screenshot model_row downloading dark", "[screenshot]") {
    modelRow("dark", "model_row/downloading", downloading(), true);
}

TEST_CASE("reference screenshot model_row downloading light", "[screenshot]") {
    modelRow("light", "model_row/downloading", downloading(), true);
}

TEST_CASE("reference screenshot component_row not-installed dark", "[screenshot]") {
    componentRow("dark", "component_row/not-installed", false);
}

TEST_CASE("reference screenshot component_row not-installed light", "[screenshot]") {
    componentRow("light", "component_row/not-installed", false);
}

TEST_CASE("reference screenshot component_row installing dark", "[screenshot]") {
    componentRow("dark", "component_row/installing", true);
}

TEST_CASE("reference screenshot component_row installing light", "[screenshot]") {
    componentRow("light", "component_row/installing", true);
}

TEST_CASE("reference screenshot hf_consent_dialog ask dark", "[screenshot]") {
    consentDialog("dark");
}

TEST_CASE("reference screenshot hf_consent_dialog ask light", "[screenshot]") {
    consentDialog("light");
}

TEST_CASE("reference screenshot required_runtime_dialog cpu dark", "[screenshot]") {
    requiredRuntimeDialog("dark");
}

TEST_CASE("reference screenshot required_runtime_dialog cpu light", "[screenshot]") {
    requiredRuntimeDialog("light");
}

TEST_CASE("reference screenshot gpu_runtime_dialog rtx-2060 dark", "[screenshot]") {
    gpuRuntimeDialog("dark");
}

TEST_CASE("reference screenshot gpu_runtime_dialog rtx-2060 light", "[screenshot]") {
    gpuRuntimeDialog("light");
}

TEST_CASE("reference screenshot batch_download_dialog two dark", "[screenshot]") {
    batchDialog("dark");
}

TEST_CASE("reference screenshot batch_download_dialog two light", "[screenshot]") {
    batchDialog("light");
}

TEST_CASE("reference screenshot component_details_dialog cuda dark", "[screenshot]") {
    detailsDialog("dark");
}

TEST_CASE("reference screenshot component_details_dialog cuda light", "[screenshot]") {
    detailsDialog("light");
}
