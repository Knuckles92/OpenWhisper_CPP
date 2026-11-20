#pragma once

// The guided setup window over the Settings window of tests/ui_settings (the App pages and
// Recording over fakes, a stand-in for every other page), as OpenWhisper's make_setup fixture
// builds it over a real SettingsDialog: a scripted speech status, recorded destinations and
// downloads, and the microphone test on audio::testing::FakeBackend.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QTimer>

#include "../ui_record/microphone_support.h"
#include "../ui_settings/app_pages_support.h"
#include "settings/hotkeys.h"
#include "ui_setup/guided_setup_dialog.h"

namespace fvtest {

// What make_setup's monkeypatched speech_model_downloaded and speech_model_name answer.
struct ScriptedSpeech {
    std::optional<bool> ready = true;
    std::optional<std::string> model = "parakeet-v3";
};

// A single-shot timer's timeout, now (a test's _try_fill_timer.timeout.emit()).
inline void expire(QTimer* timer) {
    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
}

// _wait_for_queued_refresh: runs the window's zero-delay refresh.
inline bool waitForQueuedRefresh(fv::GuidedSetupDialog& setup) {
    return pumpUntil([&] { return !setup.refreshTimer()->isActive(); },
                     std::chrono::milliseconds(2000));
}

class SetupRig {
public:
    explicit SetupRig(const fv::Json& values = fv::Json::object(), bool welcome = false)
        : pages_(values) {
        ports.settings = [this]() -> fv::SettingsDialog& {
            ++built;
            return pages_.dialog();
        };
        ports.openDestination = [this](const QString& destination) {
            destinations.push_back(destination);
        };
        ports.openAllSettings = [this] {
            ++allSettingsOpened;
        };
        ports.downloadModel = [this](const QString& model) {
            downloads.push_back(model);
        };
        ports.recorder = microphone_.factory();
        ports.speechModelName = [this](const fv::Json&) {
            return speech.model;
        };
        ports.speechModelDownloaded = [this](const fv::Json&) {
            return speech.ready;
        };
        ports.cleanup = fv::CleanupPort::settingsDefault([this](QWidget*, const QString& level) {
            confirmed.push_back(level);
            return confirmAnswer;
        });
        setup_ = std::make_unique<fv::GuidedSetupDialog>(pages_.settings(), ports);
        setup_->begin(welcome);
    }

    ~SetupRig() {
        setup_->hide();
        setup_.reset();
        flush();
    }

    SetupRig(const SetupRig&) = delete;
    SetupRig& operator=(const SetupRig&) = delete;

    fv::GuidedSetupDialog& setup() { return *setup_; }
    // The Settings window, or null when setup has not built it.
    fv::SettingsDialog* settings() { return setup_->settingsDialog(); }
    AppPagesRig& pages() { return pages_; }
    fv::MemorySettingsStore& store() { return pages_.settings(); }
    MicrophoneRig& microphone() { return microphone_; }
    fv::Json all() { return *store().snapshot(); }
    fv::Json saved(std::string_view key) { return pages_.saved(key); }
    fv::Json hotkeys() { return fv::load_hotkey_settings(store()); }

    // Rebuilds the window with the same ports (after a test changed them).
    void rebuild(bool welcome = false) {
        setup_ = std::make_unique<fv::GuidedSetupDialog>(pages_.settings(), ports);
        setup_->begin(welcome);
    }

    ScriptedSpeech speech;
    fv::GuidedSetupPorts ports;
    int built = 0;
    int allSettingsOpened = 0;
    std::vector<QString> destinations;
    std::vector<QString> downloads;
    std::vector<QString> confirmed;
    bool confirmAnswer = false;

private:
    MicrophoneRig microphone_;
    AppPagesRig pages_;
    std::unique_ptr<fv::GuidedSetupDialog> setup_;
};

} // namespace fvtest
