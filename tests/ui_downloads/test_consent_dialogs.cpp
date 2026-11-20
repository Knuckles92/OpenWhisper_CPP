// The consent and runtime dialogs: tests/test_hf_consent_dialog.py. The model names are the
// speech models of the catalog (Python's tests use Whisper's "base" and "large-v3"); the
// runtimes are asr-nvidia-cpu and asr-nvidia-cuda.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>

#include "history/format_utils.h"
#include "models/consent_text.h"
#include "models/hf_access_policy.h"
#include "ui_downloads/hf_consent_dialog.h"
#include "ui_downloads/required_runtime_dialog.h"
#include "ui_downloads_support.h"

using fv::HuggingFaceConsentDialog;
using fv::models::ConsentAction;
using fv::models::NvidiaGpu;
using fv::models::RuntimeNeeds;
namespace policy = fv::models::hf_access_policy;

namespace {

constexpr char kModel[] = "parakeet-v3";

QString plain(QString text) {
    return text.replace(QChar(0x00A0), QLatin1Char(' '));
}

HuggingFaceConsentDialog makeDialog(std::string_view policyName, bool envBlocked = false,
                                    RuntimeNeeds needs = {}, const char* model = kModel) {
    return HuggingFaceConsentDialog(model, std::string(policyName), envBlocked, std::move(needs));
}

} // namespace

TEST_CASE("ask dialog identifies source model and size", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kAsk);
    const QString body = dialog.bodyText();
    CHECK(body.contains("Parakeet TDT 0.6B v3"));
    CHECK(body.contains("Hugging Face"));
    CHECK(body.contains("nvidia/parakeet-tdt-0.6b-v3"));
    // The bundled estimate shows once, in the title, which never scrolls out of view.
    CHECK(dialog.titleLabel()->text() == "Download \"parakeet-v3\" (about 714 MB)?");
    CHECK_FALSE(body.contains("714 MB"));
    CHECK(dialog.windowTitle() == "Download Speech Model");
}

TEST_CASE("large model title carries its size", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kAsk, false, {}, "orukeet-v0.1");
    CHECK(dialog.titleLabel()->text() == "Download \"orukeet-v0.1\" (about 714 MB)?");
}

TEST_CASE("unknown model omits size estimate", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kAsk, false, {}, "someone/custom-model");
    CHECK_FALSE(dialog.bodyText().contains("Approximate download size"));
    CHECK(dialog.titleLabel()->text() == "Download \"someone/custom-model\" model?");
}

TEST_CASE("never dialog explains policy", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kNever);
    CHECK(dialog.bodyText().contains("Never connect"));
}

TEST_CASE("env blocked dialog explains environment", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kNever, true);
    CHECK(dialog.bodyText().contains("HF_HUB_OFFLINE"));
}

TEST_CASE("ask policy buttons", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kAsk);
    CHECK(fvtest::buttonNamed(dialog, "consentDownloadOnceButton") != nullptr);
    CHECK(fvtest::buttonNamed(dialog, "consentAlwaysAllowButton") != nullptr);
    CHECK(fvtest::buttonNamed(dialog, "consentCancelButton") != nullptr);
    CHECK(fvtest::buttonNamed(dialog, "consentOpenSettingsButton") == nullptr);
}

TEST_CASE("never policy buttons", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kNever);
    CHECK(fvtest::buttonNamed(dialog, "consentDownloadOnceButton") != nullptr);
    CHECK(fvtest::buttonNamed(dialog, "consentOpenSettingsButton") != nullptr);
    CHECK(fvtest::buttonNamed(dialog, "consentCancelButton") != nullptr);
    CHECK(fvtest::buttonNamed(dialog, "consentAlwaysAllowButton") == nullptr);
}

TEST_CASE("env blocked offers no download actions", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kAsk, true);
    CHECK(fvtest::buttonNamed(dialog, "consentDownloadOnceButton") == nullptr);
    CHECK(fvtest::buttonNamed(dialog, "consentAlwaysAllowButton") == nullptr);
    CHECK(fvtest::buttonNamed(dialog, "consentCloseButton") != nullptr);
}

TEST_CASE("each button gives its result", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    struct Case {
        std::string_view policy;
        const char* button;
        ConsentAction result;
    };
    const Case cases[] = {
        {policy::kAsk, "consentDownloadOnceButton", ConsentAction::DownloadOnce},
        {policy::kAsk, "consentAlwaysAllowButton", ConsentAction::AlwaysAllow},
        {policy::kNever, "consentOpenSettingsButton", ConsentAction::OpenSettings},
        {policy::kAsk, "consentCancelButton", ConsentAction::Cancel},
    };
    for (const Case& expected : cases) {
        CAPTURE(expected.button);
        auto dialog = makeDialog(expected.policy);
        fvtest::buttonNamed(dialog, expected.button)->click();
        CHECK(dialog.resultAction() == expected.result);
        CHECK(dialog.result() == (expected.result == ConsentAction::Cancel
                                      ? int(QDialog::Rejected)
                                      : int(QDialog::Accepted)));
    }
}

TEST_CASE("Close of an env-blocked dialog is a cancel", "[widget]") {
    fvtest::AppTheme theme;
    auto dialog = makeDialog(policy::kAsk, true);
    fvtest::buttonNamed(dialog, "consentCloseButton")->click();
    CHECK(dialog.resultAction() == ConsentAction::Cancel);
    CHECK(dialog.result() == QDialog::Rejected);
}

TEST_CASE("cancel is the default and focused button", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    for (const std::string_view policyName : {policy::kAsk, policy::kNever}) {
        auto dialog = makeDialog(policyName);
        CHECK(fvtest::defaultButtons(dialog) == QStringList{"Cancel"});
        CHECK(dialog.focusWidget() == fvtest::buttonNamed(dialog, "consentCancelButton"));
        CHECK_FALSE(fvtest::buttonNamed(dialog, "consentDownloadOnceButton")->autoDefault());
    }
    auto dialog = makeDialog(policy::kAsk);
    CHECK_FALSE(fvtest::buttonNamed(dialog, "consentAlwaysAllowButton")->autoDefault());

    auto blocked = makeDialog(policy::kAsk, true);
    CHECK(fvtest::defaultButtons(blocked) == QStringList{"Close"});
}

TEST_CASE("enter does not start the download", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    {
        auto dialog = makeDialog(policy::kAsk);
        dialog.show();
        QCoreApplication::processEvents();
        QTest::keyClick(&dialog, Qt::Key_Return);
        CHECK(dialog.resultAction() == ConsentAction::Cancel);
        CHECK(dialog.result() == QDialog::Rejected);
    }
    // Not even with the download button focused: it is not auto-default.
    {
        auto dialog = makeDialog(policy::kAsk);
        dialog.show();
        QCoreApplication::processEvents();
        fvtest::buttonNamed(dialog, "consentDownloadOnceButton")->setFocus();
        QTest::keyClick(&dialog, Qt::Key_Enter);
        CHECK(dialog.resultAction() == ConsentAction::Cancel);
        dialog.close();
    }
}

namespace {

// A shown dialog on a screen of the given available height.
std::unique_ptr<HuggingFaceConsentDialog> shownOn(int availableHeight) {
    auto dialog = std::make_unique<HuggingFaceConsentDialog>(kModel, std::string(policy::kAsk));
    dialog->setAvailableHeight([availableHeight] { return availableHeight; });
    dialog->show();
    QCoreApplication::processEvents();
    return dialog;
}

} // namespace

TEST_CASE("a short screen scrolls the notice and keeps the buttons",
          "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = shownOn(400);
    CHECK(dialog->height() <= 400 - HuggingFaceConsentDialog::kScreenMargin);
    QScrollBar* bar = dialog->bodyScroll()->verticalScrollBar();
    CHECK(bar->maximum() > 0);
    CHECK(bar->value() == 0);
    for (const char* name : {"consentCancelButton", "consentDownloadOnceButton"}) {
        QPushButton* button = fvtest::buttonNamed(*dialog, name);
        const int bottom = button->mapTo(dialog.get(), button->rect().bottomLeft()).y();
        CHECK(bottom <= dialog->height());
    }
    CHECK(dialog->titleLabel()->isVisible());
    // Opening it puts the focus on the safe button.
    CHECK(dialog->focusWidget() == dialog->safeButton());
    dialog->close();
}

TEST_CASE("a tall screen shows the whole notice", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    auto dialog = shownOn(2000);
    CHECK(dialog->bodyScroll()->verticalScrollBar()->maximum() == 0);
    CHECK(dialog->width() == HuggingFaceConsentDialog::kWidth);
    dialog->close();
}

TEST_CASE("the notice never shrinks below its minimum height", "[widget]") {
    fvtest::AppTheme theme;
    auto dialog = shownOn(100);
    CHECK(dialog->bodyScroll()->height() >= HuggingFaceConsentDialog::kMinBodyHeight);
    dialog->close();
}

TEST_CASE("optional model consent explains required runtime", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    RuntimeNeeds needs;
    needs.runtime = "asr-nvidia-cpu";
    auto dialog = makeDialog(policy::kAsk, false, needs);
    const QString body = dialog.bodyText();
    CHECK(body.contains("Parakeet TDT 0.6B v3"));
    CHECK(body.contains("NVIDIA Speech CPU is also required"));
    CHECK(body.contains("download alone will not enable transcription"));
}

TEST_CASE("optional model consent names both runtimes when the gpu is offered",
          "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    RuntimeNeeds needs;
    needs.runtime = "asr-nvidia-cpu";
    needs.gpu_runtime = "asr-nvidia-cuda";
    auto dialog = makeDialog(policy::kAsk, false, needs);
    const QString body = dialog.bodyText();
    CHECK(body.contains("NVIDIA Speech GPU for this computer's NVIDIA GPU, or NVIDIA Speech CPU"));
    CHECK(body.contains("asked which runtime to install"));
    CHECK(body.contains("download alone will not enable transcription"));
}

namespace {

fv::GpuRuntimeDialog gpuDialog(const char* gpuComponent, const NvidiaGpu& gpu) {
    return fv::GpuRuntimeDialog(kModel, gpuComponent, "asr-nvidia-cpu", gpu, fvtest::windowsHost());
}

} // namespace

TEST_CASE("gpu runtime offer names the card sizes and choices",
          "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    NvidiaGpu gpu;
    gpu.name = "NVIDIA GeForce RTX 2060";
    gpu.total_mib = 6144;
    gpu.compute_capability = std::pair{7, 5};
    auto dialog = gpuDialog("asr-nvidia-cuda", gpu);
    const QString body = plain(dialog.body()->text());

    CHECK(dialog.titleLabel()->text() == "Run Parakeet on the RTX 2060?");
    CHECK(dialog.titleLabel()->objectName() == "headerLabel");
    CHECK(body.contains("This computer's NVIDIA GeForce RTX 2060 can run it"));
    CHECK(body.contains("NVIDIA Speech GPU runs the model on the RTX 2060 with CUDA"));
    for (const char* component : {"asr-nvidia-cuda", "asr-nvidia-cpu"}) {
        const auto sizes = fv::models::runtime_sizes(component, fvtest::windowsHost());
        CHECK(sizes.download > 0);
        const QString expected =
            QString::fromStdString(fv::format_size_bytes(sizes.download) + " download, " +
                                   fv::format_size_bytes(sizes.on_disk) + " on disk");
        CHECK(body.contains(expected));
    }

    CHECK(fvtest::buttonText(dialog, "Later") != nullptr);
    CHECK(fvtest::buttonText(dialog, "Use the CPU") != nullptr);
    CHECK(fvtest::buttonText(dialog, "Use this GPU") != nullptr);
    CHECK(dialog.findChildren<QPushButton*>().size() == 3);
    // Enter on a prompt that just appeared must not start a download.
    CHECK(fvtest::defaultButtons(dialog) == QStringList{"Later"});
    CHECK_FALSE(dialog.gpuButton()->autoDefault());
    CHECK_FALSE(dialog.cpuButton()->autoDefault());

    dialog.gpuButton()->click();
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK(dialog.choice() == std::optional<std::string>("asr-nvidia-cuda"));
}

TEST_CASE("gpu runtime offer cpu and later answers", "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    NvidiaGpu gtx;
    gtx.name = "NVIDIA GeForce GTX 1050 Ti";
    gtx.total_mib = 4096;
    gtx.compute_capability = std::pair{6, 1};
    {
        auto dialog = gpuDialog("asr-nvidia-vulkan", gtx);
        CHECK(dialog.titleLabel()->text() == "Run Parakeet on the GTX 1050 Ti?");
        // The card's name stays on one line, and Vulkan names its API.
        CHECK(dialog.body()->text().contains(QString::fromUtf8("on the GTX 1050 Ti with Vulkan")));
        dialog.cpuButton()->click();
        CHECK(dialog.result() == QDialog::Accepted);
        CHECK(dialog.choice() == std::optional<std::string>("asr-nvidia-cpu"));
    }
    {
        auto dialog = gpuDialog("asr-nvidia-vulkan", gtx);
        dialog.laterButton()->click();
        CHECK(dialog.result() == QDialog::Rejected);
        CHECK_FALSE(dialog.choice().has_value());
    }
}

TEST_CASE("without a GPU the offer says this GPU", "[widget]") {
    fvtest::AppTheme theme;
    fv::GpuRuntimeDialog dialog(kModel, "asr-nvidia-cuda", "asr-nvidia-cpu", std::nullopt,
                                fvtest::windowsHost());
    CHECK(dialog.titleLabel()->text() == "Run Parakeet on the this GPU?");
    CHECK(dialog.windowTitle() == "Choose a speech runtime");
}

TEST_CASE("runtime prompt has explicit install and later actions",
          "[test_hf_consent_dialog][widget]") {
    fvtest::AppTheme theme;
    {
        fv::RequiredRuntimeDialog dialog(kModel, "asr-nvidia-cpu", fvtest::windowsHost());
        CHECK(dialog.body()->text().contains(
            "needs both its model files and NVIDIA Speech CPU to work"));
        CHECK(dialog.titleLabel()->text() == "NVIDIA Speech CPU is required");
        CHECK(dialog.windowTitle() == "Required speech runtime");
        CHECK(fvtest::defaultButtons(dialog) == QStringList{"Later"});
        CHECK_FALSE(dialog.installButton()->autoDefault());
        CHECK(dialog.installButton()->text() == "Install required runtime");
        dialog.installButton()->click();
        CHECK(dialog.result() == QDialog::Accepted);
    }
    {
        fv::RequiredRuntimeDialog dialog(kModel, "asr-nvidia-cpu", fvtest::windowsHost());
        dialog.laterButton()->click();
        CHECK(dialog.result() == QDialog::Rejected);
    }
}

TEST_CASE("the runtime prompt says how big the runtime download is", "[widget]") {
    fvtest::AppTheme theme;
    fv::RequiredRuntimeDialog dialog(kModel, "asr-nvidia-cpu", fvtest::windowsHost());
    const auto sizes = fv::models::runtime_sizes("asr-nvidia-cpu", fvtest::windowsHost());
    CHECK(sizes.download > 0);
    CHECK(dialog.body()->text().contains(QString::fromStdString(
        "This is a separate " + fv::format_size_bytes(sizes.download) + " download")));
}
