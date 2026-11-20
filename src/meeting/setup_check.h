#pragma once

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/backend.h"
#include "audio/devices.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/env.h"
#include "base/json.h"
#include "base/paths.h"
#include "http/client.h"
#include "models/coordinator.h"
#include "models/hf_access.h"
#include "models/speech_model_cache.h"
#include "settings/store.h"

// Model preparation and the temporary audio check of Meeting Mode's first-run setup
// (services/meeting_setup.py). Nothing here saves a sample or starts a meeting.
// Affinity: functions; they block (downloads, device opens), never on the GUI thread.
namespace fv::meeting {

// The setup's microphone threshold: it checks whether the mic can hear the room, about -54 dBFS
// for int16 PCM. The dashboard uses a higher one for sustained speech-level signal.
inline constexpr double SETUP_MIC_SIGNAL_RMS = 64.0;
// How long check_audio listens by default.
inline constexpr std::chrono::milliseconds kAudioCheckDuration{6000};

// What meeting setup reads and the services it downloads and installs through. Every member is a
// seam with a production value from setup_deps(); tests replace them.
struct SetupDeps {
    // The settings snapshot (load_all_settings).
    std::function<Json()> settings;
    const Clock* clock = nullptr; // nullptr: system_clock()
    // resolve_runtime(backend, requested device): the runtime component a model of `backend`
    // needs on this computer.
    std::function<std::string(std::string_view backend, std::string_view device)> resolve_runtime;
    std::function<bool(std::string_view component)> is_installed;
    std::function<bool(std::string_view model)> is_model_cached;
    std::function<std::optional<std::filesystem::path>()> speaker_model_path;

    // prepare_models. The coordinators serialize installs and downloads with the rest of the app.
    models::ComponentCoordinator* components = nullptr;
    models::HuggingFaceAccessCoordinator* hf_access = nullptr;
    // The component's catalog entry for this platform, or nullopt.
    std::function<std::optional<Json>(std::string_view component)> catalog_entry;
    // The catalog entry's compatibility text, "" when it can install here.
    std::function<std::string(const Json& entry)> check_compatibility;
    std::function<bool()> hf_offline_env;
    // "ask", "always" or "never".
    std::function<std::string()> hf_access_policy;
    // Throws; describe_download_error turns the failure into the sentence the user reads.
    std::function<void(std::string_view model, const CancelToken& cancel)> download_model;
    std::function<std::string(std::string_view model, const std::exception_ptr& error)>
        describe_download_error;
    std::function<void(std::string_view component, const Json& entry, const CancelToken& cancel)>
        install_component;
    // Why the speaker model may not download now, or "".
    std::function<std::string()> speaker_model_download_blocked;
    std::function<void(const CancelToken& cancel)> ensure_speaker_model;

    // check_audio: the saved microphone order (load_priority).
    std::function<std::vector<audio::DeviceKey>()> mic_priority;
};

// The services setup_deps() wires the seams to; they must outlive the returned SetupDeps.
struct SetupServices {
    const Paths& paths;
    const EnvReader& env;
    SettingsStore& settings;
    http::Client& client;
    models::SpeechModelCache& speech_models;
    models::ComponentCoordinator& components;
    models::HuggingFaceAccessCoordinator& hf_access;
    audio::Backend& audio;
};
SetupDeps setup_deps(const SetupServices& services);

// Whether the user finished meeting setup (meeting_setup_complete).
bool setup_complete(const Json& settings);

struct ModelSetup {
    // How the meeting speech model reads ("Parakeet TDT 0.6B v3", "Remote speech engine").
    std::string label;
    // The model to download, or "".
    std::string speech_download;
    // The speech runtime component to install, or "".
    std::string runtime;
    bool speaker_download = false;

    // What is still missing, in order: the label, "speech runtime", "speaker identification
    // model".
    std::vector<std::string> missing() const;
    friend bool operator==(const ModelSetup&, const ModelSetup&) = default;
};

// What the meeting needs on this computer: the speaker model when speaker identification is not
// off and none is found; for a remote speech source nothing else; else the meeting model when it
// is not cached and its runtime when it is not installed.
ModelSetup inspect_models(const SetupDeps& deps);

// Downloads and installs what inspect_models reports missing, through the caches and
// coordinators the rest of the app uses, after the user's click; returns inspect_models again.
// Throws std::runtime_error with the sentence to show: "The selected speech runtime is
// unavailable on this computer.", the compatibility text, "Speech model downloads are disabled in
// Downloads settings.", "The speech model is already downloading. Try again when it finishes.",
// the download's description, "The speech runtime is already installing. Try again when it
// finishes.", the speaker model's block reason. `status` hears "Downloading <label>…",
// "Installing the speech runtime…", "Downloading the speaker identification model…".
ModelSetup prepare_models(const SetupDeps& deps, const CancelToken& cancel,
                          const std::function<void(std::string)>& status);

inline constexpr std::string_view kSignalDetected = "Signal detected";

struct AudioCheck {
    // "Signal detected", "No signal detected", "No audio received" or "Could not open".
    std::string microphone;
    // The same, or "Screen & System Audio Recording permission needed".
    std::string system_audio;

    bool ready() const { return microphone == kSignalDetected && system_audio_verified(); }
    bool system_audio_verified() const { return system_audio == kSignalDetected; }
    friend bool operator==(const AudioCheck&, const AudioCheck&) = default;
};

// Listens to the microphone (find_mic_device over the saved order) and, when allowed, to system
// audio (WASAPI loopback of the default render endpoint) for up to `duration`, ending early once
// every open device has shown signal and polling `cancel` every 0.1 s. Nothing opens when
// `cancel` fired first; a device that cannot be opened reads "Could not open" and never throws.
// Devices stop in the reverse of their opening order. One check at a time in the process: a second
// throws std::runtime_error "An audio check is still closing. Try again in a moment."
AudioCheck check_audio(audio::Backend& backend, const SetupDeps& deps, const CancelToken& cancel,
                       bool system_audio_allowed = true,
                       std::chrono::milliseconds duration = kAudioCheckDuration);

} // namespace fv::meeting
