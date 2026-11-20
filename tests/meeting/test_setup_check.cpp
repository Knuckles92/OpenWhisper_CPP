// tests/test_meeting_setup.py: model inspection and preparation, and the temporary audio check of
// Meeting Mode's setup, with fake downloads and the fake audio backend.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/backend.h"
#include "audio/testing/fake_backend.h"
#include "http/testing/fake_http_client.h"
#include "meeting/setup_check.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/hf_access.h"
#include "settings/store.h"
#include "testkit/testkit.h"

using fv::Json;
using namespace fv::meeting;
using Catch::Matchers::ContainsSubstring;

namespace {

// Everything a SetupDeps seam answers, scripted.
struct ScriptedSetup {
    Json settings = Json::object();
    std::vector<std::pair<std::string, std::string>> resolve_calls;
    bool installed = false;
    bool cached = false;
    std::optional<std::filesystem::path> speaker_model;
    std::string hf_policy = "ask";
    std::vector<std::string> downloads;
    std::vector<std::string> installs;
    int speaker_downloads = 0;
    std::vector<const fv::CancelToken*> cancels;

    SetupDeps deps() {
        SetupDeps out;
        out.settings = [this] {
            return settings;
        };
        out.resolve_runtime = [this](std::string_view backend, std::string_view device) {
            resolve_calls.emplace_back(backend, device);
            return std::string("asr-nvidia-cpu");
        };
        out.is_installed = [this](std::string_view) {
            return installed;
        };
        out.is_model_cached = [this](std::string_view) {
            return cached;
        };
        out.speaker_model_path = [this] {
            return speaker_model;
        };
        out.catalog_entry = [](std::string_view) {
            return std::optional<Json>(Json::object());
        };
        out.check_compatibility = [](const Json&) {
            return std::string();
        };
        out.hf_offline_env = [] {
            return false;
        };
        out.hf_access_policy = [this] {
            return hf_policy;
        };
        out.download_model = [this](std::string_view model, const fv::CancelToken& cancel) {
            downloads.emplace_back(model);
            cancels.push_back(&cancel);
            cached = true;
        };
        out.describe_download_error = [](std::string_view, const std::exception_ptr&) {
            return std::string("download failed");
        };
        out.install_component = [this](std::string_view component, const Json&,
                                       const fv::CancelToken& cancel) {
            installs.emplace_back(component);
            cancels.push_back(&cancel);
            installed = true;
        };
        out.speaker_model_download_blocked = [] {
            return std::string();
        };
        out.ensure_speaker_model = [this](const fv::CancelToken&) {
            ++speaker_downloads;
            speaker_model = "speaker.onnx";
        };
        return out;
    }
};

// The coordinators prepare_models claims, over a store and settings of their own.
struct Coordinators {
    fv::testkit::TempDataRoot root;
    fv::http::FakeHttpClient client;
    fv::SettingsStore settings{root.path() / "settings.json"};
    fv::models::ComponentStore store{root.path() / "components", client,
                                     fv::models::ComponentStoreOptions{.host = fv::models::Host{}}};
    fv::models::ComponentCoordinator components{store};
    fv::models::HuggingFaceAccessCoordinator access{settings, [](std::string_view) {
                                                        return false;
                                                    }};
};

// The fake audio backend behind a decorator that records the order its streams are closed in.
class RecordingBackend final : public fv::audio::Backend {
public:
    explicit RecordingBackend(std::shared_ptr<fv::audio::testing::FakeBackend> inner)
        : inner_(std::move(inner)) {}

    std::vector<std::string> closed() const {
        const std::lock_guard lock(mutex_);
        return closed_;
    }

    std::vector<fv::audio::DeviceInfo> inputs() override { return inner_->inputs(); }
    std::vector<fv::audio::DeviceInfo> outputs() override { return inner_->outputs(); }
    std::vector<fv::audio::LoopbackEndpoint> loopbacks() override {
        loopback_listings_.fetch_add(1);
        return inner_->loopbacks();
    }
    std::expected<void, std::string> check_input(const fv::audio::StreamConfig& config) override {
        return inner_->check_input(config);
    }
    std::expected<std::unique_ptr<fv::audio::Stream>, std::string>
    open_capture(const fv::audio::StreamConfig& config, fv::audio::CaptureFn fn, void* user,
                 std::function<void(std::string)> on_lost) override {
        return wrap("mic", inner_->open_capture(config, fn, user, std::move(on_lost)));
    }
    std::expected<std::unique_ptr<fv::audio::Stream>, std::string>
    open_loopback(const fv::audio::LoopbackEndpoint& endpoint, fv::audio::CaptureFn fn, void* user,
                  std::function<void(std::string)> on_lost) override {
        return wrap("loopback", inner_->open_loopback(endpoint, fn, user, std::move(on_lost)));
    }
    std::expected<std::unique_ptr<fv::audio::Stream>, std::string>
    open_playback(const fv::audio::StreamConfig& config,
                  fv::audio::PlaybackSource& source) override {
        return inner_->open_playback(config, source);
    }
    bool refresh() override { return inner_->refresh(); }

    int loopback_listings() const { return loopback_listings_.load(); }

private:
    class Recorded final : public fv::audio::Stream {
    public:
        Recorded(std::unique_ptr<fv::audio::Stream> inner, std::string name,
                 RecordingBackend& owner)
            : inner_(std::move(inner)), name_(std::move(name)), owner_(owner) {}
        ~Recorded() override { note(); }
        bool active() const override { return inner_->active(); }
        void abort() override {
            note();
            inner_->abort();
        }

    private:
        void note() {
            if (!noted_) {
                noted_ = true;
                const std::lock_guard lock(owner_.mutex_);
                owner_.closed_.push_back(name_);
            }
        }
        std::unique_ptr<fv::audio::Stream> inner_;
        std::string name_;
        RecordingBackend& owner_;
        bool noted_ = false;
    };

    std::expected<std::unique_ptr<fv::audio::Stream>, std::string>
    wrap(std::string name, std::expected<std::unique_ptr<fv::audio::Stream>, std::string> opened) {
        if (!opened) {
            return opened;
        }
        return std::make_unique<Recorded>(std::move(*opened), std::move(name), *this);
    }

    std::shared_ptr<fv::audio::testing::FakeBackend> inner_;
    mutable std::mutex mutex_;
    std::vector<std::string> closed_;
    std::atomic<int> loopback_listings_{0};
};

std::shared_ptr<fv::audio::testing::FakeBackend> fake_devices() {
    auto fake = std::make_shared<fv::audio::testing::FakeBackend>();
    fake->set_layout({{"MME", 0, -1}}, {{"Microphone", 0, 1, 0, 16000.0}}, 0);
    fake->set_loopbacks({{"speakers", "Speakers", true, 16000, 1}});
    return fake;
}

// Plays the devices: once a stream of `kind` has started, two signal windows of `level`.
void play(fv::audio::testing::FakeBackend& fake, fv::audio::testing::FakeStream::Kind kind,
          std::int16_t level) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& stream : fake.streams()) {
            if (stream->kind() == kind && stream->started()) {
                // 8 blocks of 1024 frames at 16 kHz: two full 0.25 s windows.
                const std::vector<std::int16_t> pcm(8 * 1024, level);
                stream->deliver_blocks(pcm);
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST_CASE("setup_complete reads the saved meeting setup flag", "[meeting][setup]") {
    CHECK_FALSE(setup_complete(Json::object()));
    CHECK_FALSE(setup_complete({{"meeting_setup_complete", false}}));
    CHECK(setup_complete({{"meeting_setup_complete", true}}));
}

TEST_CASE("test_inspect_models_checks_selected_meeting_runtime", "[meeting][setup]") {
    for (const bool cached : {false, true}) {
        DYNAMIC_SECTION("cached " << cached) {
            ScriptedSetup setup;
            setup.settings = {{"meeting_speech_separate", true},
                              {"meeting_asr_model", "parakeet-v3"},
                              {"meeting_speaker_id_backend", "off"}};
            setup.installed = cached;
            setup.cached = cached;

            const ModelSetup plan = inspect_models(setup.deps());

            CHECK(plan.runtime == (cached ? "" : "asr-nvidia-cpu"));
            CHECK(plan.speech_download == (cached ? "" : "parakeet-v3"));
            CHECK_FALSE(plan.speaker_download);
            CHECK(setup.resolve_calls ==
                  std::vector<std::pair<std::string, std::string>>{{"parakeet", "auto"}});
        }
    }
}

TEST_CASE("test_prepare_installs_only_missing_files_and_releases_claims", "[meeting][setup]") {
    ScriptedSetup setup;
    setup.settings = {{"meeting_speech_separate", true}, {"meeting_asr_model", "parakeet-v3"}};
    Coordinators coordinators;
    SetupDeps deps = setup.deps();
    deps.components = &coordinators.components;
    deps.hf_access = &coordinators.access;
    REQUIRE(inspect_models(deps).missing() ==
            std::vector<std::string>{"Parakeet TDT 0.6B v3", "speech runtime",
                                     "speaker identification model"});
    const fv::CancelSource source;
    const fv::CancelToken cancel = source.token();
    std::vector<std::string> statuses;

    const ModelSetup ready =
        prepare_models(deps, cancel, [&statuses](std::string text) { statuses.push_back(text); });

    CHECK(ready == ModelSetup{"Parakeet TDT 0.6B v3", "", "", false});
    CHECK(setup.downloads == std::vector<std::string>{"parakeet-v3"});
    CHECK(setup.installs == std::vector<std::string>{"asr-nvidia-cpu"});
    REQUIRE(setup.cancels.size() == 2);
    CHECK(setup.cancels[0] == &cancel);
    CHECK(setup.cancels[1] == &cancel);
    CHECK(setup.speaker_downloads == 1);
    CHECK_FALSE(coordinators.components.is_any_installing());
    CHECK(coordinators.access.requests_in_flight() == 0);
    CHECK(statuses == std::vector<std::string>{"Downloading Parakeet TDT 0.6B v3…",
                                               "Installing the speech runtime…",
                                               "Downloading the speaker identification model…"});
}

TEST_CASE("test_prepare_honors_offline_policy", "[meeting][setup]") {
    ScriptedSetup setup;
    setup.settings = {{"meeting_speaker_id_backend", "off"}};
    setup.installed = true;
    setup.hf_policy = "never";
    Coordinators coordinators;
    SetupDeps deps = setup.deps();
    deps.components = &coordinators.components;
    deps.hf_access = &coordinators.access;

    try {
        prepare_models(deps, {}, {});
        FAIL("no error");
    } catch (const std::runtime_error& error) {
        CHECK_THAT(error.what(), ContainsSubstring("downloads are disabled"));
    }
    CHECK(setup.downloads.empty());
}

TEST_CASE("test_audio_check_routes_and_closes_both_devices", "[meeting][setup]") {
    struct Level {
        std::int16_t system;
        bool ready;
    };
    for (const Level level : {Level{1000, true}, Level{100, false}, Level{0, false}}) {
        DYNAMIC_SECTION("system level " << level.system) {
            auto fake = fake_devices();
            RecordingBackend backend(fake);
            std::thread devices([&fake, &level] {
                play(*fake, fv::audio::testing::FakeStream::Kind::Capture, 100);
                play(*fake, fv::audio::testing::FakeStream::Kind::Loopback, level.system);
            });
            ScriptedSetup setup;

            const AudioCheck result =
                check_audio(backend, setup.deps(), {}, true, std::chrono::milliseconds(600));
            devices.join();

            CHECK(result.ready() == level.ready);
            CHECK(result.microphone == "Signal detected");
            CHECK(result.system_audio == (level.ready ? "Signal detected" : "No signal detected"));
            CHECK(backend.closed() == std::vector<std::string>{"loopback", "mic"});
            CHECK(backend.loopback_listings() > 0);
        }
    }
}

TEST_CASE("test_cancelled_audio_check_does_not_open_devices", "[meeting][setup]") {
    auto fake = fake_devices();
    fv::CancelSource source;
    source.cancel();
    ScriptedSetup setup;

    const AudioCheck result =
        check_audio(*fake, setup.deps(), source.token(), true, std::chrono::milliseconds(0));

    CHECK(fake->listings() == 0);
    CHECK(fake->streams().empty());
    CHECK(result == AudioCheck{"Could not open", "Could not open"});
}

TEST_CASE("the meeting audio check hears the real microphone and system audio",
          "[meeting][setup][interactive]") {
    // Opens this computer's default microphone and render endpoint for six seconds.
    const std::shared_ptr<fv::audio::Backend> backend = fv::audio::make_portaudio_backend();
    SetupDeps deps;
    const AudioCheck result = check_audio(*backend, deps, {});
    INFO("microphone: " << result.microphone << ", system audio: " << result.system_audio);
    CHECK(result.microphone != "Could not open");
    CHECK(result.system_audio != "Could not open");
}
