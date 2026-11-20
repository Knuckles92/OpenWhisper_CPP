#pragma once

#include <string>
#include <string_view>

// Sentences the speech layer reports, shared by the service, the backends and their callers.
namespace fv::speech {

// What a cancelled, replaced or superseded decode ends with (EngineError's message).
inline constexpr std::string_view kTranscriptionCanceled = "Transcription canceled";

// What a decode reports when the speech engine died under it: OpenWhisper's worker-crash
// sentence (local_asr/process.py), naming this app's log file. In process a native crash ends
// the app, so only the crash-recovery path after a restart shows it.
std::string engine_stopped_message();

} // namespace fv::speech
