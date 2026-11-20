#pragma once

#include <functional>
#include <string_view>

#include "appkit/feature.h"
#include "ui_downloads/downloads_page.h"
#include "ui_downloads/voice_model_page.h"

namespace fv {

// Registers the Voice model page (destination voice_model) in the context's Settings pages.
// `options` supplies what each page built reads of this computer (called once per page); unset,
// the app's own folders: the speech models under Paths::speech_models_root, the runtimes under
// Paths::components_root, and the credential store for the API key. The page follows the engine
// saved from other views through RecordWidgets when the context has them. Tests register the
// page over fakes by passing their own.
// Affinity: UI actor.
void registerVoiceModelPage(AppContext& context,
                            std::function<VoiceModelPageOptions()> options = {});

class DownloadsPresenter;

// What a Downloads page reads of this computer in the app: the shared inventory
// (CachedModelsSnapshot) and component coordinator when the controller provides them, else ones
// made here over Paths::speech_models_root and Paths::components_root; the loaded model through
// ModelRuntime; the speech models folder. Made on first use, so registering costs nothing.
// Affinity: UI actor.
std::function<DownloadsPageOptions()> downloadsPageOptions(AppContext& context);

// Registers the Downloads page (destination downloads, "Downloads", its own scrolling) with the
// "When a model is missing from this computer" policy row. The page is the presenter's: the one
// made, deferred, with the Settings window, or a new one. Its rail value is the page's
// (railValue), redrawn when the inventory changes and, during a download, once per percent.
// Affinity: UI actor.
void registerDownloadsPage(AppContext& context, DownloadsPresenter& presenter);

// The Downloads rows, dialogs, pages and presenter live in this library. provide() makes the
// DownloadsPresenter; install() registers the Voice model and Downloads pages and attaches the
// presenter to the model and component runtimes, the Settings window and the record widgets
// the other features provided (each only when provided).
// Affinity: UI actor.
class DownloadsFeature final : public Feature {
public:
    static constexpr std::string_view kId = "downloads";

    std::string_view id() const override { return kId; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

} // namespace fv
