// M2 end to end (section 11): the real feature graph, offscreen, under a temporary data folder,
// over the fake audio system, the fake speech engine (a text per window), the fake HTTP client
// and the fake keyboard hook. Each case is one link of the M2 demo.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextEdit>

#include "../models/models_test_support.h"
#include "base/executor.h"
#include "base/hash.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "controller_support.h"
#include "credentials/dotenv.h"
#include "credentials/testing/memory_backend.h"
#include "dictation/batch.h"
#include "dictation/streaming_runtime.h"
#include "history/manager.h"
#include "http/testing/fake_http_client.h"
#include "models/component_runtime.h"
#include "models/gpu_probe.h"
#include "models/model_runtime.h"
#include "models/testing/fake_openwhisper.h"
#include "recorder/recorder.h"
#include "settings/keys.h"
#include "speech/cloud_backend.h"
#include "speech/incremental_session.h"
#include "ui_downloads/downloads_page.h"
#include "ui_downloads/downloads_presenter.h"
#include "ui_downloads/hf_consent_dialog.h"
#include "ui_history/history_calendar_dialog.h"
#include "ui_history/history_export_dialog.h"
#include "ui_history/stats_dialog.h"
#include "ui_record/upload_file_tab.h"
#include "ui_record/upload_presenter.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_search_palette.h"
#include "ui_settings/settings_window.h"
#include "ui_setup/guided_setup_dialog.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_widgets/tabbed_content.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;
using namespace std::chrono_literals;

namespace {

namespace fs = std::filesystem;

const std::string kCuda = "asr-nvidia-cuda";
const std::string kOrukeet = "orukeet-v0.1";
const std::string kWeights = "verified model weights";

// The visible top-level window of type T, or null.
template <class T> T* shownWindow() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* found = qobject_cast<T*>(widget); found != nullptr && found->isVisible()) {
            return found;
        }
    }
    return nullptr;
}

std::size_t historyCount(ControllerRig& rig) {
    auto& history = rig.context().service<fv::HistoryManager>();
    return fvtest::offGui([&history] { return history.get_history().size(); });
}

std::vector<std::string> historyTexts(ControllerRig& rig) {
    auto& history = rig.context().service<fv::HistoryManager>();
    return fvtest::offGui([&history] {
        std::vector<std::string> texts;
        for (const auto& entry : history.get_history()) {
            texts.push_back(entry.text);
        }
        return texts;
    });
}

// A copy of the 16 kHz tone fixture in the data folder.
fs::path toneFile(ControllerRig& rig, const std::string& name) {
    const fs::path path = rig.paths().data_root() / name;
    fs::copy_file(ControllerRig::testkit_audio("tone-16k-mono.wav"), path,
                  fs::copy_options::overwrite_existing);
    return path;
}

// Starts a hotkey dictation and waits for the microphone stream.
void startDictation(ControllerRig& rig) {
    rig.pressKeypad("*");
    REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    REQUIRE(waitUntil([&rig] { return !rig.audio().streams().empty(); }));
}

// The record shortcut ignores a press within 300 ms of the last one (HOTKEY_DEBOUNCE_MS).
void stopDictation(ControllerRig& rig) {
    waitUntil([] { return false; }, 400ms);
    rig.pressKeypad("*");
}

std::vector<fv::speech::FakeCall> decodesOn(ControllerRig& rig, fv::speech::Lane lane) {
    std::vector<fv::speech::FakeCall> found;
    for (const auto& call : rig.engine().calls()) {
        if (call.op == "decode" && call.lane == lane) {
            found.push_back(call);
        }
    }
    return found;
}

} // namespace

TEST_CASE("M2 end to end: the live preview shows windows in the overlay and Quick Record",
          "[e2e][widget]") {
    RigOptions options;
    options.settings["streaming_enabled"] = true;
    options.script.texts = {"Hello", "from the preview"};
    ControllerRig rig(options);
    auto& streaming = rig.context().service<fv::dictation::StreamingRuntime>();
    std::vector<std::string> shown;
    auto link = streaming.streaming_text_update.connect(
        rig.uiExecutor(), [&shown](std::string text, bool) { shown.push_back(text); });
    REQUIRE(rig.startEngine());
    REQUIRE(waitUntil([&streaming] { return streaming.configured(); }));

    startDictation(rig);
    // 7 s of speech, a second at a time, as the microphone delivers it.
    for (int second = 0; second < 7; ++second) {
        CHECK(rig.speak(1.0) > 0);
        waitUntil([] { return false; }, 150ms);
    }
    REQUIRE(waitUntil([&shown] { return shown.size() >= 2; }, 20s));
    CHECK(rig.runtime().is_recording());
    CHECK(rig.overlay().currentState() == fv::WaveformOverlay::kStateStreaming);
    CHECK_FALSE(rig.overlay().streamingText().isEmpty());
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    CHECK_FALSE(tab->transcriptText()->toPlainText().isEmpty());

    stopDictation(rig);
    REQUIRE(waitUntil([&rig] { return historyCount(rig) == 1 && !rig.runtime().has_active_job(); },
                      30s));
    CHECK(decodesOn(rig, fv::speech::Lane::Preview).size() >= 2);
}

TEST_CASE("M2 end to end: a long dictation decodes its windows while it is recorded",
          "[e2e][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());

    startDictation(rig);
    // 70 s of speech in 5 s blocks: the recorder's tape keeps about 12 s, and the session reads
    // it once a second, as it does while the user speaks.
    for (int block = 0; block < 14; ++block) {
        CHECK(rig.speak(5.0) > 0);
        waitUntil([] { return false; }, 1100ms);
    } // Two complete 30 s windows are decoded early, on the Incremental lane.
    REQUIRE(waitUntil([&rig] { return decodesOn(rig, fv::speech::Lane::Incremental).size() >= 2; },
                      30s));
    CHECK(rig.runtime().is_recording());
    const std::size_t finalBefore = decodesOn(rig, fv::speech::Lane::Final).size();

    stopDictation(rig);
    // The job slot is released after persistence. Avoid starting a database thread on every
    // event-loop poll, which competes with the recorder and workers under a parallel test run.
    const bool completed = waitUntil(
        [&rig] { return !rig.runtime().is_recording() && !rig.runtime().has_active_job(); }, 60s);
    const auto recorder = rig.context().service<fv::Recorder>().snapshot();
    const bool recording = rig.runtime().is_recording();
    const bool activeJob = rig.runtime().has_active_job();
    const auto finalDecodes = decodesOn(rig, fv::speech::Lane::Final).size() - finalBefore;
    const auto history = historyCount(rig);
    const auto keys = rig.keys().sent();
    const auto statuses = rig.statuses();
    const bool hookInstalled = rig.hook().installed();
    CAPTURE(recording, activeJob, recorder.stop_requested, recorder.post_roll_end_reason,
            finalDecodes, history, keys, statuses, hookInstalled);
    REQUIRE(completed);
    REQUIRE(history == 1);
    // Only what the stop left is decoded after it.
    CHECK(decodesOn(rig, fv::speech::Lane::Incremental).size() == 2);
    CHECK(decodesOn(rig, fv::speech::Lane::Final).size() - finalBefore == 1);
}

TEST_CASE("M2 end to end: three separate files from Upload File make three History rows",
          "[e2e][widget]") {
    RigOptions options;
    options.script.texts = {"First file.", "Second file.", "Third file."};
    ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    auto& upload = rig.context().service<fv::UploadPresenter>();
    upload.attach(qobject_cast<fv::UploadFileTab*>(rig.context().screens().ensure(
        fv::RecordFeature::kUploadScreen, rig.window().tabbedContent()->stack())));

    fv::dictation::BatchUploadRequest request;
    for (const char* name : {"one.wav", "two.wav", "three.wav"}) {
        request.items.push_back(
            fv::dictation::BatchItem{fv::base_utf8::path_to_utf8(toneFile(rig, name)), 1.0});
    }
    request.relation = fv::dictation::BatchRelation::Separate;
    upload.requestUploads(request);

    REQUIRE(waitUntil([&rig] { return historyCount(rig) == 3 && !rig.runtime().has_active_job(); },
                      30s));
    auto texts = historyTexts(rig);
    std::ranges::sort(texts);
    CHECK(texts == std::vector<std::string>{"First file.", "Second file.", "Third file."});
}

TEST_CASE("M2 end to end: the API engine sends a file to Groq as multipart", "[e2e][widget]") {
    fv::http::FakeHttpClient http;
    http.set_handler([](const fv::http::Request& request) {
        fv::http::ScriptedResponse reply;
        reply.headers = {{"Content-Type", "text/plain"}};
        reply.body = "Hello from Groq.";
        if (const auto* form = std::get_if<fv::http::Multipart>(&request.body)) {
            for (const auto& part : form->parts) {
                const auto* value = std::get_if<std::string>(&part.data);
                if (part.name == "response_format" && value != nullptr && *value == "json") {
                    reply.headers = {{"Content-Type", "application/json"}};
                    reply.body = R"({"text": "Hello from Groq."})";
                }
            }
        }
        return reply;
    });
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    fv::MapEnvReader env({{"GROQ_API_KEY", "gsk-test"}});
    fv::ThreadPool pool("uploads-test", 3);
    fv::Strand uploads(pool, "uploads", fv::speech::kChunkUploadConcurrency,
                       fv::ShutdownMode::CancelQueued);
    std::unique_ptr<fv::DotEnv> dotenv;

    RigOptions options;
    options.settings["selected_model"] = "api";
    options.settings["api_transcription_endpoint"] = "groq";
    options.http = &http;
    options.credentials = &credentials;
    options.backends = [&](fv::speech::BackendFactory local) {
        return fv::speech::BackendFactory([&, local](const fv::speech::BackendConfig& config)
                                              -> std::unique_ptr<fv::speech::Backend> {
            if (config.backend != "api") {
                return local(config);
            }
            return std::make_unique<fv::speech::CloudSpeechBackend>(
                http, credentials, env, *dotenv, config.settings, uploads, fv::system_clock());
        });
    };
    options.beforeFeatures = [&dotenv](fv::AppContext& context) {
        dotenv = std::make_unique<fv::DotEnv>(context.paths().env_file(), fv::system_env());
    };
    ControllerRig rig(options);
    rig.mainWindowShown();
    REQUIRE(waitUntil([&rig] {
        return rig.speech().readiness(fv::speech::Slot::Dictation).state ==
               fv::speech::ReadyState::Ready;
    }));
    REQUIRE(rig.speech().accepts_files(fv::speech::Slot::Dictation));

    const fs::path audio = toneFile(rig, "meeting-notes.wav");
    REQUIRE(rig.runtime().upload_audio_file(audio, 1.0));
    REQUIRE(waitUntil([&rig] { return historyCount(rig) == 1 && !rig.runtime().has_active_job(); },
                      30s));

    CHECK(historyTexts(rig) == std::vector<std::string>{"Hello from Groq."});
    const auto requests = http.requests();
    const auto sent = std::ranges::find_if(requests, [](const fv::http::Request& request) {
        return std::holds_alternative<fv::http::Multipart>(request.body);
    });
    REQUIRE(sent != requests.end());
    CHECK(sent->url == "https://api.groq.com/openai/v1/audio/transcriptions");
    const auto parts = http.multipart_parts();
    const auto file = std::ranges::find_if(
        parts.at(static_cast<std::size_t>(sent - requests.begin())),
        [](const fv::http::MultipartPart& part) { return part.name == "file"; });
    CHECK(file != parts.at(static_cast<std::size_t>(sent - requests.begin())).end());
    rig.plan().run();
    uploads.shutdown(2000ms);
}

TEST_CASE("M2 end to end: Downloads installs the CUDA runtime and downloads Orukeet with consent",
          "[e2e][widget]") {
    const std::string archive = fv::models::test::stored_zip(
        {{"bin/nemo_speech_asr_c.dll", "runtime"}, {"bin/ggml.dll", "ggml"}});
    fv::http::FakeHttpClient http;
    http.set_handler([&archive](const fv::http::Request& request) {
        return fv::http::FakeHttpClient::serve(
            request, request.url.find(".zip") != std::string::npos ? archive : kWeights);
    });
    const fv::Json entry{
        {"version", "cuda"},
        {"component_api", fv::models::kComponentApi},
        {"platform", "win_amd64"},
        {"install_bytes", 1000},
        {"archives", fv::Json::array({fv::Json{{"name", "runtime.zip"},
                                               {"url", "https://example.invalid/runtime.zip"},
                                               {"sha256", fv::sha256_hex(archive)},
                                               {"size_bytes", archive.size()},
                                               {"extract", "zip"}}})}};
    const fv::Json catalog{{"schema", 2},
                           {"components", {{kCuda, {{"platforms", {{"win_amd64", entry}}}}}}}};

    RigOptions options;
    options.http = &http;
    options.settings["hf_access_policy"] = "ask";
    options.modelServices = [&catalog](fv::ModelServiceOptions& services) {
        services.coordinator.catalog = &catalog;
        services.store.host->gpu = [] {
            fv::models::NvidiaGpu card;
            card.name = "NVIDIA GeForce RTX 3060";
            card.total_mib = 12288;
            card.compute_capability = std::pair{8, 6};
            return std::optional<fv::models::NvidiaGpu>(card);
        };
        services.cache.artifacts = [](std::string_view key) {
            fv::Json spec = fv::models::testing::small_model_spec("weights.gguf", kWeights);
            spec["repo"] = "test/" + std::string(key);
            return spec;
        };
    };
    ControllerRig rig(options);
    // The CPU runtime is loaded: a GPU runtime installed now is used from the next start.
    REQUIRE(rig.startEngine());

    auto& presenter = rig.context().service<fv::DownloadsPresenter>();
    int consentShown = 0;
    presenter.runDialog = [&consentShown](QDialog& dialog) {
        if (dynamic_cast<fv::HuggingFaceConsentDialog*>(&dialog) != nullptr) {
            ++consentShown;
            // The answer the user clicks.
            for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("Download once")) {
                    button->click();
                }
            }
        }
        return dialog.result();
    };
    auto& window = rig.context().service<fv::SettingsWindow>();
    window.raiseDialog = [](fv::SettingsDialog&) {
    };
    window.open(QStringLiteral("downloads"));
    fv::DownloadsPage* page = presenter.page();
    REQUIRE(page != nullptr);

    auto& components = rig.context().service<fv::models::ComponentRuntime>();
    std::vector<std::string> phases;
    std::vector<std::string> finished;
    auto progress = components.component_progress.connect(
        rig.uiExecutor(), [&phases](std::string, std::string phase, std::int64_t, std::int64_t) {
            if (phases.empty() || phases.back() != phase) {
                phases.push_back(phase);
            }
        });
    auto done = components.component_install_finished.connect(
        rig.uiExecutor(),
        [&finished](std::string, bool, std::string message) { finished.push_back(message); });

    Q_EMIT page->componentInstallRequested(QString::fromStdString(kCuda));
    REQUIRE(waitUntil([&finished] { return !finished.empty(); }, 30s));
    CHECK_FALSE(phases.empty());
    CHECK(finished.front() ==
          "Restart " + std::string(fv::identity().product) + " to switch to the GPU runtime.");
    CHECK(fs::exists(rig.paths().components_root() / kCuda / ".installed"));

    Q_EMIT page->downloadRequested(QString::fromStdString(kOrukeet));
    REQUIRE(waitUntil(
        [&rig] {
            return fs::exists(rig.paths().speech_models_root() / kOrukeet / "installed.json");
        },
        30s));
    CHECK(consentShown == 1);
    rig.plan().run();
}

TEST_CASE("M2 end to end: the guided setup offers itself and its try box takes a dictation",
          "[e2e][widget]") {
    fv::http::FakeHttpClient http;
    RigOptions options;
    options.http = &http;
    options.settings["guided_setup_state"] = "pending";
    ControllerRig rig(options);
    auto& setup = rig.context().service<fv::GuidedSetup>();
    setup.hooks().raiseDialog = [](fv::GuidedSetupDialog& dialog) {
        dialog.show();
    };
    REQUIRE(rig.startEngine());

    REQUIRE(waitUntil([&setup] { return setup.dialog() != nullptr && setup.dialog()->isVisible(); },
                      10s));
    fv::GuidedSetupDialog& dialog = *setup.dialog();
    dialog.showPage(fv::GuidedSetupDialog::Ready);

    startDictation(rig);
    CHECK(rig.speak(1.5) > 0);
    stopDictation(rig);
    REQUIRE(waitUntil([&rig] { return historyCount(rig) == 1 && !rig.runtime().has_active_job(); },
                      30s));
    CHECK(waitUntil([&dialog] {
        return dialog.tryBox()->toPlainText().contains(QStringLiteral("Hello from FioriVoce."));
    }));
    dialog.hide();
}

TEST_CASE("M2 end to end: the calendar, export and Stats windows open from their menu items",
          "[e2e][widget]") {
    ControllerRig rig;
    fv::MenuRegistry& menus = rig.context().menus();
    rig.window().show();
    // The export window is modal in the app: the test records that it was run instead.
    auto* history = rig.window().findChild<fv::HistoryPresenter*>();
    REQUIRE(history != nullptr);
    bool exportRun = false;
    history->runExportDialog = [&exportRun](fv::HistoryExportDialog&) {
        exportRun = true;
    };

    menus.action(fv::menu_id::historyCalendar)->trigger();
    CHECK(waitUntil([] { return shownWindow<fv::HistoryCalendarDialog>() != nullptr; }));
    menus.action(fv::menu_id::stats)->trigger();
    CHECK(waitUntil([] { return shownWindow<fv::StatsDialog>() != nullptr; }));
    auto& panel = rig.context().service<fv::HistoryPanel>();
    rig.window().toggleSidePanel();
    REQUIRE(panel.sidebar() != nullptr);
    Q_EMIT panel.sidebar()->exportRequested();
    CHECK(waitUntil([&exportRun] { return exportRun; }));

    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<QDialog*>(widget) != nullptr) {
            widget->close();
        }
    }
}

TEST_CASE("M2 end to end: Settings search finds a speech model and opens it on Downloads",
          "[e2e][widget]") {
    fv::http::FakeHttpClient http;
    RigOptions options;
    options.http = &http;
    ControllerRig rig(options);
    auto& window = rig.context().service<fv::SettingsWindow>();
    window.raiseDialog = [](fv::SettingsDialog& dialog) {
        dialog.show();
    };
    window.open();
    fv::SettingsDialog* dialog = window.dialog();
    REQUIRE(dialog != nullptr);

    dialog->openSearch(QStringLiteral("Orukeet"));
    auto* palette = dialog->findChild<fv::SearchPalette*>();
    REQUIRE(palette != nullptr);
    REQUIRE(waitUntil([palette] { return !palette->currentEntries().empty(); }));
    const auto& entries = palette->currentEntries();
    const auto found = std::ranges::find_if(entries, [](const fv::appcore::SearchEntry& entry) {
        return entry.destination == "downloads" &&
               QString::fromStdString(entry.title).contains(QStringLiteral("Orukeet"));
    });
    REQUIRE(found != entries.end());

    Q_EMIT palette->activated(*found);

    CHECK(waitUntil(
        [dialog] { return dialog->currentDestination() == QStringLiteral("downloads"); }));
    dialog->hide();
}
