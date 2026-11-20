#include "ui_downloads/downloads_feature.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/registries.h"
#include "appkit/settings_page.h"
#include "base/env.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "credentials/dotenv.h"
#include "models/catalog.h"
#include "models/component_details.h"
#include "models/component_runtime.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/downloads_model.h"
#include "models/hf_access.h"
#include "models/meeting_speech.h"
#include "models/model_runtime.h"
#include "models/speaker_model.h"
#include "models/speech_catalog.h"
#include "models/speech_model_cache.h"
#include "remote/connection.h"
#include "remote/meeting_route.h"
#include "remote/pairing_settings.h"
#include "remote_host/service.h"
#include "settings/store.h"
#include "speech/endpoints.h"
#include "ui_downloads/downloads_presenter.h"
#include "ui_downloads/hf_policy_field.h"
#include "ui_downloads/meeting_voice_page.h"
#include "ui_downloads/model_assignments.h"
#include "ui_downloads/voice_model_page.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/record_feature.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_window.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/no_wheel.h"

namespace fv {
namespace {

static_assert(DownloadsFeature::kId == "downloads");

// The Settings window as the Voice model page sees it. Owned by the page's container, so it goes
// with the window.
class DialogHost final : public QObject, public VoiceModelPageHost {
public:
    DialogHost(SettingsDialog* dialog, QObject* parent) : QObject(parent), dialog_(dialog) {}

    void showMessage(const QString& message) override {
        if (dialog_ != nullptr) {
            dialog_->showMessage(message);
        }
    }
    void notifyChanged(const QString& kind) override {
        if (dialog_ != nullptr) {
            dialog_->notifyChanged(kind);
        }
    }
    void refreshRailValues() override {
        if (dialog_ != nullptr) {
            dialog_->refreshRailValues();
        }
    }
    void focusApiKeys(const QString& env) override {
        if (dialog_ != nullptr) {
            dialog_->focusApiKeys(env);
        }
    }

private:
    QPointer<SettingsDialog> dialog_;
};

// The catalog keys of the models an inventory holds (it is keyed by repository).
std::vector<std::string> modelKeys(const models::CachedModelsSnapshot::Inventory& inventory) {
    std::vector<std::string> keys;
    for (const models::SpeechModel& model : models::models()) {
        try {
            if (inventory.contains(models::artifacts(model.key).at("repo").get<std::string>())) {
                keys.emplace_back(model.key);
            }
        } catch (const std::exception&) {
            // A model without pins is never downloaded.
        }
    }
    return keys;
}

// What this computer has, read from the folders the app installs into. The context outlives
// every page.
VoiceModelPageOptions pageOptions(AppContext& context) {
    VoiceModelPageOptions options;
    // The inventory every Settings window and the Downloads page share, when the controller
    // provides it: a scan at most 30 s old is reused (scan_cached_models(max_age_seconds=30)),
    // and a new window starts from the last one.
    options.peekCachedModels = [&context]() -> std::optional<std::vector<std::string>> {
        if (auto* snapshot = context.find<models::CachedModelsSnapshot>()) {
            if (const auto inventory = snapshot->peek()) {
                return modelKeys(*inventory);
            }
        }
        return std::nullopt;
    };
    options.cachedModels = [&context] {
        if (auto* snapshot = context.find<models::CachedModelsSnapshot>()) {
            return modelKeys(snapshot->scan(30.0));
        }
        // The models SpeechModelCache::inventory lists, by key: the folder holds the pinned files.
        const models::SpeechModelCache cache(context.paths().speech_models_root(), context.http());
        std::vector<std::string> cached;
        for (const models::SpeechModel& model : models::models()) {
            if (cache.is_cached(model.key)) {
                cached.emplace_back(model.key);
            }
        }
        return cached;
    };
    // On the io executor: with device Auto, the host's GPU answer can be the process's first
    // NVML probe.
    options.runtimeLabel = [&context](const std::string& backend, const std::string& device) {
        const std::filesystem::path root = context.paths().components_root();
        models::Host host = models::Host::current([root](std::string_view id) {
            std::error_code error;
            return std::filesystem::is_regular_file(
                root / base_utf8::path_from_utf8(id) / ".installed", error);
        });
        // What the engine will load: a runtime build or folder the user chose counts.
        return engine_runtime_label(*context.settings().snapshot(), backend, device,
                                    std::move(host));
    };
    // The key the server's catalog is asked with: the saved one, the environment's, or the .env
    // file's, read on the io executor because the credential store blocks.
    const auto dotenv = std::make_shared<DotEnv>(context.paths().env_file(), system_env());
    options.apiKey = [&context, dotenv](const speech::SpeechEndpoint& endpoint) {
        return speech::api_key(endpoint, context.credentials(), system_env(), *dotenv);
    };
    return options;
}

} // namespace

void registerVoiceModelPage(AppContext& context, std::function<VoiceModelPageOptions()> options) {
    SettingsPageSpec spec;
    spec.destination = destinations::voice_model;
    spec.title = QStringLiteral("Voice model");
    spec.subtitle = QStringLiteral(
        "The transcription engine used by Quick Record, hotkey dictation, and Upload File.");
    spec.create = [&context, options = std::move(options)](QWidget* parent) -> QWidget* {
        auto* container = qobject_cast<SettingsPage*>(parent);
        SettingsDialog* dialog = container != nullptr ? container->dialog() : nullptr;
        auto* host = new DialogHost(dialog, parent);
        auto* page = new VoiceModelPage(context.settings(), *host, context.http(), context.io(),
                                        speechEngineCatalog(),
                                        options ? options() : pageOptions(context), parent);
        if (RecordWidgets* record = context.find<RecordWidgets>()) {
            // The engine is also changed from Quick Record, Upload File, the controller and a
            // paired computer.
            record->engineSelection().follow(page, [page] { page->refreshEngineSelection(); });
        }
        if (dialog != nullptr) {
            QObject::connect(page, &VoiceModelPage::downloadsRequested, dialog,
                             [dialog](const QString& filter) { dialog->showDownloads(filter); });
        }
        return page;
    };
    spec.railValue = [](const Json& settings) {
        return VoiceModelPage::railValue(settings);
    };
    // PAGE_SEARCH_FIELDS['voice_model']: OpenWhisper's first field is its Whisper model picker;
    // the Model field of the local engine row stands in.
    spec.searchFields = {
        {"speech_controls", QStringLiteral("Model"), QString()},
        {"api_server", QStringLiteral("API server and model"),
         QStringLiteral("Where the API engine sends audio: OpenAI, Groq, Speaches, LocalAI, vLLM, "
                        "whisper.cpp or a custom OpenAI-compatible server.")},
        {"engine_combo", QStringLiteral("Recording engine"), QString()},
    };
    spec.helpTexts = {QString::fromUtf8(kVoiceFootnote.data(), kVoiceFootnote.size())};
    spec.controls = VoiceModelPage::controlNames();
    context.settingsPages().addPage(std::move(spec));
}

namespace {

// What the Voice & speakers page reads of this computer: the speaker model under the app's
// folders (and the variables naming one), the Hugging Face policy, and the verified download.
MeetingVoicePageOptions meetingVoiceOptions(AppContext& context) {
    MeetingVoicePageOptions options;
    if (auto* service = context.find<remote_host::RemoteEngineService>()) {
        options.remoteMeetingSupported = true;
        options.testRemoteMeeting = [weak = service->weak_from_this()](const Json& settings) {
            const auto host = weak.lock();
            if (!host) {
                throw std::runtime_error("Remote speech is unavailable.");
            }
            const auto route = remote::meeting_route(settings);
            const auto pairing = remote::load_client_pairing(settings);
            auto connection = host->client_connection(pairing, false, false);
            const auto ready = remote::checked_meeting_engine(route, connection->connect());
            return ready.at("host_name").get<std::string>() + " is ready for meeting speech.";
        };
    }
    options.speakerModelReady = [&context] {
        return models::speaker_model_path(context.paths(), system_env()).has_value();
    };
    options.speakerDownloadBlocked = [&context] {
        return models::speaker_model_download_blocked(*context.settings().snapshot(), system_env());
    };
    options.speakerModelError = [] {
        return models::speaker_model_error();
    };
    options.ensureSpeakerModel = [&context] {
        models::SpeakerModelDeps deps(context.paths(), system_env(), context.settings(),
                                      context.http());
        models::ensure_speaker_model(deps);
    };
    options.openRemoteEngine = [&context] {
        if (SettingsWindow* window = context.find<SettingsWindow>()) {
            if (SettingsDialog* dialog = window->dialog()) {
                dialog->selectDestination(QString::fromUtf8(destinations::remote_engine.data(),
                                                            destinations::remote_engine.size()));
            }
        }
    };
    return options;
}

} // namespace

void registerMeetingVoicePage(AppContext& context,
                              std::function<MeetingVoicePageOptions()> options) {
    SettingsPageSpec spec;
    spec.destination = destinations::meeting_voice;
    spec.title = QStringLiteral("Voice & speakers");
    spec.subtitle = QStringLiteral(
        "The speech engine for live captions and the optional end-of-meeting re-transcription: "
        "dictation's, or one of its own. Applies to your next meeting.");
    spec.create = [&context, options = std::move(options)](QWidget* parent) -> QWidget* {
        auto* container = qobject_cast<SettingsPage*>(parent);
        SettingsDialog* dialog = container != nullptr ? container->dialog() : nullptr;
        auto* host = new DialogHost(dialog, parent);
        auto* page =
            new MeetingVoicePage(context.settings(), *host, context.io(),
                                 options ? options() : meetingVoiceOptions(context), parent);
        if (dialog != nullptr) {
            QObject::connect(page, &MeetingVoicePage::downloadsRequested, dialog,
                             [dialog](const QString& filter) { dialog->showDownloads(filter); });
        }
        return page;
    };
    spec.railValue = [](const Json& settings) {
        return MeetingVoicePage::railValue(settings);
    };
    // PAGE_SEARCH_FIELDS['meeting_voice'].
    spec.searchFields = {
        {"meeting_whisper_picker", QStringLiteral("Meeting speech model"), QString()},
        {"meeting_engine_combo", QStringLiteral("Speech engine"),
         QStringLiteral("Meetings use the dictation engine, or one of their own.")},
        {"meeting_source_combo", QStringLiteral("Runs on"), QString()},
        {"meeting_language_combo", QStringLiteral("Spoken language"), QString()},
        {"meeting_speaker_id_combo", QStringLiteral("Speaker identification"), QString()},
    };
    spec.controls = MeetingVoicePage::controlNames();
    context.settingsPages().addPage(std::move(spec));
}

namespace {

// What the Downloads page reads when the controller provides no shared instances: made on first
// use and kept for the session.
struct LocalModelServices {
    explicit LocalModelServices(AppContext& context)
        : cache(context.paths().speech_models_root(), context.http()), snapshot(cache),
          store(context.paths().components_root(), context.http()),
          coordinator(store, coordinatorOptions()) {}

    static models::ComponentCoordinator::Options coordinatorOptions() {
        models::ComponentCoordinator::Options options;
        options.copy_for = &models::component_copy;
        return options;
    }

    models::SpeechModelCache cache;
    models::CachedModelsSnapshot snapshot;
    models::ComponentStore store;
    models::ComponentCoordinator coordinator;
};

} // namespace

std::function<DownloadsPageOptions()> downloadsPageOptions(AppContext& context) {
    auto local = std::make_shared<std::shared_ptr<LocalModelServices>>();
    const auto services = [&context, local]() -> LocalModelServices& {
        if (*local == nullptr) {
            *local = std::make_shared<LocalModelServices>(context);
        }
        return **local;
    };
    return [&context, services] {
        models::CachedModelsSnapshot* snapshot = context.find<models::CachedModelsSnapshot>();
        if (snapshot == nullptr) {
            snapshot = &services().snapshot;
        }
        models::ComponentCoordinator* coordinator = context.find<models::ComponentCoordinator>();
        if (coordinator == nullptr) {
            coordinator = &services().coordinator;
        }
        DownloadsPageOptions options;
        options.peekInventory = [snapshot] {
            return snapshot->peek();
        };
        options.scanInventory = [snapshot] {
            return snapshot->scan(30.0);
        };
        options.listComponents = [coordinator] {
            return coordinator->list_components();
        };
        options.describeComponent = [coordinator](const std::string& id) {
            return coordinator->describe(id);
        };
        options.isInstalling = [coordinator](const std::string& id) {
            return coordinator->is_installing(id);
        };
        // The runtime knows the loaded model from the controller, never from the engine itself.
        options.loadedModel = [&context]() -> std::optional<std::string> {
            if (models::ModelRuntime* runtime = context.find<models::ModelRuntime>()) {
                return runtime->loaded_local_model();
            }
            return std::nullopt;
        };
        // The Usage chip's "Meetings" half.
        options.meetingModel = [](const Json& settings) {
            return models::meeting_model_in_use(settings);
        };
        options.modelsFolder =
            QString::fromStdString(base_utf8::path_to_utf8(context.paths().speech_models_root()));
        return options;
    };
}

void registerDownloadsPage(AppContext& context, DownloadsPresenter& presenter) {
    SettingsPageSpec spec;
    spec.destination = destinations::downloads;
    spec.title = QStringLiteral("Downloads");
    spec.subtitle = QStringLiteral("Speech models and optional components, downloaded on demand.");
    // Only the catalog, the profile and the components scroll; the page itself never does.
    spec.scroll = false;
    QPointer<DownloadsPresenter> owner(&presenter);
    spec.create = [&context, owner](QWidget* parent) -> QWidget* {
        auto* container = qobject_cast<SettingsPage*>(parent);
        SettingsDialog* dialog = container != nullptr ? container->dialog() : nullptr;
        DownloadsPage* page = nullptr;
        if (owner != nullptr) {
            page = owner->pageFor(dialog, parent);
        } else {
            page = new DownloadsPage(context.settings(), context.io(), {}, false, parent);
        }
        QPointer<SettingsDialog> window(dialog);
        HfPolicyField::Persist persist = HfPolicyField::storeWriter(context.settings());
        if (dialog != nullptr) {
            persist = [window](const Json& updates, std::vector<std::string> drops) {
                return window != nullptr && window->persistMany(updates, std::move(drops));
            };
        }
        auto* field = new HfPolicyField(std::move(persist));
        page->addPolicyControl(QStringLiteral("When a model is missing from this computer"),
                               field->combo());
        page->setPolicyControl(field->combo());
        page->setPolicyLoader([field, &context](const Json&) { field->load(context.settings()); });
        if (owner != nullptr) {
            QObject::connect(
                field, &HfPolicyField::policyChanged, owner, [owner](const QString& policy) {
                    if (owner != nullptr && owner->parts().models != nullptr) {
                        owner->parts().models->on_hf_policy_changed(policy.toStdString());
                    }
                });
        }
        if (container != nullptr) {
            container->pageLayout()->addWidget(page, 1);
        }
        page->refresh(false);
        return page;
    };
    spec.railValue = [&context, owner](const Json&) {
        if (owner != nullptr && owner->page() != nullptr) {
            return owner->page()->railValue();
        }
        // No Settings window has made its page yet: the last shared scan, if any.
        const DownloadsPageOptions options =
            owner != nullptr ? owner->pageOptions() : downloadsPageOptions(context)();
        const auto inventory = options.peekInventory ? options.peekInventory() : std::nullopt;
        return QString::fromStdString(
            models::downloads_rail_value(std::nullopt, std::nullopt, false,
                                         models::storage_summary(inventory.value_or(
                                             models::CachedModelsSnapshot::Inventory{}))));
    };
    spec.controls = {"hf_policy_combo"};
    context.settingsPages().addPage(std::move(spec));
}

void DownloadsFeature::provide(AppContext& context) {
    if (context.find<DownloadsPresenter>() == nullptr) {
        context.provide(
            std::make_shared<DownloadsPresenter>(context.settings(), context.ui(), context.io()));
    }
}

void DownloadsFeature::install(AppContext& context) {
    provide(context);
    DownloadsPresenter& presenter = context.service<DownloadsPresenter>();
    presenter.setPageOptions(downloadsPageOptions(context));
    presenter.dialogParent = [&context]() -> QWidget* {
        return context.find<MainWindow>();
    };
    presenter.host = [&context] {
        if (auto* coordinator = context.find<models::ComponentCoordinator>()) {
            return coordinator->store().host();
        }
        const std::filesystem::path root = context.paths().components_root();
        return models::Host::current([root](std::string_view id) {
            std::error_code error;
            return std::filesystem::is_regular_file(
                root / base_utf8::path_from_utf8(id) / ".installed", error);
        });
    };
    registerVoiceModelPage(context);
    registerMeetingVoicePage(context);
    registerDownloadsPage(context, presenter);
    DownloadsPresenterParts parts;
    parts.models = context.find<models::ModelRuntime>();
    parts.components = context.find<models::ComponentRuntime>();
    parts.settingsWindow = context.find<SettingsWindow>();
    parts.record = context.find<RecordWidgets>();
    presenter.attach(parts);
}

} // namespace fv
