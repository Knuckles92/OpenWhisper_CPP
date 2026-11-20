#pragma once

#include <string>
#include <string_view>

#include "base/clock.h"
#include "base/json.h"
#include "settings/store.h"

// Which engine and model transcribe a meeting (services/settings.py:1774-1916, 1997-2010).
// Meetings follow dictation unless they were given an engine of their own. OpenWhisper's Local
// Whisper branches are not here: OpenWhisper_CPP's local engine is Parakeet, so a meeting that
// cannot follow dictation falls back to the Parakeet default model where OpenWhisper's Windows
// build falls back to Whisper "auto".
namespace fv::models {

struct MeetingSpeech {
    // "local" or "remote" (the paired computer).
    std::string source;
    // The local model. With a remote source the host's engine decides, and this is only what a
    // local run of the meeting would use.
    std::string model;
    // The meeting has an engine of its own rather than dictation's.
    bool separate = false;
    // Why a meeting that follows dictation is not on the dictation engine.
    std::string note;

    bool operator==(const MeetingSpeech&) const = default;
};

// How a meeting speech model reads in a sentence: the catalog's label ("Parakeet TDT 0.6B v3"),
// or the name itself for a model the catalog does not have (OpenWhisper's "Local Whisper ...").
std::string meeting_model_label(std::string_view model);

// The engine a meeting uses: dictation's, unless meeting_speech_separate gave it its own. An
// engine that can't transcribe meetings (the API, or a model without the catalog's `meeting`
// flag) leaves a following meeting on the default Parakeet model, with `note` "<name> can't
// transcribe meetings, so meetings use <label>." `settings` is a settings snapshot.
MeetingSpeech resolve_meeting_speech(const Json& settings);

// The engine saved for meetings alone, whether or not it is in use.
MeetingSpeech own_meeting_speech(const Json& settings);

// resolve_meeting_speech(settings).source and .model.
std::string resolve_meeting_asr_source(const Json& settings);
std::string resolve_meeting_whisper_model(const Json& settings);

// The local model meetings keep busy (settings_downloads.py `_models_in_use`): the meeting model,
// or "" when meetings use a paired computer's speech and no local download serves them.
std::string meeting_model_in_use(const Json& settings);

// Saves whether this install's meetings have their own engine, once; true when written. An
// existing install whose saved meeting engine differs from its dictation engine keeps it as a
// separate engine; a new install and every other one follow dictation. Once the key is saved it
// is never decided again (false, nothing written). Run after keep_unchosen_engine. A failed
// write is logged and reads false.
bool keep_separate_meeting_speech(SettingsStore& store);

// The speaker-identification backend a meeting uses: "off" or "local". A stored "openai" reads
// as "local" on every date: OpenWhisper_CPP has no OpenAI speaker pass (OpenWhisper switches it on
// OpenAI's shutdown day, judged by `clock`'s local date). Anything invalid reads as the default.
std::string resolve_meeting_speaker_id_backend(const Json& settings, const Clock& clock);

} // namespace fv::models
