#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "audio/pcm_tape.h"
#include "base/clock.h"
#include "base/executor.h"
#include "speech/service.h"

// The approximate preview shown while a dictation is recorded (services/streaming_transcriber.py
// StreamingTranscriber): overlapping windows of the recording, decoded on the Preview lane.
namespace fv::speech {

// Joins window text, removing only a bounded matching phrase at the seam (append_preview_text):
// `chunk_text` is stripped; with max_overlap_words of 2 or more, the longest run of words (at
// most max_overlap_words, always leaving one new word) that ends the last 512 code points of
// `existing` and starts the first 512 of the chunk is dropped from the chunk. Words are
// \w+(?:['’]\w+)* compared casefolded with ’ read as '. Single words and wholly repeated chunks
// are ambiguous without word times, so they stay.
std::string append_preview_text(std::string_view existing, std::string_view chunk_text,
                                int max_overlap_words = 0);

// Recorder PCM to 16 kHz float32 mono (prepare_preview_audio): int16 scaled by 1/32768,
// interleaved channels averaged in float32, then fft_resample to int(frames * 16000 / rate)
// samples when the rate is not 16 kHz; nullopt when that count is 0.
std::optional<std::vector<float>> prepare_preview_audio(std::span<const std::int16_t> interleaved,
                                                        int channels, int sample_rate);
std::optional<std::vector<float>> prepare_preview_audio(std::span<const float> interleaved,
                                                        int channels, int sample_rate);

struct WindowPreviewOptions {
    // STREAMING_CHUNK_DURATION_SEC: seconds of new audio per window.
    double chunk_duration_s = 3.0;
    // STREAMING_OVERLAP_SEC: seconds of the last window's new audio decoded again in front of the
    // next one.
    double overlap_s = 0.75;
    // CHUNK_SIZE: a window grows by whole recorder blocks of this many frames, as OpenWhisper's
    // worker took them from its queue, so its boundaries are the same.
    int block_frames = 1024;
    // STREAMING_QUEUE_SEC and STREAMING_QUEUE_SIZE: audio held for the next windows while one
    // decodes is max(buffer_blocks, ceil(buffer_s * rate / block_frames)) blocks; blocks recorded
    // past that during a decode are skipped, so the preview never falls further behind.
    double buffer_s = 3.0;
    int buffer_blocks = 10;
    // How often the tape is checked for a completed window (the worker's queue poll).
    std::chrono::milliseconds poll{100};
};

namespace detail {
struct PreviewState;
}

// Follows a recording's PcmTape and decodes a window every chunk_duration_s of new audio: the
// overlap tail plus the new audio, through prepare_preview_audio (fft_resample, never
// Resampler16k), on Lane::Preview with coalesce_key "preview:<job id>" and language "auto". One
// window decodes at a time. Texts join with append_preview_text (overlap words min(12,
// ceil(overlap_s * 8)) once a window with text and a tail exist), and each joined text reaches
// the callback as (text, true) on the UI executor, while still recording. A window is skipped
// while the dictation engine is not Ready; a window at or below digital silence is never sent to
// the engine and counts as one without text.
//
// stop() never decodes: the audio it leaves (the window being gathered and the blocks after it,
// up to the stop) is decoded only by finalize(), which the dictation calls when its final
// transcript is empty.
//
// Affinity: UI actor (`ui`), except finalize(), which blocks and runs on worker threads only.
// A live window is copied off the tape on the UI thread, then resampled and submitted on a
// short-lived thread of its own, never the UI thread; the destructor joins it. The service,
// executor and clock must outlive the preview.
class WindowPreview {
public:
    WindowPreview(SpeechService& service, Executor& ui, Clock& clock,
                  WindowPreviewOptions options = {});
    // Discards any work still running; its result is dropped. Does not wait for a decode, only
    // for a speech thread already delivering a window's result (it calls the executor and the
    // clock), so that once the destructor returns no thread reaches either for this preview.
    ~WindowPreview();
    WindowPreview(const WindowPreview&) = delete;
    WindowPreview& operator=(const WindowPreview&) = delete;

    // Starts following `tape` from its first frame. Refused, with a warning, while a recording
    // is followed, finalize() decodes, or a window decode of an earlier recording still runs.
    void start(std::shared_ptr<const audio::PcmTape> tape, std::string job_id,
               std::function<void(std::string text, bool final)> callback);
    // Stops without waiting or decoding and returns the text so far ("" when not running). The
    // window decoding now still lands in the text for finalize(), but no callback runs after
    // stop. Frames appended after the stop are not the recording's preview.
    std::string stop();
    // Waits up to `timeout` for a window still decoding, then decodes what stop() left, with
    // the windowing the live preview would have used, and returns the complete text (stripped).
    // While running, or once finalized, returns the text so far without decoding. Past the
    // deadline the late output is ignored and the text so far returned. nullopt when cancel(),
    // cleanup(), a new start(), an earlier deadline or the preview's destruction took the audio;
    // cancel() and the destruction also end its wait and its decode: a window still queued is
    // dropped at once, one the engine is decoding ends with that window. Worker threads only.
    std::optional<std::string>
    finalize(std::chrono::milliseconds timeout = std::chrono::seconds(5));
    // Discards the text, the kept audio and any late output without waiting for a decode.
    void cancel();
    // cancel(), logged.
    void cleanup();

    bool running() const;
    // Whether a window's decode is in flight (OpenWhisper's worker thread still alive).
    bool decoding() const;
    // The joined text so far, unstripped.
    std::string preview_text() const;
    // Windows processed since start(), quiet ones included.
    std::size_t chunk_count() const;

private:
    std::shared_ptr<detail::PreviewState> state_;
};

} // namespace fv::speech
