#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "audio/file_decoder.h"
#include "audio/resampler.h"
#include "base/cancel.h"
#include "dsp/window_splitter.h"
#include "engine/recognizer.h"
#include "speech/service.h"

namespace fv::speech {

// A file's decoding windows (services/local_asr/audio.py windows()): every frame of its first
// audio stream, timestamp cleared, through one Resampler16k (swresample to s16 mono 16 kHz),
// flushed after the last frame, cut by dsp::WindowSplitter. Each window is a 16 kHz AudioClip of
// at most 30 s whose offset_s is where it starts in the file. Reads only as far as the next
// window needs, so a long file is never held whole.
// Affinity: one thread at a time; blocks on file I/O.
class FileWindows {
public:
    // Throws EngineError (the decoder's sentence) when the file cannot be opened.
    explicit FileWindows(const std::filesystem::path& path);
    ~FileWindows();
    FileWindows(FileWindows&&) noexcept;
    FileWindows& operator=(FileWindows&&) noexcept;

    // The next window; nullopt after the last. Throws EngineError on corrupt data.
    std::optional<AudioClip> next();

private:
    std::unique_ptr<audio::AudioFileDecoder> decoder_;
    audio::Resampler16k resampler_;
    dsp::WindowSplitter splitter_;
    std::deque<AudioClip> ready_;
    std::vector<std::int16_t> pcm_;
    bool finished_ = false;
};

// Every window of the file, in order.
std::vector<AudioClip> file_windows(const std::filesystem::path& path);

// A file decoded on `service` one window at a time (each window its own job on `lane`, the next
// one read from the file while the previous decodes), as LocalSpeechBackend.transcribe: the
// windows' texts joined with join_texts, their segments and words moved to file time. The
// result's generation is the first window's; a reload between windows ends the decode with
// "Transcription canceled", as does `cancel`. Blocking: a worker thread, never the GUI.
DecodeResult decode_file(SpeechService& service, const std::filesystem::path& path,
                         const RecognitionOptions& options, Lane lane = Lane::Final,
                         const CancelToken& cancel = {}, Slot slot = Slot::Dictation);

} // namespace fv::speech
