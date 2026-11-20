#pragma once

// What the Voice model page's tests share: the page over fakes (an in-memory settings store, the
// fake network, a manual io executor, a host that records what the page tells the window), and
// the Settings window around it with the page registered the way DownloadsFeature registers it.

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QString>
#include <QWidget>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "appkit/settings_page.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_downloads/model_assignments.h"
#include "ui_downloads/voice_model_page.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"

namespace fvtest {

// The window around the page, as the page sees it: every call is kept, and the rail is redrawn
// from the settings on demand, as the dialog does.
class RecordingHost final : public fv::VoiceModelPageHost {
public:
    explicit RecordingHost(fv::SettingsStore& settings) : settings_(settings) {}

    void showMessage(const QString& message) override { messages.push_back(message); }
    void notifyChanged(const QString& kind) override { changes.push_back(kind); }
    void refreshRailValues() override {
        ++railRedraws;
        rail = fv::VoiceModelPage::railValue(*settings_.snapshot());
    }
    void focusApiKeys(const QString& env) override { apiKeys.push_back(env); }

    std::vector<QString> messages;
    std::vector<QString> changes;
    std::vector<QString> apiKeys;
    // What the rail shows under Voice model; empty until the first redraw.
    QString rail;
    int railRedraws = 0;

private:
    fv::SettingsStore& settings_;
};

// A host on this computer with nothing but what the test installs.
inline fv::models::Host hostWith(std::set<std::string> installed) {
    fv::models::Host host;
    host.platform = std::string(fv::models::platform::kWinAmd64);
    host.is_installed = [installed = std::move(installed)](std::string_view id) {
        return installed.contains(std::string(id));
    };
    return host;
}

inline std::chrono::system_clock::time_point at(int year, unsigned month, unsigned day) {
    return std::chrono::sys_days{std::chrono::year{year} / month / day} + std::chrono::hours(12);
}

// One page over fakes. Every member a test touches is public.
class PageRig {
public:
    explicit PageRig(const fv::Json& saved = fv::Json::object(),
                     std::chrono::system_clock::time_point now = at(2026, 11, 15))
        : settings(saved), clock(now), host(settings) {
        fv::SpeechServerControls::resetCatalogs();
        client.set_handler([this](const fv::http::Request& request) {
            requested.push_back(request.url);
            fv::http::ScriptedResponse response;
            fv::Json data = fv::Json::array();
            for (const std::string& id : catalogModels) {
                data.push_back(fv::Json{{"id", id}});
            }
            response.body = fv::Json{{"data", data}}.dump();
            return response;
        });
    }
    ~PageRig() { fv::SpeechServerControls::resetCatalogs(); }
    PageRig(const PageRig&) = delete;
    PageRig& operator=(const PageRig&) = delete;

    fv::VoiceModelPageOptions options() {
        fv::VoiceModelPageOptions made;
        made.cachedModels = [this] {
            ++scans;
            if (scanFails) {
                throw std::runtime_error("the models folder cannot be read");
            }
            return std::vector<std::string>(downloaded.begin(), downloaded.end());
        };
        made.runtimeLabel = [this](const std::string& backend, const std::string& device) {
            return fv::engine_runtime_label(backend, device, fvtest::hostWith(installed));
        };
        made.clock = &clock;
        return made;
    }

    std::unique_ptr<fv::VoiceModelPage> page() {
        auto made = std::make_unique<fv::VoiceModelPage>(settings, host, client, io,
                                                         fv::speechEngineCatalog(), options());
        return made;
    }

    // Runs the worker, then delivers its result to the page.
    void settle() {
        io.run_all();
        QApplication::processEvents();
    }

    fv::MemorySettingsStore settings;
    fv::http::FakeHttpClient client;
    fv::ManualExecutor io;
    fv::FakeClock clock;
    RecordingHost host;
    // The model keys on this computer and the runtimes installed on it.
    std::set<std::string> downloaded;
    std::set<std::string> installed;
    std::vector<std::string> catalogModels{"Systran/faster-whisper-small"};
    std::vector<std::string> requested;
    int scans = 0;
    bool scanFails = false;
};

// The per-user folder (components, speech models) moved under the temporary data folder for the
// rig's life: Paths takes it from LOCALAPPDATA or XDG_DATA_HOME, not OPENWHISPER_CPP_DATA_DIR, so
// without this a test would scan, and write into, the owner's real folders.
class ScopedPerUserFolder {
public:
    explicit ScopedPerUserFolder(const std::filesystem::path& root)
        : localAppData_(fv::testkit::get_env("LOCALAPPDATA")),
          xdgDataHome_(fv::testkit::get_env("XDG_DATA_HOME")) {
        const std::filesystem::path folder = root / "per-user";
        std::filesystem::create_directories(folder);
        const auto utf8 = folder.u8string();
        const std::string value(utf8.begin(), utf8.end());
        fv::testkit::set_env("LOCALAPPDATA", value);
        fv::testkit::set_env("XDG_DATA_HOME", value);
    }
    ~ScopedPerUserFolder() {
        fv::testkit::set_env("LOCALAPPDATA", localAppData_);
        fv::testkit::set_env("XDG_DATA_HOME", xdgDataHome_);
    }
    ScopedPerUserFolder(const ScopedPerUserFolder&) = delete;
    ScopedPerUserFolder& operator=(const ScopedPerUserFolder&) = delete;

private:
    std::optional<std::string> localAppData_;
    std::optional<std::string> xdgDataHome_;
};

// The context Bootstrap builds, with what the Voice model page asks of it: a data folder and a
// per-user folder of its own, an in-memory settings file and credential store, the GUI executor,
// a manual io executor and the fake network.
class WindowRig {
public:
    explicit WindowRig(const fv::Json& saved = fv::Json::object())
        : perUser_(root_.path()), paths_(fv::Paths::resolve(fv::identity(), fv::system_env())),
          settings_(saved), ui_(qApp), context_(services()) {
        fv::SpeechServerControls::resetCatalogs();
    }
    ~WindowRig() { fv::SpeechServerControls::resetCatalogs(); }

    fv::AppContext& context() { return context_; }
    fv::MemorySettingsStore& settings() { return settings_; }
    fv::ManualExecutor& io() { return io_; }
    fv::qt::QtUiExecutor& ui() { return ui_; }
    const fv::Paths& paths() const { return paths_; }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        made.io = &io_;
        made.http = &client_;
        made.credentials = &credentials_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    ScopedPerUserFolder perUser_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::ManualExecutor io_;
    fv::http::FakeHttpClient client_;
    // Keys the pages read stay in memory, never in this computer's credential store.
    fv::CredentialStore credentials_{std::make_shared<fv::MemoryCredentialBackend>(),
                                     fv::identity()};
    fv::AppContext context_;
};

inline void choose(QComboBox* combo, const char* data) {
    combo->setCurrentIndex(combo->findData(QString::fromUtf8(data)));
}

} // namespace fvtest
