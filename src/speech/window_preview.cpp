#include "speech/window_preview.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <expected>
#include <mutex>
#include <thread>
#include <utility>

#include "base/log.h"
#include "dsp/fft_resample.h"
#include "engine/error.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "speech/messages.h"
#include "speech/window_decode.h"

namespace fv::speech {

namespace {

constexpr log::Logger kLog{"services.streaming_transcriber"};
constexpr int kTargetRate = 16000;     // WHISPER_TARGET_SAMPLE_RATE
constexpr std::size_t kLookBack = 512; // code points of each side compared at a seam
// Words of the previous window the next one may repeat: about eight a second of overlap,
// capped.
constexpr double kOverlapWordsPerSec = 8.0;
constexpr int kMaxOverlapWords = 12;
// A window slower than this is falling behind; warn after a few, rarely.
constexpr double kSlowChunkSec = 5.0;
constexpr std::size_t kSlowChunksToWarn = 3;
constexpr std::chrono::seconds kSlowWarningInterval{30};

const py::Regex& preview_word() {
    static const py::Regex regex(R"(\w+(?:['’]\w+)*)");
    return regex;
}

std::string normalize(const py::Match& word) {
    std::string folded = py::casefold(word.group());
    static constexpr std::string_view kCurly = "’"; // RIGHT SINGLE QUOTATION MARK
    for (std::size_t at = folded.find(kCurly); at != std::string::npos;
         at = folded.find(kCurly, at + 1)) {
        folded.replace(at, kCurly.size(), "'");
    }
    return folded;
}

// The resampling half of prepare_preview_audio, on float32 mono.
std::optional<std::vector<float>> resample_mono(std::vector<float> mono, int sample_rate) {
    if (sample_rate != kTargetRate) {
        // int(len * 16000 / rate): the product is exact, the division a double.
        const auto count = static_cast<std::int64_t>(
            static_cast<double>(static_cast<std::int64_t>(mono.size()) * kTargetRate) /
            sample_rate);
        if (count <= 0) {
            return std::nullopt;
        }
        return dsp::fft_resample(mono, static_cast<std::size_t>(count));
    }
    return mono;
}

// audio_array.mean(axis=1) of float32 frames: a float32 sum, divided by the count as numpy 2
// divides a float32 array by an intp scalar (in double, stored as float32).
std::vector<float> mix_down(std::span<const float> interleaved, int channels) {
    if (channels <= 1) {
        return {interleaved.begin(), interleaved.end()};
    }
    const std::size_t frames = interleaved.size() / static_cast<std::size_t>(channels);
    std::vector<float> mono(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sum = interleaved[frame * static_cast<std::size_t>(channels)];
        for (int channel = 1; channel < channels; ++channel) {
            sum += interleaved[frame * static_cast<std::size_t>(channels) +
                               static_cast<std::size_t>(channel)];
        }
        mono[frame] = static_cast<float>(static_cast<double>(sum) / channels);
    }
    return mono;
}

// Runs a posted task at once on the posting thread: a window's result is applied where the
// speech thread completes it, which needs no hop.
class DirectExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return false; }
};

DirectExecutor& direct() {
    static DirectExecutor executor;
    return executor;
}

} // namespace

std::string append_preview_text(std::string_view existing, std::string_view chunk_text,
                                int max_overlap_words) {
    std::string chunk = py::strip(chunk_text);
    if (chunk.empty()) {
        return std::string(existing);
    }
    if (existing.empty()) {
        return chunk;
    }
    if (max_overlap_words >= 2) {
        const std::string tail_text = py::slice(existing, -static_cast<std::int64_t>(kLookBack));
        const std::string head_text = py::slice(chunk, 0, static_cast<std::int64_t>(kLookBack));
        const std::vector<py::Match> tail = preview_word().finditer(tail_text);
        const std::vector<py::Match> head = preview_word().finditer(head_text);
        std::vector<std::string> tail_words;
        std::vector<std::string> head_words;
        for (const py::Match& word : tail) {
            tail_words.push_back(normalize(word));
        }
        for (const py::Match& word : head) {
            head_words.push_back(normalize(word));
        }
        // Leave at least one newly recognized word; an entire repeated phrase may be
        // intentional speech rather than the overlap being decoded twice.
        const std::int64_t maximum = std::min({static_cast<std::int64_t>(max_overlap_words),
                                               static_cast<std::int64_t>(tail_words.size()),
                                               static_cast<std::int64_t>(head_words.size()) - 1});
        for (std::int64_t count = maximum; count > 1; --count) {
            const auto n = static_cast<std::size_t>(count);
            if (std::equal(tail_words.end() - static_cast<std::ptrdiff_t>(n), tail_words.end(),
                           head_words.begin())) {
                chunk = py::slice(chunk, static_cast<std::int64_t>(head[n].start()));
                break;
            }
        }
    }
    return py::strip(std::string(existing) + " " + chunk);
}

std::optional<std::vector<float>> prepare_preview_audio(std::span<const std::int16_t> interleaved,
                                                        int channels, int sample_rate) {
    std::vector<float> scaled(interleaved.size());
    std::ranges::transform(interleaved, scaled.begin(), [](std::int16_t sample) {
        return static_cast<float>(sample) / 32768.0f;
    });
    return resample_mono(mix_down(scaled, channels), sample_rate);
}

std::optional<std::vector<float>> prepare_preview_audio(std::span<const float> interleaved,
                                                        int channels, int sample_rate) {
    return resample_mono(mix_down(interleaved, channels), sample_rate);
}

namespace detail {

// Frames [first, last) of the tape.
struct TapeRange {
    std::int64_t first = 0;
    std::int64_t last = 0;
};

// One window as the worker took it.
struct TakenWindow {
    std::uint64_t session = 0;
    std::vector<std::int16_t> new_audio; // interleaved
    std::vector<std::int16_t> audio;     // the overlap tail, then new_audio
    std::chrono::steady_clock::time_point started;
};

struct PreviewState : std::enable_shared_from_this<PreviewState> {
    PreviewState(SpeechService& service_, Executor& ui_, Clock& clock_,
                 WindowPreviewOptions options_)
        : service(service_), ui(ui_), clock(clock_), options(options_) {}

    SpeechService& service;
    Executor& ui;
    Clock& clock;
    const WindowPreviewOptions options;

    mutable std::mutex mutex;
    std::condition_variable changed; // in_flight cleared, discard set, or delivering dropped

    bool destroyed = false;
    // Speech threads delivering a window's result that found the preview alive: they still call
    // `ui` and `clock`, so the destructor waits until this is 0.
    std::size_t delivering = 0;
    std::shared_ptr<const audio::PcmTape> tape;
    std::string job_id;
    std::function<void(std::string, bool)> callback;
    std::uint64_t session = 0;

    bool streaming = false;      // is_streaming
    bool stop_requested = false; // _stop_requested
    bool discard = false;        // _discard_results
    bool finalize_pending = false;
    bool finalizing = false;
    bool in_flight = false; // a window is being processed (the worker is alive)

    std::string text; // preview_text
    std::string last_chunk_text;
    std::optional<std::vector<std::int16_t>> overlap_tail;

    // The tape as the worker's queue: blocks before `read` were taken; `gathered` holds the
    // ones of the window not yet complete; `skipped` the blocks a full queue dropped.
    std::int64_t read = 0;
    std::vector<TapeRange> gathered;
    double gathered_s = 0.0;
    std::deque<TapeRange> skipped;
    std::int64_t stop_frame = 0;

    std::size_t chunks = 0;
    std::size_t slow_chunks = 0;
    std::optional<std::chrono::steady_clock::time_point> last_warning;

    JobHandle job;
    TimerHandle timer;
    // Cancels the decode finalize() waits on (a new source per finalize): cancel() and the
    // destructor fire it, so a finalize whose window is queued behind other work returns at once
    // and one being decoded with that window, instead of decoding the rest (Quit waits for its
    // caller).
    CancelSource finalize_cancel;
    // Prepares the live window in flight and submits it; started and joined on the UI thread
    // only (pump and the preview's destructor), so it needs no lock.
    std::thread preparer;

    // --- under `mutex` ---

    int rate() const { return tape->rate(); }
    std::int64_t block() const { return options.block_frames; }

    // The next whole block at or after `read` before `limit`, past dropped blocks; a partial
    // block only when `partial`.
    std::optional<TapeRange> next_block(std::int64_t limit, bool partial) {
        while (!skipped.empty() && read >= skipped.front().first) {
            read = std::max(read, skipped.front().last);
            skipped.pop_front();
        }
        if (read + block() <= limit) {
            return TapeRange{read, read + block()};
        }
        if (partial && read < limit) {
            return TapeRange{read, limit};
        }
        return std::nullopt;
    }

    // Takes whole blocks into the window being gathered; true when they complete it.
    bool gather(std::int64_t limit) {
        while (const auto taken = next_block(limit, false)) {
            gathered.push_back(*taken);
            read = taken->last;
            gathered_s += static_cast<double>(taken->last - taken->first) / rate();
            if (gathered_s >= options.chunk_duration_s) {
                return true;
            }
        }
        return false;
    }

    // Blocks recorded while a window decoded that did not fit the worker's queue are dropped,
    // as feed_audio drops them when the queue is full. True when some were.
    bool drop_overflow(std::int64_t end) {
        const auto capacity = std::max<std::int64_t>(
            options.buffer_blocks,
            static_cast<std::int64_t>(std::ceil(options.buffer_s * rate() / block())));
        std::int64_t position = read;
        std::int64_t kept = 0;
        auto skip = skipped.begin();
        while (position + block() <= end && kept < capacity) {
            if (skip != skipped.end() && position >= skip->first) {
                position = std::max(position, skip->last);
                ++skip;
                continue;
            }
            position += block();
            ++kept;
        }
        const std::int64_t arrived = position + (end - position) / block() * block();
        if (kept == capacity && arrived > position) {
            skipped.push_back(TapeRange{position, arrived});
            return true;
        }
        return false;
    }

    // The worker's partial window and its queue up to the stop, in blocks; taken out.
    std::vector<TapeRange> take_retained() {
        std::vector<TapeRange> blocks = std::move(gathered);
        gathered.clear();
        gathered_s = 0.0;
        while (const auto taken = next_block(stop_frame, true)) {
            blocks.push_back(*taken);
            read = taken->last;
        }
        skipped.clear();
        return blocks;
    }

    void forget_retained() {
        gathered.clear();
        gathered_s = 0.0;
        skipped.clear();
        read = std::max(read, stop_frame);
    }

    // --- outside `mutex` ---

    // A window's result arrived: false when the preview is already destroyed, else the caller
    // delivers it and calls end_delivery().
    bool begin_delivery() {
        std::lock_guard lock(mutex);
        if (destroyed) {
            return false;
        }
        ++delivering;
        return true;
    }
    void end_delivery() {
        {
            std::lock_guard lock(mutex);
            --delivering;
        }
        changed.notify_all();
    }

    TakenWindow copy_window(const std::shared_ptr<const audio::PcmTape>& source,
                            const std::vector<TapeRange>& blocks,
                            const std::optional<std::vector<std::int16_t>>& tail,
                            std::uint64_t current) const {
        TakenWindow taken;
        taken.session = current;
        taken.started = clock.steady();
        const auto channels = static_cast<std::size_t>(source->channels());
        for (const TapeRange& range : blocks) {
            const std::size_t at = taken.new_audio.size();
            taken.new_audio.resize(at +
                                   static_cast<std::size_t>(range.last - range.first) * channels);
            source->view(range.first, range.last)
                .copy_to(std::span<std::int16_t>(taken.new_audio).subspan(at));
        }
        if (tail && !tail->empty()) {
            taken.audio = *tail;
        }
        taken.audio.insert(taken.audio.end(), taken.new_audio.begin(), taken.new_audio.end());
        return taken;
    }

    // The front of _process_incremental_chunk: nullopt when the window is skipped (the engine
    // is not loaded, or nothing to decode).
    std::optional<std::vector<float>> prepare(const TakenWindow& taken, int channels,
                                              int sample_rate) {
        if (service.readiness(Slot::Dictation).state != ReadyState::Ready) {
            kLog.debug("Preview window skipped: the engine is not loaded");
            return std::nullopt;
        }
        auto prepared = prepare_preview_audio(taken.audio, channels, sample_rate);
        if (!prepared || prepared->empty()) {
            return std::nullopt;
        }
        return prepared;
    }

    DecodeJob window_job(std::vector<float> prepared, Lane lane, const std::string& key) const {
        DecodeJob decode;
        AudioClip clip;
        clip.end = prepared.size();
        clip.samples = std::make_shared<const std::vector<float>>(std::move(prepared));
        decode.input = std::move(clip);
        // SpeechDecoder.transcribe's default: the preview detects the language.
        decode.options.language = "auto";
        decode.lane = lane;
        decode.coalesce_key = key;
        decode.slot = Slot::Dictation;
        return decode;
    }

    // The back of _process_incremental_chunk for a decoded window's text.
    void apply_text(const TakenWindow& taken, const std::string& chunk_text) {
        std::string joined;
        std::function<void(std::string, bool)> deliver;
        std::size_t number = 0;
        bool warn = false;
        double new_s = 0.0;
        double total_s = 0.0;
        double processing_s = 0.0;
        std::uint64_t current = 0;
        {
            std::lock_guard lock(mutex);
            if (discard || destroyed || taken.session != session) {
                return;
            }
            const int overlap_words =
                (overlap_tail && !last_chunk_text.empty())
                    ? std::min(kMaxOverlapWords,
                               static_cast<int>(std::ceil(options.overlap_s * kOverlapWordsPerSec)))
                    : 0;
            text = append_preview_text(text, chunk_text, overlap_words);
            last_chunk_text = chunk_text;

            const auto channels = static_cast<std::size_t>(tape->channels());
            const auto overlap_frames = static_cast<std::int64_t>(options.overlap_s * rate());
            const auto frames = static_cast<std::int64_t>(taken.new_audio.size() / channels);
            if (overlap_frames > 0 && frames > 0) {
                const auto kept =
                    static_cast<std::size_t>(std::min(overlap_frames, frames)) * channels;
                overlap_tail.emplace(taken.new_audio.end() - static_cast<std::ptrdiff_t>(kept),
                                     taken.new_audio.end());
            } else {
                overlap_tail.reset();
            }

            ++chunks;
            number = chunks;
            new_s = static_cast<double>(frames) / rate();
            total_s = static_cast<double>(taken.audio.size() / channels) / rate();
            const auto now = clock.steady();
            processing_s = std::chrono::duration<double>(now - taken.started).count();
            if (processing_s > kSlowChunkSec) {
                ++slow_chunks;
                if (slow_chunks >= kSlowChunksToWarn &&
                    (!last_warning || now - *last_warning > kSlowWarningInterval)) {
                    warn = true;
                    last_warning = now;
                }
            }
            if (!stop_requested && callback && !text.empty()) {
                deliver = callback;
                joined = text;
                current = session;
            }
        }
        kLog.info("Incremental transcription #{}: {}s new (+{}s overlap) -> {}s processing ({} "
                  "chars)",
                  number, py::format_fixed(new_s, 1), py::format_fixed(total_s - new_s, 1),
                  py::format_fixed(processing_s, 2), py::len(chunk_text));
        if (warn) {
            kLog.warning("Incremental transcription falling behind (3+ slow chunks)");
        }
        if (deliver) {
            // After stop the text is kept for finalize only; the preview overlay is gone.
            ui.post([weak = weak_from_this(), current, joined = std::move(joined)] {
                auto state = weak.lock();
                if (!state) {
                    return;
                }
                std::function<void(std::string, bool)> callback;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->destroyed || state->discard || state->stop_requested ||
                        state->session != current) {
                        return;
                    }
                    callback = state->callback;
                }
                // final=true: replace the whole preview.
                callback(joined, true);
            });
        }
    }

    // A live window ended. After a decode (`decoded`), the blocks recorded meanwhile that did not
    // fit the queue are dropped and the worker takes the next blocks.
    void window_done(bool decoded) {
        bool again = false;
        bool dropped = false;
        {
            std::lock_guard lock(mutex);
            in_flight = false;
            job = JobHandle();
            if (decoded && !discard && tape) {
                dropped = drop_overflow(stop_requested ? stop_frame : tape->frames());
            }
            // A skipped window is followed at once by the next one, as the worker's loop did.
            again = streaming && !stop_requested && !discard && !destroyed;
        }
        changed.notify_all();
        if (dropped) {
            kLog.debug("Audio queue full, dropping preview chunk");
        }
        if (again) {
            ui.post([weak = weak_from_this()] {
                if (auto state = weak.lock()) {
                    state->pump();
                }
            });
        }
    }

    void failed(const std::exception& error, bool canceled) {
        bool quiet = false;
        {
            std::lock_guard lock(mutex);
            quiet = discard || destroyed;
        }
        if (!(quiet && canceled)) {
            kLog.error("Error in incremental transcription: {}", error.what());
        }
    }

    // UI: starts the next live window whenever one is complete and none is decoding. The
    // window's samples are copied here; fft_resample (tens of milliseconds for a 44.1 kHz window
    // with large prime factors) and the submit run on `preparer`, off the UI thread, as
    // OpenWhisper's worker thread prepared its windows.
    void pump() {
        std::shared_ptr<const audio::PcmTape> source;
        std::vector<TapeRange> blocks;
        std::optional<std::vector<std::int16_t>> tail;
        std::uint64_t current = 0;
        std::string key;
        {
            std::lock_guard lock(mutex);
            if (!streaming || stop_requested || discard || destroyed || in_flight || !tape) {
                return;
            }
            if (!gather(tape->frames())) {
                return;
            }
            blocks = std::move(gathered);
            gathered.clear();
            gathered_s = 0.0;
            in_flight = true;
            source = tape;
            tail = overlap_tail;
            current = session;
            key = "preview:" + job_id;
        }
        TakenWindow taken = copy_window(source, blocks, tail, current);
        // The last preparer has returned or is returning: in_flight was cleared on its way out
        // or after the decode it submitted.
        if (preparer.joinable()) {
            preparer.join();
        }
        preparer = std::thread([self = shared_from_this(), taken = std::move(taken),
                                channels = source->channels(), rate = source->rate(),
                                key = std::move(key)]() mutable {
            self->prepare_and_submit(std::move(taken), channels, rate, key);
        });
    }

    // On `preparer`: the front of _process_incremental_chunk, then the window's job.
    void prepare_and_submit(TakenWindow taken, int channels, int sample_rate,
                            const std::string& key) {
        try {
            auto prepared = prepare(taken, channels, sample_rate);
            if (!prepared) {
                window_done(false);
                return;
            }
            if (!audible(*prepared)) {
                // _audible: digital silence never reaches the engine.
                apply_text(taken, "");
                window_done(false);
                return;
            }
            bool abandoned = false;
            {
                std::lock_guard lock(mutex);
                abandoned = discard || destroyed;
            }
            if (abandoned) {
                // Cancelled, timed out or destroyed while the window was prepared.
                window_done(false);
                return;
            }
            const std::uint64_t current = taken.session;
            JobHandle handle = service.submit(
                window_job(std::move(*prepared), Lane::Preview, key), direct(),
                [weak = weak_from_this(),
                 taken = std::move(taken)](std::expected<DecodeResult, EngineError> outcome) {
                    auto state = weak.lock();
                    if (!state || !state->begin_delivery()) {
                        return;
                    }
                    struct Delivery {
                        PreviewState& state;
                        ~Delivery() { state.end_delivery(); }
                    } delivery{*state};
                    if (outcome) {
                        state->apply_text(taken, py::strip(outcome->transcript.text));
                    } else {
                        state->failed(outcome.error(),
                                      outcome.error().what() == kTranscriptionCanceled);
                    }
                    state->window_done(true);
                });
            std::lock_guard lock(mutex);
            if (in_flight && session == current) {
                job = std::move(handle);
            }
        } catch (const std::exception& error) {
            failed(error, false);
            window_done(false);
        }
    }

    void tick() {
        pump();
        std::lock_guard lock(mutex);
        if (streaming && !destroyed) {
            arm();
        }
    }

    // Under `mutex`.
    void arm() {
        timer = ui.post_after(options.poll, [weak = weak_from_this()] {
            if (auto state = weak.lock()) {
                state->tick();
            }
        });
    }

    // A finalize window, decoded on the calling thread; `cancel` ends the decode early.
    void process_now(const std::shared_ptr<const audio::PcmTape>& source,
                     const std::vector<TapeRange>& blocks, std::uint64_t current,
                     const CancelToken& cancel) {
        std::optional<std::vector<std::int16_t>> tail;
        {
            std::lock_guard lock(mutex);
            if (discard || destroyed) {
                return;
            }
            tail = overlap_tail;
        }
        const TakenWindow taken = copy_window(source, blocks, tail, current);
        auto prepared = prepare(taken, source->channels(), source->rate());
        if (!prepared) {
            return;
        }
        if (!audible(*prepared)) {
            apply_text(taken, "");
            return;
        }
        try {
            // The dictation waits on this text: it replaces an empty final transcript.
            DecodeJob decode = window_job(std::move(*prepared), Lane::Final, {});
            decode.cancel = cancel;
            const DecodeResult result = service.run(std::move(decode));
            apply_text(taken, py::strip(result.transcript.text));
        } catch (const EngineError& error) {
            failed(error, error.what() == kTranscriptionCanceled);
        } catch (const std::exception& error) {
            failed(error, false);
        }
    }
};

} // namespace detail

WindowPreview::WindowPreview(SpeechService& service, Executor& ui, Clock& clock,
                             WindowPreviewOptions options)
    : state_(std::make_shared<detail::PreviewState>(service, ui, clock, options)) {
    kLog.info("StreamingTranscriber initialized (chunk_duration={}s, overlap={}s)",
              py::float_repr(options.chunk_duration_s), py::float_repr(options.overlap_s));
}

WindowPreview::~WindowPreview() {
    JobHandle job;
    CancelSource finalizing;
    {
        std::lock_guard lock(state_->mutex);
        state_->destroyed = true;
        state_->discard = true;
        state_->streaming = false;
        state_->timer.cancel();
        job = std::exchange(state_->job, JobHandle());
        finalizing = state_->finalize_cancel;
    }
    state_->changed.notify_all();
    job.cancel();
    finalizing.cancel();
    // A window being prepared sees `destroyed` and submits nothing; it must not outlive the
    // service the caller destroys next.
    if (state_->preparer.joinable()) {
        state_->preparer.join();
    }
    // A result the speech thread began delivering before `destroyed` was set may still post to
    // the UI executor or read the clock, which the caller may destroy next. A decode still
    // running is not waited for: its result finds `destroyed` and touches neither.
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock, [this] { return state_->delivering == 0; });
}

void WindowPreview::start(std::shared_ptr<const audio::PcmTape> tape, std::string job_id,
                          std::function<void(std::string, bool)> callback) {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->streaming || state_->finalizing || state_->in_flight) {
            kLog.warning("Streaming worker is still active");
            return;
        }
        state_->tape = std::move(tape);
        state_->job_id = std::move(job_id);
        state_->callback = std::move(callback);
        ++state_->session;
        state_->streaming = state_->tape != nullptr;
        state_->stop_requested = false;
        state_->discard = false;
        state_->text.clear();
        state_->last_chunk_text.clear();
        state_->overlap_tail.reset();
        // The new recording drops whatever the last stop retained.
        state_->read = 0;
        state_->gathered.clear();
        state_->gathered_s = 0.0;
        state_->skipped.clear();
        state_->stop_frame = 0;
        state_->finalize_pending = false;
        state_->chunks = 0;
        state_->slow_chunks = 0;
        if (state_->streaming) {
            state_->arm();
        }
    }
    kLog.info("Streaming transcription started");
}

std::string WindowPreview::stop() {
    std::lock_guard lock(state_->mutex);
    if (!state_->streaming) {
        return "";
    }
    state_->stop_requested = true;
    state_->streaming = false;
    state_->finalize_pending = true;
    state_->stop_frame = state_->tape->frames();
    state_->timer.cancel();
    return py::strip(state_->text);
}

std::optional<std::string> WindowPreview::finalize(std::chrono::milliseconds timeout) {
    assert_not_gui_thread();
    // Held for the call: the preview may be destroyed on the UI thread while this waits, and the
    // destructor's wake-up must not free the mutex this thread reacquires.
    const std::shared_ptr<detail::PreviewState> keep = state_;
    auto& state = *keep;
    std::shared_ptr<const audio::PcmTape> source;
    std::vector<detail::TapeRange> blocks;
    std::uint64_t current = 0;
    CancelToken cancel;
    {
        std::unique_lock lock(state.mutex);
        if (state.discard) {
            return std::nullopt;
        }
        if (!state.finalize_pending) {
            return py::strip(state.text);
        }
        state.changed.wait_for(lock, timeout, [&] {
            return !state.in_flight || state.discard || !state.finalize_pending;
        });
        if (state.discard || !state.finalize_pending) {
            // Cancel, cleanup, or a new recording took the audio meanwhile.
            return std::nullopt;
        }
        state.finalize_pending = false;
        if (state.in_flight) {
            state.discard = true;
            state.forget_retained();
            std::string text = py::strip(state.text);
            lock.unlock();
            kLog.warning("Preview worker did not finish in time; ignoring late output");
            return text;
        }
        blocks = state.take_retained();
        state.finalizing = true;
        source = state.tape;
        current = state.session;
        // Under the lock that cancel() takes: a cancel from here on reaches this decode.
        state.finalize_cancel = CancelSource();
        cancel = state.finalize_cancel.token();
    }
    const auto started = state.clock.steady();
    std::vector<detail::TapeRange> window;
    double duration = 0.0;
    for (const detail::TapeRange& block : blocks) {
        window.push_back(block);
        duration += static_cast<double>(block.last - block.first) / source->rate();
        if (duration >= state.options.chunk_duration_s) {
            state.process_now(source, window, current, cancel);
            window.clear();
            duration = 0.0;
        }
    }
    if (!window.empty()) {
        state.process_now(source, window, current, cancel);
    }
    std::optional<std::string> text;
    {
        std::lock_guard lock(state.mutex);
        state.finalizing = false;
        if (!state.discard) {
            text = py::strip(state.text);
        }
    }
    state.changed.notify_all();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(state.clock.steady() - started).count();
    kLog.info("Preview tail finalized in {} ms ({} blocks, {} chars)",
              py::format_fixed(elapsed_ms, 0), blocks.size(), text ? py::len(*text) : 0);
    return text;
}

void WindowPreview::cancel() {
    JobHandle job;
    CancelSource finalizing;
    {
        std::lock_guard lock(state_->mutex);
        state_->discard = true;
        state_->stop_requested = true;
        state_->streaming = false;
        state_->finalize_pending = false;
        if (state_->tape) {
            state_->stop_frame = std::max(state_->stop_frame, state_->tape->frames());
        }
        state_->forget_retained();
        state_->timer.cancel();
        job = std::exchange(state_->job, JobHandle());
        finalizing = state_->finalize_cancel;
    }
    state_->changed.notify_all();
    // A window still queued is dropped now; one decoding finishes and its result is ignored.
    // The same holds for a window finalize() is decoding: it returns without its text.
    job.cancel();
    finalizing.cancel();
}

void WindowPreview::cleanup() {
    cancel();
    kLog.info("StreamingTranscriber cleaned up");
}

bool WindowPreview::running() const {
    std::lock_guard lock(state_->mutex);
    return state_->streaming;
}

bool WindowPreview::decoding() const {
    std::lock_guard lock(state_->mutex);
    return state_->in_flight;
}

std::string WindowPreview::preview_text() const {
    std::lock_guard lock(state_->mutex);
    return state_->text;
}

std::size_t WindowPreview::chunk_count() const {
    std::lock_guard lock(state_->mutex);
    return state_->chunks;
}

} // namespace fv::speech
