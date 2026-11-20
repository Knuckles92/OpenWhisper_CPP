#include "speech/file_windows.h"

#include <future>
#include <span>
#include <string>
#include <utility>

#include "engine/error.h"
#include "speech/messages.h"
#include "speech/window_decode.h"

namespace fv::speech {

namespace {

constexpr std::string_view kCanceled = kTranscriptionCanceled;

AudioClip to_clip(dsp::Window&& window) {
    AudioClip clip;
    const std::size_t size = window.samples.size();
    clip.samples = std::make_shared<const std::vector<float>>(std::move(window.samples));
    clip.begin = 0;
    clip.end = size;
    clip.rate = 16000;
    clip.offset_s = window.start_s;
    return clip;
}

// Runs a posted task at once on the posting thread: the speech thread completes a decode_file
// window by fulfilling its promise, which needs no hop.
class DirectExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return false; }
};

} // namespace

FileWindows::FileWindows(const std::filesystem::path& path) {
    try {
        decoder_ = std::make_unique<audio::AudioFileDecoder>(path);
    } catch (const audio::AudioDecodeError& error) {
        throw EngineError(error.what());
    }
}

FileWindows::~FileWindows() = default;
FileWindows::FileWindows(FileWindows&&) noexcept = default;
FileWindows& FileWindows::operator=(FileWindows&&) noexcept = default;

std::optional<AudioClip> FileWindows::next() {
    while (ready_.empty() && !finished_) {
        audio::DecodedFrame frame;
        bool decoded = false;
        try {
            decoded = decoder_->next(frame);
        } catch (const audio::AudioDecodeError& error) {
            throw EngineError(error.what());
        }
        pcm_.clear();
        if (decoded) {
            resampler_.push(frame, pcm_);
        } else {
            resampler_.finish(pcm_);
        }
        for (dsp::Window& window : splitter_.push(pcm_)) {
            ready_.push_back(to_clip(std::move(window)));
        }
        if (!decoded) {
            for (dsp::Window& window : splitter_.finish()) {
                ready_.push_back(to_clip(std::move(window)));
            }
            finished_ = true;
        }
    }
    if (ready_.empty()) {
        return std::nullopt;
    }
    AudioClip clip = std::move(ready_.front());
    ready_.pop_front();
    return clip;
}

std::vector<AudioClip> file_windows(const std::filesystem::path& path) {
    FileWindows windows(path);
    std::vector<AudioClip> clips;
    while (std::optional<AudioClip> clip = windows.next()) {
        clips.push_back(std::move(*clip));
    }
    return clips;
}

DecodeResult decode_file(SpeechService& service, const std::filesystem::path& path,
                         const RecognitionOptions& options, Lane lane, const CancelToken& cancel,
                         Slot slot) {
    assert_not_gui_thread();
    using Outcome = std::expected<DecodeResult, EngineError>;
    struct Pending {
        JobHandle handle;
        std::future<Outcome> outcome;
        double offset_s = 0.0;
    };
    DirectExecutor direct;
    FileWindows windows(path);
    std::optional<Pending> pending;

    const auto submit = [&](AudioClip clip) {
        auto promise = std::make_shared<std::promise<Outcome>>();
        Pending made;
        made.outcome = promise->get_future();
        made.offset_s = clip.offset_s;
        DecodeJob job;
        job.input = std::move(clip);
        job.options = options;
        job.lane = lane;
        job.cancel = cancel;
        job.slot = slot;
        made.handle = service.submit(std::move(job), direct, [promise](Outcome outcome) {
            promise->set_value(std::move(outcome));
        });
        return made;
    };

    DecodeResult total;
    std::vector<std::string> texts;
    bool first = true;
    const auto collect = [&](Pending& done) {
        Outcome outcome = done.outcome.get();
        if (!outcome) {
            throw outcome.error();
        }
        if (first) {
            total.generation = outcome->generation;
            first = false;
        } else if (outcome->generation != total.generation) {
            // A reload between windows: text from two engines is never joined.
            throw EngineError(std::string(kCanceled));
        }
        Transcript& piece = outcome->transcript;
        texts.push_back(std::move(piece.text));
        for (Segment& segment : piece.segments) {
            segment.start += done.offset_s;
            segment.end += done.offset_s;
            total.transcript.segments.push_back(std::move(segment));
        }
        for (Word& word : piece.words) {
            word.start += done.offset_s;
            word.end += done.offset_s;
            total.transcript.words.push_back(std::move(word));
        }
        total.transcript.audio_processed = done.offset_s + piece.audio_processed;
        total.decode_s += outcome->decode_s;
    };

    try {
        for (;;) {
            if (cancel.cancelled()) {
                throw EngineError(std::string(kCanceled));
            }
            // The next window is read and resampled while the previous one decodes.
            std::optional<AudioClip> clip = windows.next();
            if (pending) {
                Pending done = std::move(*pending);
                pending.reset();
                collect(done);
            }
            if (!clip) {
                break;
            }
            pending = submit(std::move(*clip));
        }
    } catch (...) {
        if (pending) {
            pending->handle.cancel();
            pending->outcome.wait();
        }
        throw;
    }
    total.transcript.text = join_texts(texts);
    return total;
}

} // namespace fv::speech
