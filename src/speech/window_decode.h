#pragma once

#include <functional>
#include <span>
#include <string>

#include "engine/transcript.h"

// How one decoding window becomes text (transcriber/optional_backend.py 459-490), shared by the
// local backend and the service's window-by-window decode.
namespace fv::speech {

// A window whose peak is at or below this is digital silence and never decoded. Compared in
// float32, as NumPy 2 compares a float32 peak with a Python float.
inline constexpr float kSilencePeak = 0.00025f;

// Whether a window rises above digital silence (_audible): not empty, and its largest absolute
// sample above kSilencePeak.
bool audible(std::span<const float> audio);

// A transcript from its windows' texts (join_texts): each stripped of Python whitespace, the
// empty ones dropped, the rest joined with one space.
std::string join_texts(std::span<const std::string> texts);

// Decodes one 16 kHz window of audio, at most dsp::MAX_SAMPLES long.
using WindowRecognizer = std::function<Transcript(std::span<const float>)>;

// _transcribe_audio: `audio` (16 kHz) is cut with dsp::split_point, so no piece is longer than
// 30 s; a piece that is not audible() is skipped; the rest are recognized in order, their segment
// and word times moved by the piece's start (samples / 16000), and their texts joined with
// join_texts. `check_cancel` runs before each piece and throws to stop. audio_processed is the
// whole audio's length in seconds.
Transcript transcribe_audio(std::span<const float> audio, const WindowRecognizer& recognize,
                            const std::function<void()>& check_cancel);

} // namespace fv::speech
