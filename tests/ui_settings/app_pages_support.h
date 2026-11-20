#pragma once

// The App pages and Recording inside the Settings window, over fakes (OpenWhisper's
// tests/fakes/personalize.settings_dialogs and the fixtures of the page tests): an in-memory
// settings file, audio::FakeBackend's device list, an in-memory credential store with an
// environment of its own, a real worker pool, and History over a temporary database and
// recordings folder. Every other destination is a stand-in page.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>

#include <catch2/catch_test_macros.hpp>

#include "audio/devices.h"
#include "audio/testing/fake_backend.h"
#include "base/executor.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "db/database.h"
#include "history/manager.h"
#include "recorder/recorder.h"
#include "settings_support.h"
#include "speech/endpoints.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"

namespace fvtest {

// Runs fn on a thread of its own and returns what it returned: History blocks on its database,
// which a debug build refuses on the GUI thread.
template <class Fn> auto offGui(Fn fn) {
    using Result = std::invoke_result_t<Fn&>;
    if constexpr (std::is_void_v<Result>) {
        std::thread worker(fn);
        worker.join();
    } else {
        std::optional<Result> result;
        std::thread worker([&] { result.emplace(fn()); });
        worker.join();
        return std::move(*result);
    }
}

inline const std::string kMme = "MME";
inline const std::string kDsound = "Windows DirectSound";

// test_personalize_s7_settings.py's devices: the Sound Mapper, Blue Snowball, the USB microphone
// whose MME name is cut at 31 characters, the webcam, and the USB microphone and webcam again
// under DirectSound.
inline void loadS7Devices(fv::audio::testing::FakeBackend& backend) {
    using fv::audio::testing::FakeDevice;
    using fv::audio::testing::FakeHostApi;
    backend.set_layout({FakeHostApi{kMme, 0, -1}, FakeHostApi{kDsound, 4, -1}},
                       {
                           FakeDevice{"Microsoft Sound Mapper - Input", 0, 2, 0, 44100.0},
                           FakeDevice{"Microphone (Blue Snowball )", 0, 1, 0, 44100.0},
                           FakeDevice{"Microphone (2- USB Audio Device", 0, 1, 0, 44100.0},
                           FakeDevice{"Microphone (Webcam)", 0, 1, 0, 44100.0},
                           FakeDevice{"Microphone (2- USB Audio Device)", 1, 1, 0, 44100.0},
                           FakeDevice{"Microphone (Webcam)", 1, 1, 0, 44100.0},
                       },
                       0, 0);
}

inline fv::Json entry(const std::string& name, const std::string& hostapi = kMme) {
    return fv::Json{{"name", name}, {"hostapi", hostapi}};
}

inline const fv::Json kUsb = entry("Microphone (2- USB Audio Device");
inline const fv::Json kSnowball = entry("Microphone (Blue Snowball )");
inline const fv::Json kLapel = entry("Microphone (Lapel)");
inline const fv::Json kWebcam = entry("Microphone (Webcam)");
inline const fv::Json kDsoundUsb = entry("Microphone (2- USB Audio Device)", kDsound);

// _pump_until: turns the event loop until predicate holds or timeout passes.
inline bool pumpUntil(const std::function<bool()>& predicate,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return predicate();
}

// Deferred deletes are what clear a destroyed button's focus; plain processEvents() never runs
// them outside an event loop.
inline void flush() {
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

// A one-shot latch a slow fake waits on.
class Latch {
public:
    void release() {
        std::lock_guard lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return open_; });
    }
    bool isOpen() {
        std::lock_guard lock(mutex_);
        return open_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_ = false;
};

// One Settings window's world: the rig, the fakes, AppPages registered with fake hooks and a
// stand-in for every other destination.
class AppPagesRig {
public:
    // `extraPages` registers further pages once AppPages has (and before the stand-ins), so a
    // page of another package can take its destination from a stand-in.
    explicit AppPagesRig(const fv::Json& settings = fv::Json::object(),
                         const std::function<void(AppPagesRig&)>& extraPages = {})
        : rig_(settings), backend_(std::make_shared<fv::audio::testing::FakeBackend>()),
          pool_("settings-test", 3),
          credentialBackend_(std::make_shared<fv::MemoryCredentialBackend>()),
          store_(credentialBackend_, fv::identity()),
          dotenv_(rig_.context().paths().env_file(), env_), pages_(rig_.context()) {
        loadS7Devices(*backend_);
        fv::AppPagesHooks& hooks = pages_.hooks();
        hooks.trayAvailable = [this] {
            return trayAvailable;
        };
        hooks.meetingActive = [this] {
            return meetingActive;
        };
        hooks.playbackPlaying = [this] {
            return playbackPlaying;
        };
        hooks.listDevices = [this] {
            ++listings;
            if (listGate != nullptr) {
                listEntered.release();
                (void)listGate->wait(std::chrono::seconds(5));
            }
            return fv::audio::list_input_devices(*backend_);
        };
        hooks.refreshDevices = [this] {
            ++refreshes;
            return backend_->refresh();
        };
        hooks.credentials = &store_;
        hooks.env = &env_;
        hooks.dotenv = &dotenv_;
        hooks.verifyKey = [this](const fv::speech::SpeechEndpoint& endpoint,
                                 const std::string& key) {
            std::lock_guard lock(verifyMutex_);
            verified.emplace_back(endpoint.id, key);
            return verifyAnswer;
        };
        hooks.confirm = [this](QWidget*, const QString& title, const QString& text) {
            asked.emplace_back(title, text);
            return answer;
        };
        hooks.dialog = [this]() -> fv::SettingsDialog* {
            return current_;
        };
        hooks.worker = &pool_;
        // The Appearance preview's microphone toggle is shown, as in the app; this microphone
        // never opens.
        hooks.previewRecorder = [](const fv::Json&) -> std::unique_ptr<fv::Recorder> {
            return nullptr;
        };
        // Pinned well past the steady epoch, as time.monotonic() is in the reference builders.
        clock_.advance(std::chrono::seconds(1000));
        hooks.clock = &clock_;
        pages_.registerPages();
        if (extraPages) {
            extraPages(*this);
        }
        for (const Heading& heading : headings()) {
            if (rig_.registry().page(heading.key) == nullptr) {
                rig_.registry().addPage(standInSpec(heading));
            }
        }
    }

    ~AppPagesRig() {
        // A credential read still on its way asks hooks.dialog() for the window to redraw.
        current_ = nullptr;
        dialogs_.clear();
        pool_.shutdown(std::chrono::seconds(5));
        // Results still queued for the GUI thread go now, before the services they name.
        QCoreApplication::processEvents();
        if (database_) {
            offGui([this] { database_->close(); });
        }
    }

    // History over a temporary database and recordings folder, wired into the hooks; the
    // retention spy records every set_retention.
    fv::HistoryManager& useHistory() {
        const auto root = rig_.context().paths().env_file().parent_path();
        recordings_ = root / "recordings";
        std::filesystem::create_directories(recordings_);
        database_ = std::make_unique<fv::Database>(
            offGui([&root] { return fv::Database::open_or_create(root / "test.db"); }));
        history_ =
            std::make_unique<fv::HistoryManager>(*database_, recordings_, fv::RecordingRetention{});
        fv::HistoryManager* history = history_.get();
        fv::AppPagesHooks& hooks = pages_.hooks();
        hooks.recordingsUsage = [history] {
            return history->get_recordings_usage();
        };
        hooks.recordingsOverLimit = [history](std::optional<std::int64_t> count,
                                              std::optional<std::int64_t> bytes) {
            return history->recordings_over_limit(count, bytes);
        };
        hooks.setRetention = [this, history](std::optional<std::int64_t> count,
                                             std::optional<std::int64_t> bytes) {
            // Recorded once History has pruned, so a test that sees the call sees its effect.
            history->set_retention(count, bytes);
            std::lock_guard lock(retentionMutex_);
            applied.emplace_back(count, bytes);
        };
        return *history_;
    }
    const std::filesystem::path& recordingsFolder() const { return recordings_; }
    std::vector<std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>> appliedNow() {
        std::lock_guard lock(retentionMutex_);
        return applied;
    }
    std::vector<std::pair<std::string, std::string>> verifiedNow() {
        std::lock_guard lock(verifyMutex_);
        return verified;
    }

    fv::SettingsDialog& dialog() {
        if (current_ == nullptr) {
            auto made = std::make_unique<fv::SettingsDialog>(rig_.context());
            made->availableScreenRect = [] {
                return QRect(0, 0, 1920, 1080);
            };
            current_ = made.get();
            dialogs_.push_back(std::move(made));
        }
        return *current_;
    }
    // Destroys the window (a closed one is freed), so the next dialog() builds another.
    void dropDialog() {
        current_ = nullptr;
        dialogs_.clear();
    }

    template <class Page> Page& page(std::string_view key) {
        dialog().selectDestination(qs(key));
        auto* content = dynamic_cast<Page*>(dialog().pageContent(key));
        REQUIRE(content != nullptr);
        return *content;
    }

    fv::MemorySettingsStore& settings() { return rig_.settings(); }
    fv::Json saved(std::string_view key) {
        const auto snapshot = rig_.settings().snapshot();
        const auto found = snapshot->find(key);
        return found == snapshot->end() ? fv::Json() : *found;
    }
    bool has(std::string_view key) { return rig_.settings().snapshot()->contains(key); }
    fv::AppContext& context() { return rig_.context(); }
    fv::AppPages& pages() { return pages_; }
    fv::audio::testing::FakeBackend& backend() { return *backend_; }
    fv::CredentialStore& store() { return store_; }
    fv::MapEnvReader& env() { return env_; }
    fv::DotEnv& dotenv() { return dotenv_; }
    fv::ThreadPool& pool() { return pool_; }

    // Scripted answers and what was asked.
    bool trayAvailable = true;
    bool meetingActive = false;
    bool playbackPlaying = false;
    bool answer = false;
    std::vector<std::pair<QString, QString>> asked;
    std::pair<bool, std::string> verifyAnswer{true, "api.openai.com accepted the key."};
    std::vector<std::pair<std::string, std::string>> verified;
    std::vector<std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>> applied;
    std::atomic<int> listings{0};
    std::atomic<int> refreshes{0};
    // While set, listing devices waits for it (a driver that takes seconds).
    std::shared_ptr<Latch> listGate;
    Latch listEntered;

private:
    SettingsRig rig_;
    fv::FakeClock clock_;
    std::shared_ptr<fv::audio::testing::FakeBackend> backend_;
    fv::ThreadPool pool_;
    std::shared_ptr<fv::MemoryCredentialBackend> credentialBackend_;
    fv::CredentialStore store_;
    fv::MapEnvReader env_;
    fv::DotEnv dotenv_;
    fv::AppPages pages_;
    std::filesystem::path recordings_;
    std::unique_ptr<fv::Database> database_;
    std::unique_ptr<fv::HistoryManager> history_;
    std::mutex retentionMutex_;
    std::mutex verifyMutex_;
    fv::SettingsDialog* current_ = nullptr;
    std::vector<std::unique_ptr<fv::SettingsDialog>> dialogs_;
};

} // namespace fvtest
