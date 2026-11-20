#pragma once

#include <chrono>
#include <set>
#include <string>
#include <string_view>

#include "base/json.h"
#include "models/catalog.h"

// What the Voice model page says about the dictation engine, as plain functions over a settings
// snapshot (ui_qt/dialogs/settings_models.py: ModelAssignments.voice_summary, voice_detail,
// voice_is_remote, voice_is_cloud, _update_engine_caption, _engine_filter,
// _refresh_engine_inventory, _check_engine_runtime and the footnote). The rail, Home and the page
// itself read the same functions, so the three always agree.
//
// OpenWhisper's Whisper branches are gone: the engines are "api", "parakeet" and "remote".
namespace fv {

class Clock;

// _ENGINE_CAPTIONS's sentences for the two engines that run elsewhere; the caption of a local
// engine is its model's purpose (engine_caption).
inline constexpr std::string_view kApiEngineCaption =
    "OpenAI, Groq, or an OpenAI-compatible speech server you run. Hosted services need an API "
    "key in Settings → App → API keys.";
inline constexpr std::string_view kRemoteEngineCaption =
    "Another computer's engine, over your network. Pair with it in Settings → Remote engine.";

// The footnote card under "On this computer". OpenWhisper's names Nemotron, Moonshine and Whisper
// beside Parakeet; the only live preview here is Parakeet's windowed decode.
inline constexpr std::string_view kVoiceFootnote =
    "For dictation previews, Parakeet transcribes short audio chunks with the loaded model. The "
    "final transcript uses your selected model.";

// The inventory line when the lookup of what is on this computer failed.
inline constexpr std::string_view kInventoryFailed =
    "Open Downloads to see which models are on this computer.";
// The inventory line's tail when the runtime could not be looked up.
inline constexpr std::string_view kRuntimeCheckFailed = ". Open Downloads to check its runtime.";

// _engine_value without a built page: the saved selected_model ("api", "parakeet" or "remote").
std::string voice_engine(const Json& settings);

// The dictation engine and model as the rail shows it: the API server and its model ("Groq ·
// whisper-large-v3-turbo"), "Remote · {host}" or "Remote · not paired", else the selected speech
// model's label. `today` is the local date the OpenAI retirement dates are judged by; without
// it, the system clock's.
std::string voice_summary(const Json& settings);
std::string voice_summary(const Json& settings, std::chrono::year_month_day today);

// Where dictation runs, for Home: "On this computer · {device}", "On this computer · {server}",
// "Sent to {destination} for transcription", "Sent to {host} on your network" or "Pair with a
// host in Remote engine". `device_info` replaces the saved device ("auto", "cpu", "cuda") of a
// local engine with a better text the caller has (the resolved device); empty keeps the saved
// device.
std::string voice_detail(const Json& settings, std::string device_info = {});

// True when dictation audio leaves this computer: a paired computer, or an API server that is
// not on this one.
bool voice_is_remote(const Json& settings);
// True when dictation audio goes to an API server off this computer.
bool voice_is_cloud(const Json& settings);

// The NeMo runtime as Home's footer names it: the saved device (Auto, CPU or GPU), the runtime
// build chosen with fv_speech_runtime ("CPU runtime", "GPU runtime", or none chosen yet) and
// whether it is loaded in this process: "Auto · CPU runtime loaded", "Auto · CPU runtime not
// loaded", "Auto · no runtime yet". OpenWhisper's was the Whisper device and quantization, which
// OpenWhisper_CPP does not have.
std::string runtime_summary(const Json& settings, bool loaded);

// The caption under the engine combo: the engine's default model's purpose and ". Runs locally."
// for a local engine, else its fixed sentence; "" for an engine with neither.
std::string engine_caption(std::string_view backend);

// _engine_filter: the Downloads filter that shows what the engine uses ("" for an engine whose
// audio goes elsewhere).
std::string engine_filter(std::string_view backend);

// Whether anything of the engine is kept on this computer (false for "api" and "remote"): the
// "On this computer" title and row show only then.
bool engine_is_local(std::string_view backend);

// _refresh_engine_inventory's first half: "{n} of {m} {Backend} model(s) on this computer", n
// counting the models of the backend whose key is in `cached`; "" for an engine that keeps
// nothing here. Throws std::out_of_range for a backend the catalog does not have.
std::string engine_inventory_count(std::string_view backend, const std::set<std::string>& cached);

// _check_engine_runtime: the tail of the inventory line for what `backend` runs on at `device`
// ("auto", "cpu" or "cuda"): " · {runtime name} installed." or " · {runtime name} not
// installed.", or kRuntimeCheckFailed when the runtime cannot be resolved. Before a result is
// known the tail is ".".
std::string engine_runtime_label(std::string_view backend, std::string_view device,
                                 const models::Host& host);

// The same, for the runtime the Parakeet engine will load as models::resolve_parakeet resolves
// it from `settings`: a runtime build chosen with fv_speech_runtime is the one named, whatever
// the device, and a runtime folder chosen with fv_runtime_dir stands in for it, so it reads as
// installed when the folder holds the runtime library of a build that serves it
// (models::runtime_dir_serves: a CPU build reads as no GPU runtime). Reads the disk. Without
// either, and for another backend, engine_runtime_label above.
std::string engine_runtime_label(const Json& settings, std::string_view backend,
                                 std::string_view device, models::Host host);

// The complete inventory line as the page shows it: the count, then `runtime_label` (".", or what
// engine_runtime_label gave). "" for an engine that keeps nothing here.
std::string engine_inventory_text(std::string_view backend, const std::set<std::string>& cached,
                                  std::string_view runtime_label);

// --- Meeting voice (meeting_model_label, meeting_voice_summary, meeting_voice_detail,
// speaker_id_is_remote, _refresh_meeting_runtime_label, speaker_id_label and
// meeting_language_label). The Voice & speakers page saves every choice as it is made, so the
// saved settings are what its controls show.

// speaker_id_label: "Me / Others labels" for "off", "OpenAI speaker labels" for "openai", else
// "On-device speaker labels".
std::string speaker_id_label(std::string_view backend);

// meeting_language_label: the language's name in MeetingLanguage.CHOICES ("Detect
// automatically", "English", ...), else the code itself.
std::string meeting_language_label(std::string_view code);

// The meeting speech model as the rail and Home name it: "Remote computer" when meetings go to
// the paired computer, else the model's label (resolve_meeting_speech).
std::string meeting_model_label(const Json& settings);

// The rail's value under Voice & speakers: "{model} · {language}".
std::string meeting_voice_summary(const Json& settings);

// Home's line under Voice & speakers: "{language} · {speaker labels}", with " · audio sent to
// paired host" when meetings go to the paired computer. The speaker backend is the one a meeting
// uses (resolve_meeting_speaker_id_backend on the local date of `clock`).
std::string meeting_voice_detail(const Json& settings, const Clock& clock);
std::string meeting_voice_detail(const Json& settings);

// Whether speaker labels send audio off this computer (the OpenAI pass). A stored "openai" reads
// as on-device here, so this is false until that pass exists.
bool speaker_id_is_remote(const Json& settings, const Clock& clock);
bool speaker_id_is_remote(const Json& settings);

// The sentence under the meeting's speech fields (_refresh_meeting_runtime_label): what a remote
// meeting sends where, what a following meeting uses ("Meetings use {label}, the same as
// dictation." or the note of an engine that can't run meetings, then "Change the dictation engine
// on Dictation → Voice model."), or the local model a separate engine runs with its device
// preference. OpenWhisper's Whisper branch (Runtime's device and quantization) has no model here
// to reach it.
std::optional<std::string> meeting_preferred_host(const Json& settings);
std::string meeting_runtime_sentence(const Json& settings);

} // namespace fv
