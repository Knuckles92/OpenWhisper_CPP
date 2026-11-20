// Quit while workers are busy (OpenWhisper docs/release-health.md, "Quit while workers are busy |
// Measure process exit time, including pending history writes and component installation").
// OpenWhisper sets no bound on it: "There is no application-wide hard shutdown deadline" (the
// same document, "Shutdown limits"), so these metrics are reported, not judged.
//
// The real Bootstrap runs offscreen over the fakes of test_startup.cpp (support/fake_platform.h)
// and its own event loop (Application::run). Each scenario makes one kind of work busy, then the
// main window's Quit (MainWindow::requestQuit) asks the busy question through the controller's
// QuitPrompt, which answers Yes. Two times are taken from that confirmed Quit: to the end of the
// shutdown plan (the moment ShutdownPlan records shutdown_s), and to Bootstrap::run returning
// with every service released, which is the last thing fiorivoce.exe's main() does; the C
// runtime's and Qt's own teardown after it is not in the number, as QApplication's construction
// is not in startup_to_window_s. shutdown_s is the plan's own metric, as the app records it.
//
// What the busy work models:
// - Transcription: a 120 s dictation (kp * on the fake keyboard hook, the tone fixture on the
//   fake microphone) decoded by a backend that does what ParakeetBackend does with a clip: it
//   cuts it into 30 s windows, checks the job's cancel token before each window, and decodes
//   each window in one call that nothing can interrupt (NeMo-Speech.cpp's C ABI has no abort;
//   architecture 5.5). The incremental dictation decodes the finished windows while the
//   recording runs (instantly here, as a fast engine keeps up), so the stop's own decode is the
//   tail window; it takes kWindowDecode, a 30 s Parakeet window on a CPU runtime (an
//   assumption, not a measurement). Quit is confirmed the moment that decode starts, so it
//   waits for a whole window: the worst case for a dictation's decode.
// - Component install: the CUDA speech runtime from the catalog's own size (106,044,768 bytes)
//   downloaded by the real curl client from a server on 127.0.0.1 that sends it at about
//   4 MB/s; the download checks its cancel token as the real one does (curl's progress callback
//   and the verified download's loop). Quit is confirmed after 4 MiB.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QTimer>

#include "../http/loopback_server.h"
#include "base/metrics.h"
#include "credentials/testing/memory_backend.h"
#include "models/component_runtime.h"
#include "speech/messages.h"
#include "support/fake_platform.h"
#include "support/perf_support.h"
#include "ui_controller/quit_confirm.h"
#include "ui_shell/application.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/single_instance.h"

namespace {

using namespace std::chrono_literals;
using fvtest::waitUntil;

// One 30 s window of Parakeet TDT 0.6B on a CPU runtime (see the file comment).
constexpr auto kWindowDecode = std::chrono::milliseconds(1500);
constexpr double kWindowSeconds = 30.0;
constexpr double kDictationSeconds = 120.0;
constexpr int kRecorderRate = 44100;

const std::string kCuda = "asr-nvidia-cuda";
// The catalog's size of the Windows CUDA runtime archive (src/models/data/speech_catalog.json).
constexpr std::int64_t kArchiveBytes = 106'044'768;
constexpr std::size_t kServeChunk = 64 * 1024;
// 64 KiB every 16 ms: about 4 MB/s.
constexpr auto kServePace = std::chrono::milliseconds(16);
constexpr std::int64_t kQuitAfterBytes = 4 * 1024 * 1024;

// What the busy decode saw.
struct DecodeProbe {
    std::atomic<bool> finalStarted{false};
    std::atomic<int> windows{0};
    std::atomic<int> decoded{0};
    std::atomic<bool> canceled{false};
    std::atomic<int> pastes{0};
};

// ParakeetBackend's shape of a decode (see the file comment); load is instant and nothing warms.
class WindowedBackend final : public fv::speech::Backend {
public:
    WindowedBackend(std::shared_ptr<DecodeProbe> probe, fv::speech::BackendConfig config)
        : probe_(std::move(probe)), config_(std::move(config)) {}

    std::string_view id() const override { return "parakeet"; }
    fv::speech::Capabilities capabilities() const override { return {}; }
    fv::speech::Readiness load(const fv::CancelToken&) override {
        loaded_ = true;
        const std::string label =
            config_.model_label.empty() ? std::string("Fake") : config_.model_label;
        return {fv::speech::ReadyState::Ready, label + " | " + config_.device, ""};
    }
    void release() override { loaded_ = false; }

    fv::speech::DecodeResult decode(const fv::speech::DecodeJob& job) override {
        const auto* clip = std::get_if<fv::speech::AudioClip>(&job.input);
        if (clip == nullptr) {
            throw std::invalid_argument("The busy decode takes audio clips");
        }
        if (!loaded_) {
            throw fv::EngineError("Speech engine is not loaded");
        }
        const bool isFinal = job.lane == fv::speech::Lane::Final;
        const double seconds = static_cast<double>(clip->end - clip->begin) / clip->rate;
        const int windows = std::max(1, static_cast<int>(std::ceil(seconds / kWindowSeconds)));
        if (isFinal) {
            probe_->windows = windows;
            probe_->finalStarted = true;
        }
        for (int window = 0; window < windows; ++window) {
            if (job.cancel.cancelled()) {
                if (isFinal) {
                    probe_->canceled = true;
                }
                throw fv::EngineError(std::string(fv::speech::kTranscriptionCanceled));
            }
            if (isFinal) {
                std::this_thread::sleep_for(kWindowDecode);
                ++probe_->decoded;
            }
        }
        fv::speech::DecodeResult result;
        result.transcript.text = "busy decode";
        result.transcript.audio_processed = seconds;
        return result;
    }

private:
    std::shared_ptr<DecodeProbe> probe_;
    fv::speech::BackendConfig config_;
    bool loaded_ = false;
};

// What one quit measured.
struct QuitTimes {
    bool ran = false;
    std::optional<std::string> question; // the busy question, as the prompt was asked it
    double planMs = -1;                  // confirmed Quit to the end of the shutdown plan
    double loopExitMs = -1;              // confirmed Quit to the event loop's return
    double exitMs = -1;                  // confirmed Quit to Bootstrap::run returned
    std::optional<double> shutdownS;     // the plan's own shutdown_s
    std::filesystem::path components;    // the data folder's components root
};

// The context the busy work starts from.
struct Busy {
    fv::AppContext& context;
    fvperf::FakePlatformState& platform;
    std::shared_ptr<fv::audio::testing::FakeBackend> audio;
};

void pressKeypad(fv::hotkeys::FakeKeyboardHook& hook, std::string_view name) {
    for (const bool down : {true, false}) {
        fv::hotkeys::KeyEvent event;
        event.down = down;
        event.name = name;
        event.keypad = true;
        event.t = fv::hotkeys::monotonic_clock()();
        hook.send(event);
    }
}

// Starts the app, makes `startBusy` busy once the engine is ready, confirms Quit from the main
// window and times the way out.
QuitTimes quitWhileBusy(const std::function<bool(Busy&)>& startBusy,
                        std::shared_ptr<DecodeProbe> probe,
                        const std::function<void(fv::ControllerPlatform&)>& adjust = {}) {
    fv::testkit::TempDataRoot root;
    fvtest::ScopedPerUserFolder perUser(root.path());
    fvperf::FakePlatformState state;
    QuitTimes times;

    std::mutex mutex;
    std::optional<fvperf::Clock::time_point> confirmed;
    std::optional<fvperf::Clock::time_point> planEnded;
    fv::metrics::set_sink([&](std::string_view name, double value) {
        if (name == "shutdown_s") {
            const std::lock_guard lock(mutex);
            planEnded = fvperf::Clock::now();
            times.shutdownS = value;
        }
    });

    fv::ControllerFeature* controller = nullptr;
    fv::BootstrapHooks hooks;
    hooks.features = [&state, &controller, &adjust] {
        fv::ControllerPlatform platform = fvperf::fakePlatform(state);
        if (adjust) {
            adjust(platform);
        }
        auto made = std::make_unique<fv::ControllerFeature>(std::move(platform));
        controller = made.get();
        std::vector<std::unique_ptr<fv::Feature>> features;
        features.push_back(std::make_unique<fv::ShellFeature>());
        features.push_back(std::make_unique<fv::RecordFeature>());
        features.push_back(std::make_unique<fv::HistoryFeature>());
        features.push_back(std::make_unique<fv::OverlayFeature>());
        features.push_back(std::make_unique<fv::SetupFeature>());
        features.push_back(std::make_unique<fv::SettingsFeature>());
        features.push_back(std::make_unique<fv::DownloadsFeature>());
        features.push_back(std::make_unique<fv::PersonalizeFeature>());
        features.push_back(std::move(made));
        return features;
    };
    hooks.setupLogging = [](const fv::Paths&) {
        return std::shared_ptr<void>();
    };
    const std::filesystem::path locks = root.path() / "run";
    hooks.acquireInstance = [locks](const fv::Paths& paths) {
        fv::SingleInstanceOptions options;
        options.lockFolder = locks;
        options.handoff = 200ms;
        return fv::SingleInstance::acquire(paths.data_root(), fv::identity(), options);
    };
    hooks.credentialBackend = [] {
        return std::make_shared<fv::MemoryCredentialBackend>();
    };
    hooks.tray.available = [] {
        return true;
    };
    hooks.tray.showIcon = [](QSystemTrayIcon&) {
    };
    hooks.processEvents = [] {
        QApplication::processEvents();
    };
    hooks.errorBox = [](const std::string& message) {
        FAIL_CHECK(message);
    };
    hooks.arguments = std::vector<std::string>{"fiorivoce"};
    auto audio = std::make_shared<fv::audio::testing::FakeBackend>();
    audio->set_layout({{"MME"}}, {{"Microphone (Test)", 0, 1, 0, 44100.0}}, 0);
    hooks.audioBackend = [audio] {
        return audio;
    };
    hooks.speechBackends = [probe] {
        return fv::speech::BackendFactory([probe](const fv::speech::BackendConfig& config)
                                              -> std::unique_ptr<fv::speech::Backend> {
            return std::make_unique<WindowedBackend>(probe, config);
        });
    };
    hooks.httpClient = [] {
        // No proxy: the only server is the test's own on 127.0.0.1.
        fv::http::CurlClientOptions options;
        options.proxy = [](std::string_view) {
            return std::optional<std::string>();
        };
        return fv::http::make_curl_client(std::move(options));
    };
    hooks.run = [&](fv::Application& application, fv::MainWindow& window) {
        fv::AppContext& context = window.context();
        times.components = context.paths().components_root();
        auto& app = context.service<fv::ApplicationController>();
        const bool ready = waitUntil(
            [&] {
                return context.speech().readiness(fv::speech::Slot::Dictation).state ==
                           fv::speech::ReadyState::Ready &&
                       app.reload_machine().settled() && app.startup_database_ready();
            },
            30s);
        CHECK(ready);
        Busy busy{context, state, audio};
        if (!ready || controller == nullptr || controller->uiController() == nullptr ||
            !startBusy(busy)) {
            return 1;
        }
        controller->uiController()->quitPrompt = [&](QWidget*, const fv::QuitQuestion& asked) {
            const std::lock_guard lock(mutex);
            times.question = asked.text.toStdString();
            // The user's Yes: everything after this is the way out.
            confirmed = fvperf::Clock::now();
            return true;
        };
        QTimer::singleShot(0, &window, [&window] { window.requestQuit(); });
        // If the quit were refused, the loop would run on; end it so the case fails instead.
        QTimer::singleShot(60'000, &window, [] { QApplication::exit(2); });
        const int code = application.run(&window);
        const std::lock_guard lock(mutex);
        if (confirmed) {
            times.loopExitMs =
                std::chrono::duration<double, std::milli>(fvperf::Clock::now() - *confirmed)
                    .count();
        }
        return code;
    };

    int argc = 1;
    char program[] = "fiorivoce";
    char* argv[] = {program, nullptr};
    int result = -1;
    {
        fv::Bootstrap bootstrap(std::move(hooks));
        result = bootstrap.run(argc, argv);
    }
    const auto returned = fvperf::Clock::now();
    fv::metrics::set_sink({});

    CHECK(result == 0);
    const std::lock_guard lock(mutex);
    if (confirmed && planEnded) {
        times.planMs = std::chrono::duration<double, std::milli>(*planEnded - *confirmed).count();
        times.exitMs = std::chrono::duration<double, std::milli>(returned - *confirmed).count();
        times.ran = result == 0;
    }
    return times;
}

// A 120 s dictation stopped, and its final decode started.
bool startTranscription(Busy& busy, DecodeProbe& probe) {
    auto& runtime = busy.context.service<fv::dictation::TranscriptionRuntime>();
    if (busy.platform.hook == nullptr) {
        return false;
    }
    pressKeypad(*busy.platform.hook, "*");
    if (!waitUntil([&runtime] { return runtime.is_recording(); }) ||
        !waitUntil([&busy] { return !busy.audio->streams().empty(); })) {
        return false;
    }
    const auto tone =
        fv::audio::testing::read_pcm(fvtest::ControllerRig::testkit_audio("tone-44k1-mono.wav"));
    std::vector<std::int16_t> pcm;
    const auto wanted = static_cast<std::size_t>(kDictationSeconds * kRecorderRate);
    while (pcm.size() < wanted) {
        pcm.insert(pcm.end(), tone.begin(), tone.end());
    }
    pcm.resize(wanted);
    // A second of capture at a time, faster than real time but slow enough for the recording's
    // journal: 120 s at once overflows its queue ("Recording storage cannot keep up").
    const auto stream = busy.audio->streams().back();
    for (std::size_t at = 0; at < pcm.size(); at += kRecorderRate) {
        const std::size_t size = std::min<std::size_t>(kRecorderRate, pcm.size() - at);
        if (stream->deliver_blocks(std::span<const std::int16_t>(pcm.data() + at, size)) == 0) {
            return false;
        }
        waitUntil([] { return false; }, 25ms);
    }
    if (!runtime.is_recording()) {
        return false;
    }
    // The record shortcut ignores a press within 300 ms of the last (HOTKEY_DEBOUNCE_MS).
    waitUntil([] { return false; }, 350ms);
    if (busy.platform.keys != nullptr) {
        busy.platform.keys->on_send = [&probe](const std::string& what) {
            if (what == "paste") {
                ++probe.pastes;
            }
        };
    }
    pressKeypad(*busy.platform.hook, "*");
    return waitUntil([&probe] { return probe.finalStarted.load(); }, 60s);
}

} // namespace

TEST_CASE("quit while workers are busy", "[perf]") {
    const std::string test = "perf.quit while workers are busy";
    std::vector<double> shutdownSamples;

    // ---- a transcription ----
    auto probe = std::make_shared<DecodeProbe>();
    const QuitTimes transcribing =
        quitWhileBusy([&probe](Busy& busy) { return startTranscription(busy, *probe); }, probe);
    REQUIRE(transcribing.ran);
    CHECK(transcribing.question.has_value());
    // The decode Quit waited for was the stop's own, and the job was cancelled: its text never
    // reached the cursor.
    CHECK(probe->windows >= 1);
    CHECK(probe->pastes == 0);
    const fv::Json decodeNotes{
        {"busy", "120 s dictation, the stop's decode of its tail window, Quit as it starts"},
        {"window_decode_ms", kWindowDecode.count()},
        {"windows", probe->windows.load()},
        {"windows_decoded", probe->decoded.load()},
        {"window_refused_after_cancel", probe->canceled.load()},
        {"question", transcribing.question.value_or("")},
        {"loop_exit_ms", transcribing.loopExitMs},
        {"measured_to", "the shutdown plan's end (shutdown_s recorded)"}};
    const auto plan =
        fvperf::record("quit_transcribing_ms", "ms", {transcribing.planMs}, test, decodeNotes);
    CHECK(fvperf::underCeiling(plan, plan.max));
    fv::Json exitNotes = decodeNotes;
    exitNotes["measured_to"] = "Bootstrap::run returned, every service released";
    fvperf::record("quit_transcribing_exit_ms", "ms", {transcribing.exitMs}, test, exitNotes);
    if (transcribing.shutdownS) {
        shutdownSamples.push_back(*transcribing.shutdownS);
    }

    // ---- a component install ----
    struct Served {
        std::atomic<std::int64_t> bytes{0};
        std::atomic<bool> stop{false};
    };
    auto served = std::make_shared<Served>();
    fv::testing::LoopbackServer server([served](const fv::testing::LoopbackServer::Request& request,
                                                fv::testing::LoopbackServer::Reply& reply) {
        reply.write("HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: " +
                    std::to_string(kArchiveBytes) + "\r\nConnection: close\r\n\r\n");
        if (request.method == "HEAD") {
            return;
        }
        const std::string chunk(kServeChunk, 'z');
        std::int64_t sent = 0;
        while (sent < kArchiveBytes && !served->stop) {
            const auto size = static_cast<std::size_t>(std::min<std::int64_t>(
                static_cast<std::int64_t>(chunk.size()), kArchiveBytes - sent));
            reply.write(std::string_view(chunk.data(), size));
            sent += static_cast<std::int64_t>(size);
            served->bytes += static_cast<std::int64_t>(size);
            if (!reply.wait(kServePace)) {
                return;
            }
        }
    });
    const fv::Json entry{
        {"version", "perf"},
        {"component_api", fv::models::kComponentApi},
        {"platform", "win_amd64"},
        {"install_bytes", 431677822},
        {"archives", fv::Json::array({fv::Json{{"name", "nemo-speech-cuda.zip"},
                                               {"url", server.url("/nemo-speech-cuda.zip")},
                                               {"sha256", std::string(64, '0')},
                                               {"size_bytes", kArchiveBytes},
                                               {"extract", "zip"}}})}};
    const fv::Json catalog{{"schema", 2},
                           {"components", {{kCuda, {{"platforms", {{"win_amd64", entry}}}}}}}};
    const auto withCatalog = [&catalog](fv::ControllerPlatform& platform) {
        auto base = platform.modelServices;
        platform.modelServices = [base, &catalog](fv::ModelServiceOptions& services) {
            if (base) {
                base(services);
            }
            services.coordinator.catalog = &catalog;
            // The CUDA runtime is offered only with an NVIDIA GPU.
            services.store.host->gpu = [] {
                fv::models::NvidiaGpu card;
                card.name = "NVIDIA GeForce RTX 3060";
                card.total_mib = 12288;
                card.compute_capability = std::pair{8, 6};
                return std::optional<fv::models::NvidiaGpu>(card);
            };
        };
    };
    const auto startInstall = [&served](Busy& busy) {
        auto& components = busy.context.service<fv::models::ComponentRuntime>();
        components.request_install(kCuda);
        return waitUntil([&served] { return served->bytes.load() >= kQuitAfterBytes; }, 30s);
    };
    const QuitTimes installing =
        quitWhileBusy(startInstall, std::make_shared<DecodeProbe>(), withCatalog);
    const std::int64_t servedAtExit = served->bytes.load();
    served->stop = true;
    REQUIRE(installing.ran);
    CHECK(installing.question.has_value());
    // Cancellation was honoured: the download stopped and nothing was installed.
    CHECK(servedAtExit < kArchiveBytes);
    CHECK_FALSE(std::filesystem::exists(installing.components / kCuda / ".installed"));
    const fv::Json installNotes{
        {"busy", "CUDA runtime install, 106,044,768-byte download at about 4 MB/s from "
                 "127.0.0.1, Quit after 4 MiB"},
        {"bytes_served_by_exit", servedAtExit},
        {"question", installing.question.value_or("")},
        {"loop_exit_ms", installing.loopExitMs},
        {"measured_to", "the shutdown plan's end (shutdown_s recorded)"}};
    const auto installPlan =
        fvperf::record("quit_installing_ms", "ms", {installing.planMs}, test, installNotes);
    CHECK(fvperf::underCeiling(installPlan, installPlan.max));
    fv::Json installExit = installNotes;
    installExit["measured_to"] = "Bootstrap::run returned, every service released";
    fvperf::record("quit_installing_exit_ms", "ms", {installing.exitMs}, test, installExit);
    if (installing.shutdownS) {
        shutdownSamples.push_back(*installing.shutdownS);
    }

    REQUIRE(shutdownSamples.size() == 2);
    fvperf::record("shutdown_s", "s", shutdownSamples, test,
                   fv::Json{{"samples", "a quit during a transcription, then during an install"}});
}
