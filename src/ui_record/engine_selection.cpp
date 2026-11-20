#include "ui_record/engine_selection.h"

#include <algorithm>
#include <array>

#include "settings/store.h"
#include "ui_record/transcription_tab_base.h"

namespace fv {
namespace {

// ENGINE_KEYS without the four Whisper keys (whisper_model, whisper_device,
// whisper_compute_type, custom_whisper_models), which FioriVoce does not have.
constexpr std::array<std::string_view, 9> kEngineKeys{
    "selected_model",
    "local_asr_models",
    "local_asr_devices",
    "local_asr_language",
    "api_transcription_endpoint",
    "api_transcription_servers",
    "api_transcription_model",
    // The Language field steps aside while a dictation language is in charge.
    "dictation_languages",
    "dictation_active_language",
};

} // namespace

std::span<const std::string_view> engineKeys() {
    return kEngineKeys;
}

EngineSelection::EngineSelection(SettingsStore& settings, Executor& ui) {
    subscription_ = settings.watch({kEngineKeys.begin(), kEngineKeys.end()}, ui,
                                   [this](const Json&) { changed(); });
}

void EngineSelection::add(TranscriptionTabBase* tab) {
    if (tab != nullptr) {
        follow(tab, [tab] { tab->syncEngineSelection(); });
    }
}

void EngineSelection::follow(QObject* receiver, std::function<void()> reload) {
    if (receiver != nullptr && reload) {
        followers_.push_back({receiver, std::move(reload)});
    }
}

std::vector<TranscriptionTabBase*> EngineSelection::views() const {
    std::vector<TranscriptionTabBase*> alive;
    for (const Follower& follower : followers_) {
        if (auto* tab = qobject_cast<TranscriptionTabBase*>(follower.receiver.data())) {
            alive.push_back(tab);
        }
    }
    return alive;
}

void EngineSelection::changed() {
    std::erase_if(followers_,
                  [](const Follower& follower) { return follower.receiver == nullptr; });
    // A view reloading may save (a pre-2.0 backend name is rewritten), which is heard again and
    // may add or delete views, so it works on a copy and checks each view is still alive.
    const std::vector<Follower> snapshot = followers_;
    for (const Follower& follower : snapshot) {
        if (follower.receiver != nullptr) {
            follower.reload();
        }
    }
}

} // namespace fv
