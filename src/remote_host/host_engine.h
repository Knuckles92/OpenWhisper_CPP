#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "settings/store.h"
#include "speech/service.h"

// What a host serves: a thin view of its selected transcription engine
// (services/remote_asr/engines.py). host_engine_for wraps the backend the host has selected; a
// paired computer's window decodes then queue on this computer's speech thread with its own
// dictation.
namespace fv::remote_host {

// The interface the host calls.
// Affinity: thread-safe; transcribe blocks until the decode is done, so it runs on a worker
// thread, never the network reactor or the GUI thread.
class HostEngine {
public:
    virtual ~HostEngine() = default;
    // Changes when the host switches engine or model, and clients then reconnect: Python's
    // identity tuple, every member as a string. A request's vocabulary never enters it.
    virtual std::vector<std::string> identity() const = 0;
    // Whether transcribe's `phrases` reach a speech model that uses them; advertised to clients
    // as capabilities.recognition_hints.
    virtual bool accepts_phrases() const { return false; }
    // {family, model, label, device, streaming, available, status}.
    virtual Json describe() const = 0;
    // One window of 16 kHz mono audio: {"text", "segments": [{"text", "start", "end"}]}.
    // `language` nullopt (or empty) leaves the engine's own setting in charge. Throws what the
    // engine reports; the host sends its what() as the reply's error.
    virtual Json transcribe(std::span<const float> audio, std::optional<std::string> language,
                            std::span<const std::string> phrases) = 0;
    // A live stream's next audio: {"events": [...]}. std::runtime_error "The host's engine has no
    // live stream" unless the engine has one.
    virtual Json stream(std::string_view session, std::span<const float> audio,
                        std::optional<std::string> language, bool finish);
    virtual void cancel_stream(std::string_view /*session*/) {}
};

// An engine the host can't serve, for `reason`: identity ("unavailable", reason), describe()
// unavailable with the reason as its status, transcribe throws std::runtime_error(reason).
std::shared_ptr<HostEngine> unavailable_engine(std::string reason);

// What a SpeechServiceEngine reads of the engine this computer has loaded for dictation
// (OpenWhisper's LocalSpeechBackend attributes). Every function is called on the caller's
// thread, on each identity() or describe(), and must be cheap and thread-safe.
struct SpeechServiceEngineDeps {
    speech::SpeechService& speech;
    // local_asr_devices and local_asr_language, read for every identity().
    SettingsStore& settings;
    std::function<std::string()> family; // the backend id: "parakeet"
    std::function<std::string()> model;  // the catalog key: "parakeet-v3"
    std::function<std::string()> label;  // "Parakeet TDT 0.6B v3"
    std::function<std::string()> device; // what it runs on: "cpu" or "cuda"
    std::function<std::string()> device_info;
    std::function<bool()> available;
};

// The dictation engine of this computer's SpeechService, shared with its own dictation: a
// client's window waits in the RemoteClient lane, behind this computer's final decodes. Its
// identity is ("worker", family, model, device, the saved device, the saved language), so a
// saved change from "auto" to "cuda" disconnects clients even when the device it runs on stays
// the same. Phrases never reach it: Parakeet applies the dictionary after decoding.
std::shared_ptr<HostEngine> speech_service_engine(SpeechServiceEngineDeps deps);

// The engine to serve for the backend this computer has selected (a03's selected_model):
// a local backend (models::backends(): "parakeet") serves `deps`'s engine; "remote" is unavailable,
// "The host is itself using a remote engine. Select a local engine there."; "" is "No transcription
// engine is selected on the host"; anything else runs in the cloud, "The host's selected engine
// runs in the cloud and can't be shared. Select a local engine there.". The caller keeps the result
// for as long as the same backend stays selected.
std::shared_ptr<HostEngine> host_engine_for(std::string_view backend, SpeechServiceEngineDeps deps);

} // namespace fv::remote_host
