#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/json.h"
#include "meeting_capture/capture_block.h"

// meeting/asr/preview.py WindowSpeechPreview: best-effort live captions on the Parakeet window
// path. Every second of new audio on a channel, the channel's last eight seconds are decoded and
// the text is reported as a preview event; durable chunk transcription always comes first.
// Capture only copies into a bounded queue, slow decodes lower the preview rate instead of
// queueing speculative work, and dropped previews never touch the spool.
namespace fv::meeting {

// Seconds of new audio on a channel before it is decoded again, and the audio window decoded.
inline constexpr double PREVIEW_INTERVAL_S = 1.0;
inline constexpr double PREVIEW_WINDOW_S = 8.0;
// A block further than this from its channel's previous one, in meeting or monotonic time,
// starts a new window.
inline constexpr double PREVIEW_MEDIA_GAP_S = 0.15;
inline constexpr double PREVIEW_WALL_GAP_S = 0.25;
// Capture blocks queued for the preview; more are dropped (preview data only).
inline constexpr std::size_t PREVIEW_QUEUE_BLOCKS = 128;
// The next decode waits at least this, and three times the last decode's time (at most 25 % of
// the model's time goes to previews, across both channels).
inline constexpr double PREVIEW_MIN_DELAY_S = 0.1;
inline constexpr double PREVIEW_STOP_JOIN_S = 2.0;

// One decode of a channel's window (16 kHz float): its text, or nullopt when it was skipped
// (durable work outstanding, or the decode was cancelled). Anything it throws ends the preview.
// The token is the preview's own: stop() cancels it.
using PreviewDecode = std::function<std::optional<std::string>(
    std::string_view channel, std::span<const float> samples, const CancelToken& stop)>;
// {"channel", "text", "start_s", "end_s", "final": false}, in meeting seconds.
using PreviewCallback = std::function<void(const Json& preview)>;

// The consumer's state: _accept and _decode, with no thread, so tests drive it step by step.
// Affinity: one thread at a time (the preview thread).
class PreviewWindows {
public:
    PreviewWindows(PreviewDecode decode, PreviewCallback callback, std::function<bool()> busy,
                   const Clock& clock);

    // Adds a block's audio (prepared for the decoder) to its channel's window, starting a new
    // window after a gap. start_s is the block's meeting time, t_mono its monotonic seconds.
    void accept(std::string_view channel, std::span<const std::int16_t> frames, int rate,
                double t_mono, double start_s);
    // Decodes the channel whose window waited longest, once at least PREVIEW_INTERVAL_S of new
    // audio arrived, unless busy() holds or the last decode's delay has not passed. A window
    // that is near silence counts as decoded without a decode. `stop` is handed to the decode,
    // and no event is reported once it is cancelled.
    void decode(const CancelToken& stop = {});
    // Lets the next decode() run at once (the decode delay of the last one is dropped).
    void allow_decode_now() { next_decode_ = 0.0; }

private:
    struct Session {
        std::vector<float> audio; // the last PREVIEW_WINDOW_S seconds at 16 kHz
        double end = 0.0;         // meeting time where the audio ends
        double decoded = 0.0;     // meeting time up to which the channel was decoded
        double wall_end = 0.0;    // monotonic time where the audio ends
    };

    double now() const;

    PreviewDecode decode_;
    PreviewCallback callback_;
    std::function<bool()> busy_;
    const Clock& clock_;
    // Channels in the order they first appeared (Python's dict keeps a replaced key's place).
    std::vector<std::pair<std::string, Session>> sessions_;
    double next_decode_ = 0.0;
};

struct PreviewOptions {
    std::size_t queue_blocks = PREVIEW_QUEUE_BLOCKS;
    double stop_join_s = PREVIEW_STOP_JOIN_S;
    // Tests only: false queues blocks without a consumer thread.
    bool start_thread = true;
};

// A copy of one queued block.
struct PreviewBlock {
    std::string channel;
    std::vector<std::int16_t> frames;
    int rate = 0;
    double t_mono = 0.0;
    double start_s = 0.0;
};

// The preview with its queue and its thread "fv-meeting-preview".
// Affinity: feed() on capture device threads (several producers; it never blocks and allocates
// only for a channel name longer than 31 bytes); stop() on any thread. The decode and the
// callback run on the preview thread.
class WindowSpeechPreview {
public:
    WindowSpeechPreview(PreviewDecode decode, PreviewCallback callback, std::function<bool()> busy,
                        const Clock& clock, PreviewOptions options = {});
    // Stops, then waits for the thread however long its decode takes.
    ~WindowSpeechPreview();
    WindowSpeechPreview(const WindowSpeechPreview&) = delete;
    WindowSpeechPreview& operator=(const WindowSpeechPreview&) = delete;

    // Copies the block into the queue; dropped when the queue is full or the preview stopped.
    // A block wider than one queue slot takes several, each stamped where its first sample falls.
    void feed(const CaptureBlock& block, double start_s) noexcept;
    // Cancels the running decode and waits up to stop_join_s for the thread. True when it ended.
    bool stop();

    std::size_t queued() const;
    // Tests only, while no thread consumes: the oldest queued block.
    std::optional<PreviewBlock> take();

private:
    class Queue;
    void run();

    PreviewWindows windows_;
    PreviewOptions options_;
    std::unique_ptr<Queue> queue_;
    CancelSource stop_source_;
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool finished_ = false; // guarded by mutex_
    std::thread thread_;
};

} // namespace fv::meeting
