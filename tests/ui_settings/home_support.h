#pragma once

// Settings > Home and the search palette over fakes: the App pages and Recording (AppPages with
// fake hooks, as app_pages_support.h has them), Home, the Voice model page registered as
// DownloadsFeature registers it (over the fake network and a manual io executor), a Downloads
// stand-in that records its deep links (the real page is M2.23's), and a stand-in for every
// other destination. HomeSources reads the Voice model page's own functions; the storage and
// the components are the test's.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
#include <QVBoxLayout>

#include "app_pages_support.h"
#include "appkit/app_context.h"
#include "base/testing/manual_executor.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/downloads_model.h"
#include "settings_support.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_downloads/model_assignments.h"
#include "ui_settings/home_page.h"
#include "ui_settings/home_rows.h"
#include "ui_settings/settings_search_palette.h"

namespace fvtest {

// The Downloads stand-in: the page's own container with the stand-in tiles, and the deep links
// it was asked for.
class DownloadsStandIn : public QWidget,
                         public fv::SettingsPageModule,
                         public fv::SettingsDeepLinks {
public:
    DownloadsStandIn(QStringList& calls, QWidget* parent) : QWidget(parent), calls_(calls) {
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(new fv::InfoTile(QStringLiteral("First tile"),
                                           QStringLiteral("A tile of the page that registers "
                                                          "here.")));
    }
    void showBackend(const QString& backend) override { calls_.append("backend " + backend); }
    void focusComponent(const QString& id) override { calls_.append("component " + id); }
    void revealModel(const QString& model) override { calls_.append("model " + model); }

private:
    QStringList& calls_;
};

// The components a test's computer has: the CPU runtime installed, the CUDA one not.
inline std::vector<fv::models::ComponentInfo> testComponents() {
    fv::models::ComponentInfo cpu;
    cpu.component_id = "asr-nvidia-cpu";
    cpu.display_name = "NVIDIA speech runtime (CPU)";
    cpu.summary = "Runs Parakeet on the processor";
    cpu.state = fv::models::ComponentState::Installed;
    fv::models::ComponentInfo cuda;
    cuda.component_id = "asr-nvidia-cuda";
    cuda.display_name = "NVIDIA speech runtime (CUDA)";
    cuda.summary = "Runs Parakeet on an NVIDIA GPU";
    cuda.state = fv::models::ComponentState::NotInstalled;
    return {cpu, cuda};
}

class HomeRig {
public:
    // unregistered: destinations that get no stand-in (a build without those pages).
    explicit HomeRig(const fv::Json& saved = fv::Json::object(),
                     std::vector<std::string_view> unregistered = {})
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), settings_(saved), ui_(qApp),
          backend_(std::make_shared<fv::audio::testing::FakeBackend>()), pool_("home-test", 3),
          credentialBackend_(std::make_shared<fv::MemoryCredentialBackend>()),
          store_(credentialBackend_, fv::identity()), context_(services()),
          dotenv_(context_.paths().env_file(), env_), pages_(context_) {
        loadS7Devices(*backend_);
        fv::AppPagesHooks& hooks = pages_.hooks();
        hooks.trayAvailable = [this] {
            return trayAvailable;
        };
        hooks.meetingActive = [] {
            return false;
        };
        hooks.playbackPlaying = [] {
            return false;
        };
        hooks.listDevices = [this] {
            ++listings;
            return fv::audio::list_input_devices(*backend_);
        };
        hooks.refreshDevices = [this] {
            return backend_->refresh();
        };
        hooks.credentials = &store_;
        hooks.env = &env_;
        hooks.dotenv = &dotenv_;
        hooks.verifyKey = [](const fv::speech::SpeechEndpoint&, const std::string&) {
            return std::pair<bool, std::string>(true, "accepted");
        };
        hooks.confirm = [](QWidget*, const QString&, const QString&) {
            return false;
        };
        hooks.dialog = [this]() -> fv::SettingsDialog* {
            return current_;
        };
        hooks.worker = &pool_;
        hooks.previewRecorder = [](const fv::Json&) -> std::unique_ptr<fv::Recorder> {
            return nullptr;
        };
        clock_.advance(std::chrono::seconds(1000));
        hooks.clock = &clock_;
        pages_.registerPages();
        // As SettingsFeature provides it: Home reads the tray hook through the context. The rig
        // owns the pages, so the context's handle does not.
        context_.provide<fv::AppPages>(
            std::shared_ptr<fv::AppPages>(&pages_, [](fv::AppPages*) {}));
        fv::registerHomePage(context_);
        fv::registerVoiceModelPage(context_, [] {
            fv::VoiceModelPageOptions options;
            options.cachedModels = [] {
                return std::vector<std::string>{"parakeet-v3"};
            };
            return options;
        });
        http_.set_handler([](const fv::http::Request&) {
            fv::http::ScriptedResponse response;
            response.body = R"({"data": []})";
            return response;
        });
        for (const Heading& heading : headings()) {
            if (context_.settingsPages().page(heading.key) != nullptr ||
                std::ranges::find(unregistered, heading.key) != unregistered.end()) {
                continue;
            }
            fv::SettingsPageSpec spec = standInSpec(heading);
            if (heading.key == dest::downloads) {
                spec.create = [this](QWidget* parent) -> QWidget* {
                    return new DownloadsStandIn(downloadCalls, parent);
                };
            }
            context_.settingsPages().addPage(std::move(spec));
        }
        fv::HomeSources& sources = context_.service<fv::HomeSources>();
        sources.voiceDetail = [](const fv::Json& settings) {
            return fv::voice_detail(settings);
        };
        sources.voiceIsRemote = [](const fv::Json& settings) {
            return fv::voice_is_remote(settings);
        };
        sources.voiceIsCloud = [](const fv::Json& settings) {
            return fv::voice_is_cloud(settings);
        };
        sources.runtimeSummary = [](const fv::Json& settings) {
            return fv::runtime_summary(settings, false);
        };
        sources.storage = [this] {
            return storage;
        };
        sources.components = [this] {
            return components;
        };
    }

    ~HomeRig() {
        current_ = nullptr;
        dialogs_.clear();
        pool_.shutdown(std::chrono::seconds(5));
        QCoreApplication::processEvents();
    }
    HomeRig(const HomeRig&) = delete;
    HomeRig& operator=(const HomeRig&) = delete;

    fv::SettingsDialog& dialog() {
        if (current_ == nullptr) {
            auto made = std::make_unique<fv::SettingsDialog>(context_);
            made->availableScreenRect = [] {
                return QRect(0, 0, 1920, 1080);
            };
            current_ = made.get();
            dialogs_.push_back(std::move(made));
        }
        return *current_;
    }
    // A second window over the same settings (closing the first).
    fv::SettingsDialog& reopen() {
        current_ = nullptr;
        dialogs_.clear();
        return dialog();
    }

    fv::HomePage& home() {
        auto* page = dynamic_cast<fv::HomePage*>(dialog().pageContent(dest::overview));
        REQUIRE(page != nullptr);
        return *page;
    }
    fv::SearchPalette& palette() {
        auto* found = dialog().findChild<fv::SearchPalette*>();
        REQUIRE(found != nullptr);
        return *found;
    }

    fv::AppContext& context() { return context_; }
    fv::MemorySettingsStore& settings() { return settings_; }
    fv::SettingsPagesSignals& signals() { return pages_.signals(); }
    fv::HomeRows& homeRows() { return context_.service<fv::HomeRows>(); }
    fv::ManualExecutor& io() { return io_; }
    fv::audio::testing::FakeBackend& backend() { return *backend_; }
    fv::Json saved(std::string_view key) {
        const auto snapshot = settings_.snapshot();
        const auto found = snapshot->find(key);
        return found == snapshot->end() ? fv::Json() : *found;
    }
    bool has(std::string_view key) { return settings_.snapshot()->contains(key); }

    // Every registered destination built now.
    std::vector<std::string> builtPages() {
        std::vector<std::string> built;
        for (const std::string& key : dialog().pageKeys()) {
            if (dialog().isBuilt(key)) {
                built.push_back(key);
            }
        }
        return built;
    }

    bool trayAvailable = true;
    std::atomic<int> listings{0};
    QStringList downloadCalls;
    // What the Downloads inventory reports (nullopt: still reading) and the components.
    std::optional<fv::models::StorageSummary> storage = fv::models::StorageSummary{};
    std::vector<fv::models::ComponentInfo> components = testComponents();

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.io = &io_;
        made.http = &http_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::ManualExecutor io_;
    fv::http::FakeHttpClient http_;
    fv::FakeClock clock_;
    std::shared_ptr<fv::audio::testing::FakeBackend> backend_;
    fv::ThreadPool pool_;
    std::shared_ptr<fv::MemoryCredentialBackend> credentialBackend_;
    fv::CredentialStore store_;
    fv::MapEnvReader env_;
    fv::AppContext context_;
    fv::DotEnv dotenv_;
    fv::AppPages pages_;
    fv::SettingsDialog* current_ = nullptr;
    std::vector<std::unique_ptr<fv::SettingsDialog>> dialogs_;
};

} // namespace fvtest
