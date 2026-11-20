#include "meeting_capture/window_preview.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iterator>

#include "base/log.h"
#include "meeting_capture/asr_audio.h"
#include "meeting_capture/detail/files.h"
#include "pyport/errors.h"
#include "speech/window_decode.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.asr.preview");

// How often the preview thread looks for queued blocks: feed() runs on device threads, which may
// not signal (no system calls there). Python waits up to 0.1 s on its queue.
constexpr auto kPoll = std::chrono::milliseconds(10);

constexpr std::size_t kWindowSamples =
    static_cast<std::size_t>(PREVIEW_WINDOW_S * DECODER_SAMPLE_RATE);

} // namespace

// --- PreviewWindows ------------------------------------------------------------------------------

PreviewWindows::PreviewWindows(PreviewDecode decode, PreviewCallback callback,
                               std::function<bool()> busy, const Clock& clock)
    : decode_(std::move(decode)), callback_(std::move(callback)), busy_(std::move(busy)),
      clock_(clock) {}

double PreviewWindows::now() const {
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

void PreviewWindows::accept(std::string_view channel, std::span<const std::int16_t> frames,
                            int rate, double t_mono, double start_s) {
    if (rate == 0) {
        throw py::PyError("ZeroDivisionError", "division by zero");
    }
    const double length_s = static_cast<double>(frames.size()) / rate;
    const double end_s = start_s + length_s;
    auto found = std::ranges::find(sessions_, channel, &std::pair<std::string, Session>::first);
    const bool restart = found == sessions_.end() ||
                         std::abs(start_s - found->second.end) > PREVIEW_MEDIA_GAP_S ||
                         std::abs(t_mono - found->second.wall_end) > PREVIEW_WALL_GAP_S;
    if (restart) {
        Session fresh;
        fresh.end = start_s;
        fresh.decoded = start_s;
        if (found == sessions_.end()) {
            sessions_.emplace_back(std::string(channel), std::move(fresh));
            found = std::prev(sessions_.end());
        } else {
            found->second = std::move(fresh);
        }
    }
    Session& state = found->second;
    const std::vector<float> audio = prepare_for_decoder(frames, rate);
    state.audio.insert(state.audio.end(), audio.begin(), audio.end());
    if (state.audio.size() > kWindowSamples) {
        state.audio.erase(state.audio.begin(),
                          state.audio.end() - static_cast<std::ptrdiff_t>(kWindowSamples));
    }
    state.end = end_s;
    state.wall_end = t_mono + length_s;
}

void PreviewWindows::decode(const CancelToken& stop) {
    if (busy_() || now() < next_decode_) {
        return;
    }
    // Oldest due channel first, so continuous microphone speech cannot starve system audio.
    std::vector<Session*> order;
    std::vector<const std::string*> names;
    for (auto& [name, state] : sessions_) {
        order.push_back(&state);
        names.push_back(&name);
    }
    std::vector<std::size_t> index(order.size());
    for (std::size_t i = 0; i < index.size(); ++i) {
        index[i] = i;
    }
    std::ranges::stable_sort(index, {}, [&](std::size_t i) { return order[i]->decoded; });
    for (const std::size_t i : index) {
        Session& state = *order[i];
        if (state.end - state.decoded < PREVIEW_INTERVAL_S) {
            continue;
        }
        state.decoded = state.end;
        if (!speech::audible(state.audio)) {
            continue;
        }
        const std::vector<float> samples = state.audio;
        const double began = now();
        const std::optional<std::string> text = decode_(*names[i], samples, stop);
        if (!text) {
            return;
        }
        next_decode_ = now() + std::max(PREVIEW_MIN_DELAY_S, 3 * (now() - began));
        if (!stop.cancelled()) {
            Json event = Json::object();
            event["channel"] = *names[i];
            event["text"] = *text;
            event["start_s"] =
                state.end - static_cast<double>(samples.size()) / DECODER_SAMPLE_RATE;
            event["end_s"] = state.end;
            event["final"] = false;
            callback_(event);
        }
        return;
    }
}

// --- the queue -----------------------------------------------------------------------------------

// A bounded multi-producer, single-consumer queue of preallocated slots (Vyukov's bounded
// queue): producers claim a slot with one compare-and-swap and touch only preallocated memory,
// so feed() never blocks.
class WindowSpeechPreview::Queue {
public:
    // Samples one slot holds: four of PortAudio's 1024-frame blocks.
    static constexpr std::size_t kSlotSamples = 4096;

    explicit Queue(std::size_t slots) : cells_(std::max<std::size_t>(slots, 1)) {
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
            cells_[i].samples.resize(kSlotSamples);
            cells_[i].channel.reserve(31);
        }
    }

    std::size_t capacity() const { return cells_.size(); }

    std::size_t size() const {
        const std::size_t head = enqueue_.load(std::memory_order_acquire);
        const std::size_t tail = dequeue_.load(std::memory_order_acquire);
        return head - tail;
    }

    bool push(std::string_view channel, std::span<const std::int16_t> samples, int rate,
              double t_mono, double start_s) noexcept {
        Cell* cell = nullptr;
        std::size_t position = enqueue_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[position % cells_.size()];
            const std::size_t sequence = cell->sequence.load(std::memory_order_acquire);
            const auto difference =
                static_cast<std::ptrdiff_t>(sequence) - static_cast<std::ptrdiff_t>(position);
            if (difference == 0) {
                if (enqueue_.compare_exchange_weak(position, position + 1,
                                                   std::memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                return false; // full
            } else {
                position = enqueue_.load(std::memory_order_relaxed);
            }
        }
        try {
            cell->channel.assign(channel);
        } catch (...) {
            cell->channel.clear(); // no memory for a long name: the block keeps an empty one
        }
        std::copy(samples.begin(), samples.end(), cell->samples.begin());
        cell->count = samples.size();
        cell->rate = rate;
        cell->t_mono = t_mono;
        cell->start_s = start_s;
        cell->sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    // Hands the oldest block to fn(const Cell&) and frees its slot; false when empty.
    template <class Fn> bool pop(Fn&& fn) {
        const std::size_t position = dequeue_.load(std::memory_order_relaxed);
        Cell& cell = cells_[position % cells_.size()];
        if (cell.sequence.load(std::memory_order_acquire) != position + 1) {
            return false;
        }
        fn(cell);
        cell.sequence.store(position + cells_.size(), std::memory_order_release);
        dequeue_.store(position + 1, std::memory_order_release);
        return true;
    }

    struct Cell {
        std::atomic<std::size_t> sequence{0};
        std::string channel;
        std::vector<std::int16_t> samples;
        std::size_t count = 0;
        int rate = 0;
        double t_mono = 0.0;
        double start_s = 0.0;
    };

private:
    std::vector<Cell> cells_;
    std::atomic<std::size_t> enqueue_{0};
    std::atomic<std::size_t> dequeue_{0}; // written by the consumer only
};

// --- WindowSpeechPreview -------------------------------------------------------------------------

WindowSpeechPreview::WindowSpeechPreview(PreviewDecode decode, PreviewCallback callback,
                                         std::function<bool()> busy, const Clock& clock,
                                         PreviewOptions options)
    : windows_(std::move(decode), std::move(callback), std::move(busy), clock), options_(options),
      queue_(std::make_unique<Queue>(options.queue_blocks)) {
    if (options_.start_thread) {
        thread_ = std::thread([this] { run(); });
    } else {
        finished_ = true;
    }
}

WindowSpeechPreview::~WindowSpeechPreview() {
    stop();
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();
        } else {
            thread_.join();
        }
    }
}

void WindowSpeechPreview::feed(const CaptureBlock& block, double start_s) noexcept {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    const std::span<const std::int16_t> samples = block.samples();
    std::size_t at = 0;
    do {
        const std::size_t count = std::min(Queue::kSlotSamples, samples.size() - at);
        const double offset = block.rate > 0 ? static_cast<double>(at) / block.rate : 0.0;
        if (!queue_->push(block.channel, samples.subspan(at, count), block.rate,
                          block.t_mono + offset, start_s + offset)) {
            return; // full: only preview data is lost, the spool owns every block
        }
        at += count;
    } while (at < samples.size());
}

bool WindowSpeechPreview::stop() {
    stopped_.store(true, std::memory_order_release);
    stop_source_.cancel();
    wake_.notify_all();
    std::unique_lock lock(mutex_);
    const bool ended = wake_.wait_for(lock, std::chrono::duration<double>(options_.stop_join_s),
                                      [this] { return finished_; });
    lock.unlock();
    if (ended && thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    return ended;
}

std::size_t WindowSpeechPreview::queued() const {
    return queue_->size();
}

std::optional<PreviewBlock> WindowSpeechPreview::take() {
    std::optional<PreviewBlock> out;
    queue_->pop([&](const Queue::Cell& cell) {
        out = PreviewBlock{cell.channel,
                           std::vector<std::int16_t>(cell.samples.begin(),
                                                     cell.samples.begin() +
                                                         static_cast<std::ptrdiff_t>(cell.count)),
                           cell.rate, cell.t_mono, cell.start_s};
    });
    return out;
}

void WindowSpeechPreview::run() {
    detail::set_thread_name("fv-meeting-preview");
    const CancelToken stop = stop_source_.token();
    const auto accept = [this](const Queue::Cell& cell) {
        windows_.accept(cell.channel,
                        std::span<const std::int16_t>(cell.samples.data(), cell.count), cell.rate,
                        cell.t_mono, cell.start_s);
    };
    try {
        while (!stopped_.load(std::memory_order_acquire)) {
            if (!queue_->pop(accept)) {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, kPoll,
                               [this] { return stopped_.load(std::memory_order_acquire); });
                continue;
            }
            // Catch up with capture before inference, instead of replaying stale windows.
            for (std::size_t i = 0; i < PREVIEW_QUEUE_BLOCKS; ++i) {
                if (!queue_->pop(accept)) {
                    break;
                }
            }
            windows_.decode(stop);
        }
    } catch (const std::exception& error) {
        logger.error_with(error, "Window speech preview stopped; durable recording continues");
    } catch (...) {
        logger.error("Window speech preview stopped; durable recording continues");
    }
    {
        const std::lock_guard lock(mutex_);
        finished_ = true;
    }
    wake_.notify_all();
}

} // namespace fv::meeting
