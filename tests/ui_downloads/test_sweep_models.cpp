// Sweep fixes in the Downloads composition (what downloadsPageOptions and the Voice model page
// options supply) and in the Voice model page.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <QApplication>
#include <QLabel>
#include <QString>

#include "models/downloads_model.h"
#include "models/hf_access.h"
#include "models/speech_catalog.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_downloads/downloads_page.h"
#include "ui_downloads/voice_model_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_widgets/wrapped_label.h"
#include "voice_model_support.h"

TEST_CASE("The Downloads usage chip names the model meetings use, and none with remote meeting "
          "speech",
          "[widget]") {
    fvtest::WindowRig rig;
    const fv::DownloadsPageOptions options = fv::downloadsPageOptions(rig.context())();
    REQUIRE(options.meetingModel);

    // Meetings follow a Parakeet dictation model they can run.
    const fv::Json follows{{"selected_model", "parakeet"},
                           {"local_asr_models", {{"parakeet", "orukeet-v0.1"}}}};
    CHECK(options.meetingModel(follows) == "orukeet-v0.1");
    CHECK(fv::models::usage_text("orukeet-v0.1", "parakeet-v3", options.meetingModel(follows)) ==
          "Meetings");

    // A separate meeting engine on this computer.
    const fv::Json own{{"selected_model", "api"},
                       {"meeting_speech_separate", true},
                       {"meeting_asr_source", "local"},
                       {"meeting_asr_model", "orukeet-v0.1"}};
    CHECK(options.meetingModel(own) == "orukeet-v0.1");

    // A paired computer serves meeting speech: no local download is a meeting's.
    fv::Json remote = own;
    remote["meeting_asr_source"] = "remote";
    CHECK(options.meetingModel(remote).empty());
    CHECK(fv::models::usage_text("orukeet-v0.1", "parakeet-v3", options.meetingModel(remote))
              .empty());
}

namespace {

using fvtest::PageRig;

QString q(const char* text) {
    return QString::fromUtf8(text);
}

const fv::Json kParakeetCpu{{"selected_model", "parakeet"},
                            {"local_asr_devices", fv::Json::object({{"parakeet", "cpu"}})}};

} // namespace

TEST_CASE("the Voice model page checks the runtime on the io executor, never on the GUI thread",
          "[widget]") {
    PageRig rig(kParakeetCpu);
    rig.installed = {"asr-nvidia-cpu"};
    fv::VoiceModelPageOptions options = rig.options();
    // A probe that would block the GUI thread (NVML's first answer) counts where it runs.
    int checks = 0;
    const auto inner = options.runtimeLabel;
    options.runtimeLabel = [&checks, inner](const std::string& backend, const std::string& device) {
        ++checks;
        return inner(backend, device);
    };
    fv::VoiceModelPage page(rig.settings, rig.host, rig.client, rig.io, fv::speechEngineCatalog(),
                            options);
    page.refresh(true);
    CHECK(checks == 0);
    CHECK(page.inventoryLabel()->text() == q("0 of 2 Parakeet models on this computer."));

    rig.settle();
    CHECK(checks >= 1);
    CHECK(page.inventoryLabel()->text() ==
          q("0 of 2 Parakeet models on this computer · NVIDIA Speech CPU installed."));

    // A check that throws reads as one that could not be made.
    options.runtimeLabel = [](const std::string&, const std::string&) -> std::string {
        throw std::runtime_error("NVML is not answering");
    };
    fv::VoiceModelPage failing(rig.settings, rig.host, rig.client, rig.io,
                               fv::speechEngineCatalog(), options);
    rig.settle();
    CHECK(failing.inventoryLabel()->text() ==
          q("0 of 2 Parakeet models on this computer. Open Downloads to check its runtime."));
}

TEST_CASE("a new Voice model page starts from the last shared scan, not from 0 of N", "[widget]") {
    PageRig rig(kParakeetCpu);
    fv::VoiceModelPageOptions options = rig.options();
    std::optional<std::vector<std::string>> shared;
    options.peekCachedModels = [&shared] {
        return shared;
    };

    // No window has scanned yet: nothing is known.
    {
        fv::VoiceModelPage page(rig.settings, rig.host, rig.client, rig.io,
                                fv::speechEngineCatalog(), options);
        page.refresh(true);
        CHECK(page.inventoryLabel()->text().startsWith(q("0 of 2 Parakeet models")));
        rig.settle();
    }

    shared = std::vector<std::string>{"parakeet-v3", "orukeet-v0.1"};
    fv::VoiceModelPage page(rig.settings, rig.host, rig.client, rig.io, fv::speechEngineCatalog(),
                            options);
    page.refresh(true);
    CHECK(page.inventoryLabel()->text().startsWith(q("2 of 2 Parakeet models")));

    // The page's own scan decides from then on, even against an older shared one.
    rig.downloaded = {"parakeet-v3"};
    rig.settle();
    CHECK(page.inventoryLabel()->text().startsWith(q("1 of 2 Parakeet models")));
    page.refresh(false);
    CHECK(page.inventoryLabel()->text().startsWith(q("1 of 2 Parakeet models")));
}

TEST_CASE("Settings windows share the controller's model inventory", "[widget]") {
    fvtest::WindowRig rig(kParakeetCpu);
    std::string orukeet = fv::models::artifacts("orukeet-v0.1").at("repo").get<std::string>();
    int walks = 0;
    auto snapshot = std::make_shared<fv::models::CachedModelsSnapshot>([&walks, orukeet] {
        ++walks;
        fv::models::CachedModelInfo info;
        info.repo_id = orukeet;
        return std::vector<fv::models::CachedModelInfo>{info};
    });
    rig.context().provide(snapshot);
    fv::registerVoiceModelPage(rig.context());

    fv::SettingsDialog first(rig.context());
    first.ensurePage("voice_model");
    auto* page = qobject_cast<fv::VoiceModelPage*>(first.pageContent("voice_model"));
    REQUIRE(page != nullptr);
    page->refresh(true);
    rig.io().run_all();
    QApplication::processEvents();
    CHECK(walks == 1);
    CHECK(page->inventoryLabel()->text().startsWith(q("1 of 2 Parakeet models")));

    // A second window shows that scan before its own, and its own reuses it (at most 30 s old).
    fv::SettingsDialog second(rig.context());
    second.ensurePage("voice_model");
    auto* other = qobject_cast<fv::VoiceModelPage*>(second.pageContent("voice_model"));
    REQUIRE(other != nullptr);
    other->refresh(true);
    CHECK(other->inventoryLabel()->text().startsWith(q("1 of 2 Parakeet models")));
    rig.io().run_all();
    QApplication::processEvents();
    CHECK(walks == 1);
    CHECK(other->inventoryLabel()->text().startsWith(q("1 of 2 Parakeet models")));
}
