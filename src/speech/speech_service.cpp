#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "base/log.h"
#include "dsp/window_splitter.h"
#include "speech/detail/thread_name.h"
#include "speech/messages.h"
#include "speech/service.h"
#include "speech/window_decode.h"

namespace fv::speech {

namespace {

constexpr log::Logger kLog{"services.speech"};
constexpr std::string_view kCanceled = kTranscriptionCanceled;
constexpr std::string_view kReleased = "Released while Meeting Mode runs";
constexpr std::size_t kLanes = 7;
constexpr std::size_t kSlots = 2;

using Outcome = std::expected<DecodeResult, EngineError>;
using Callback = std::function<void(Outcome)>;

std::size_t lane_index(Lane lane) {
    return static_cast<std::size_t>(lane);
}

std::size_t slot_index(Slot slot) {
    return static_cast<std::size_t>(slot);
}

EngineError canceled() {
    return EngineError(std::string(kCanceled));
}

} // namespace

namespace detail {

struct JobState {
    std::uint64_t id = 0;
    DecodeJob job;
    Executor* reply_on = nullptr; // null: complete on the thread that finishes the job
    Callback callback;
    std::size_t slot = 0; // the slot whose engine runs it, resolved at submit
    std::uint64_t generation = 0;
    bool warmup = false;
    CancelSource cancel; // the handle's cancel, and the job token's through on_cancel
    Subscription token_link;
    std::atomic<bool> done{false};

    // Speech thread only.
    std::size_t next = 0; // samples of the clip already decoded
    std::vector<std::string> texts;
    Transcript transcript;
    double decode_s = 0.0;

    bool started() const { return next > 0; }
};

struct ControlTask {
    enum class Kind { Load, Release };
    Kind kind = Kind::Load;
    std::size_t slot = 0;
    std::uint64_t generation = 0;
    BackendConfig config;
};

struct SlotState {
    std::optional<BackendConfig> config;
    // Written by the speech thread under `mutex`; other threads copy it under `mutex` for the
    // facade calls, so a backend released meanwhile lives until their call returns.
    std::shared_ptr<Backend> backend;
    std::string resident_model_key;
    Readiness readiness{ReadyState::Loading, "", ""};
    std::optional<Readiness> pending_readiness;
    Subscription backend_watch;
    std::uint64_t generation = 0;
    CancelSource generation_source;
    bool released = false;
    std::optional<BackendConfig> pending; // a select a lease held back
};

// What a generation change leaves to do outside the lock (section 4 rule 6: cancelling runs the
// tokens' callbacks, and completing posts the job callbacks): cancel the old generation's token
// and the running job, end the queued jobs.
struct Bumped {
    CancelSource generation;
    std::optional<CancelSource> running;
    std::vector<std::shared_ptr<JobState>> queued;
};

struct ServiceState : std::enable_shared_from_this<ServiceState> {
    ServiceState(BackendFactory factory_, Executor& notify_)
        : factory(std::move(factory_)), notify(notify_) {}

    BackendFactory factory;
    std::map<std::string, BackendFactory> feature_factories; // guarded by mutex
    Executor& notify;

    // Signals are emitted through `notify`, while `owner` is set.
    std::recursive_mutex emit_mutex;
    SpeechService* owner = nullptr;
    // Held while a signal is posted to `notify`, so posts keep the order their causes had: a
    // caller that changed state under `mutex` takes it before releasing `mutex` and posts after
    // (emit_after_unlock), and the speech thread's later posts wait for it. Acquired after
    // `mutex`, never before it; recursive, since emit() takes it again.
    std::recursive_mutex post_order;

    // Acquired before mutex. Maintenance runs off the speech thread; the guard covers the
    // backend's actual acquire/release rather than the controller's delayed readiness signal.
    std::mutex model_assets;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::array<std::deque<std::shared_ptr<JobState>>, kLanes> lanes;
    std::deque<ControlTask> controls;
    std::shared_ptr<JobState> running;
    bool running_control = false;
    // The cancel source of the load running on the speech thread (its slot's generation).
    std::optional<CancelSource> loading;
    std::array<SlotState, kSlots> slots;
    std::vector<std::pair<std::uint64_t, std::string>> leases;
    std::uint64_t next_id = 1;
    bool stopping = false;
    bool busy = false;
    std::thread thread;

    // --- under `mutex` ---

    bool jobs_queued() const {
        return std::ranges::any_of(lanes, [](const auto& lane) { return !lane.empty(); });
    }

    bool queued() const { return !controls.empty() || jobs_queued(); }

    std::size_t resolve(Slot slot) const {
        const auto& selected = slots[slot_index(slot)];
        if (slot == Slot::Meeting && (!selected.config || selected.released)) {
            return slot_index(Slot::Dictation);
        }
        return slot_index(slot);
    }

    // Takes every queued job of the slot (all slots when nullopt) out of the lanes.
    std::vector<std::shared_ptr<JobState>> take_queued(std::optional<std::size_t> slot) {
        std::vector<std::shared_ptr<JobState>> taken;
        for (auto& lane : lanes) {
            for (auto it = lane.begin(); it != lane.end();) {
                if (!slot || (*it)->slot == *slot) {
                    taken.push_back(std::move(*it));
                    it = lane.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return taken;
    }

    // Advances the slot's generation. The old generation's token and the slot's running job are
    // returned to be cancelled outside the lock, and its queued jobs to be ended.
    Bumped bump(std::size_t slot) {
        SlotState& state = slots[slot];
        ++state.generation;
        state.pending_readiness.reset();
        Bumped bumped;
        bumped.generation = std::exchange(state.generation_source, CancelSource());
        if (running && running->slot == slot) {
            bumped.running = running->cancel;
        }
        bumped.queued = take_queued(slot);
        return bumped;
    }

    // --- outside `mutex` ---

    void emit(std::function<void(SpeechService&)> fn) {
        std::lock_guard order(post_order);
        notify.post([weak = weak_from_this(), fn = std::move(fn)] {
            if (auto state = weak.lock()) {
                std::lock_guard guard(state->emit_mutex);
                if (state->owner != nullptr) {
                    fn(*state->owner);
                }
            }
        });
    }

    void emit_readiness(std::size_t slot, Readiness readiness) {
        emit([slot, readiness = std::move(readiness)](SpeechService& service) {
            service.readiness_changed.emit(static_cast<Slot>(slot), readiness);
        });
    }

    // With `lock` held on `mutex`: releases it, then posts each (slot, readiness) ahead of
    // anything the speech thread reports afterwards. The executor's post never runs under
    // `mutex`.
    void emit_after_unlock(std::unique_lock<std::mutex>& lock,
                           std::vector<std::pair<std::size_t, Readiness>> readiness) {
        std::lock_guard order(post_order);
        lock.unlock();
        for (auto& [slot, read] : readiness) {
            emit_readiness(slot, std::move(read));
        }
    }

    void emit_watched_readiness(std::size_t slot, std::uint64_t generation) {
        emit([weak = weak_from_this(), slot, generation](SpeechService& service) {
            const auto state = weak.lock();
            if (!state) {
                return;
            }
            Readiness latest;
            {
                std::lock_guard lock(state->mutex);
                if (state->slots[slot].generation != generation || state->stopping) {
                    return;
                }
                latest = state->slots[slot].readiness;
            }
            service.readiness_changed.emit(static_cast<Slot>(slot), latest);
        });
    }

    void emit_warmed(std::size_t slot, bool ran) {
        emit([slot, ran](SpeechService& service) {
            service.warmed_up.emit(static_cast<Slot>(slot), ran);
        });
    }

    void complete(const std::shared_ptr<JobState>& job, Outcome outcome) {
        if (job->done.exchange(true)) {
            return;
        }
        if (job->warmup) {
            emit_warmed(job->slot, outcome.has_value());
            return;
        }
        if (!job->callback) {
            return;
        }
        if (job->reply_on == nullptr) {
            job->callback(std::move(outcome));
            return;
        }
        job->reply_on->post(
            [callback = std::move(job->callback), outcome = std::move(outcome)]() mutable {
                callback(std::move(outcome));
            });
    }

    void end_all(std::vector<std::shared_ptr<JobState>>& jobs) {
        for (const auto& job : jobs) {
            complete(job, std::unexpected(canceled()));
        }
    }

    void cancel_job(std::uint64_t id) {
        std::shared_ptr<JobState> taken;
        std::optional<CancelSource> running_cancel;
        {
            std::lock_guard lock(mutex);
            for (auto& lane : lanes) {
                const auto found =
                    std::ranges::find_if(lane, [id](const auto& job) { return job->id == id; });
                if (found != lane.end()) {
                    taken = std::move(*found);
                    lane.erase(found);
                    break;
                }
            }
            if (!taken && running && running->id == id) {
                running_cancel = running->cancel; // its window finishes; the result is discarded
            }
        }
        if (running_cancel) {
            running_cancel->cancel();
        }
        if (taken) {
            complete(taken, std::unexpected(canceled()));
            idle.notify_all();
        }
    }

    // Under `mutex`: selects `config` for the slot now.
    Bumped apply_select(std::size_t slot, BackendConfig config) {
        SlotState& state = slots[slot];
        state.config = config;
        state.released = false;
        state.pending.reset();
        auto bumped = bump(slot);
        state.readiness = Readiness{ReadyState::Loading, "", ""};
        controls.push_back(
            ControlTask{ControlTask::Kind::Load, slot, state.generation, std::move(config)});
        wake.notify_all();
        return bumped;
    }

    void after_bump(Bumped& bumped) {
        bumped.generation.cancel();
        if (bumped.running) {
            bumped.running->cancel();
        }
        end_all(bumped.queued);
    }

    // --- the speech thread ---

    void set_busy(bool now) {
        // Under `mutex`; emitted in order through `notify`.
        if (busy == now) {
            return;
        }
        busy = now;
        // Not through emit(): busy changes are ordered by `mutex` already, and waiting for
        // `post_order` here would hold `mutex` while another thread posts.
        notify.post([weak = weak_from_this(), now] {
            if (auto state = weak.lock()) {
                std::lock_guard guard(state->emit_mutex);
                if (state->owner != nullptr) {
                    state->owner->busy_changed.emit(now);
                }
            }
        });
    }

    void run_control(ControlTask& task) {
        std::unique_lock asset_guard(model_assets);
        SlotState& state = slots[task.slot];
        {
            std::lock_guard lock(mutex);
            if (task.generation != state.generation) {
                return; // a newer select, release or restore replaced it
            }
        }
        std::shared_ptr<Backend> previous;
        Subscription previous_watch;
        {
            std::lock_guard lock(mutex);
            previous = std::move(state.backend);
            previous_watch = std::move(state.backend_watch);
            state.backend.reset();
            state.resident_model_key.clear();
        }
        previous_watch.disconnect();
        if (previous) {
            previous->release();
            previous.reset();
        }
        if (task.kind == ControlTask::Kind::Release) {
            return;
        }
        CancelToken token;
        {
            std::lock_guard lock(mutex);
            token = state.generation_source.token();
            loading = state.generation_source;
        }
        std::unique_ptr<Backend> backend;
        Subscription watch;
        Readiness readiness{ReadyState::Failed, "", ""};
        try {
            backend = factory ? factory(task.config) : nullptr;
            if (!backend) {
                BackendFactory fallback;
                {
                    std::lock_guard lock(mutex);
                    const auto found = feature_factories.find(task.config.backend);
                    if (found != feature_factories.end()) {
                        fallback = found->second;
                    }
                }
                if (fallback) {
                    backend = fallback(task.config);
                }
            }
            if (!backend) {
                const std::string message =
                    "No speech engine is available for \"" + task.config.backend + "\".";
                readiness = Readiness{ReadyState::Failed, message, message};
            } else {
                watch = backend->watch_readiness(
                    notify, [weak = weak_from_this(), slot = task.slot,
                             generation = task.generation](Readiness read) {
                        const auto service = weak.lock();
                        if (!service) {
                            return;
                        }
                        {
                            std::lock_guard lock(service->mutex);
                            auto& watched = service->slots[slot];
                            if (watched.generation != generation || service->stopping) {
                                return;
                            }
                            if (!watched.backend) {
                                watched.pending_readiness = std::move(read);
                                return;
                            }
                            if (watched.readiness.state == read.state &&
                                watched.readiness.device_info == read.device_info &&
                                watched.readiness.message == read.message) {
                                return;
                            }
                            watched.readiness = read;
                        }
                        service->emit_watched_readiness(slot, generation);
                    });
                readiness = backend->load(token);
            }
        } catch (const std::exception& error) {
            readiness = Readiness{ReadyState::Failed, error.what(), error.what()};
        }
        const bool watching = watch.connected();
        bool current = false;
        {
            std::lock_guard lock(mutex);
            loading.reset();
            current = task.generation == state.generation;
            if (current) {
                if (state.pending_readiness) {
                    readiness = std::move(*state.pending_readiness);
                    state.pending_readiness.reset();
                }
                state.readiness = readiness;
                state.backend_watch = std::move(watch);
                if (backend && task.config.backend == "parakeet" &&
                    readiness.state == ReadyState::Ready) {
                    state.resident_model_key = task.config.model_key;
                }
                state.backend = std::move(backend);
            }
        }
        if (!current) {
            if (backend) {
                backend->release();
            }
            return;
        }
        asset_guard.unlock();
        if (watching) {
            emit_watched_readiness(task.slot, task.generation);
        } else {
            emit_readiness(task.slot, std::move(readiness));
        }
    }

    void run_warmup(const std::shared_ptr<JobState>& job) {
        Backend* backend = slots[job->slot].backend.get();
        CancelToken token;
        bool skip = false;
        {
            std::lock_guard lock(mutex);
            token = slots[job->slot].generation_source.token();
            skip = jobs_queued() || job->generation != slots[job->slot].generation ||
                   backend == nullptr || slots[job->slot].readiness.state != ReadyState::Ready ||
                   job->cancel.cancelled();
        }
        bool ran = false;
        if (!skip) {
            ran = backend->warm_up(token);
        }
        job->done = true;
        emit_warmed(job->slot, ran);
    }

    // One window of a decode job; true when the job is to be queued again for its next window.
    bool run_step(const std::shared_ptr<JobState>& job) {
        SlotState& slot = slots[job->slot];
        CancelToken generation_token;
        bool stale = false;
        {
            std::lock_guard lock(mutex);
            stale = job->generation != slot.generation;
            generation_token = slot.generation_source.token();
        }
        if (stale || job->cancel.cancelled()) {
            complete(job, std::unexpected(canceled()));
            return false;
        }
        Backend* backend = slot.backend.get();
        if (backend == nullptr) {
            std::string message;
            {
                std::lock_guard lock(mutex);
                message = slot.readiness.device_info;
            }
            complete(job, std::unexpected(EngineError(
                              message.empty() ? "Speech engine is not loaded" : message)));
            return false;
        }

        // This window's job: the caller's input cut to one window, cancelled by the caller,
        // the handle or the next generation.
        DecodeJob piece;
        piece.options = job->job.options;
        piece.lane = job->job.lane;
        piece.slot = job->job.slot;
        piece.phrases = job->job.phrases;
        piece.on_progress = job->job.on_progress;
        CancelSource piece_cancel(job->cancel.token());
        const Subscription generation_link =
            generation_token.on_cancel([piece_cancel]() mutable { piece_cancel.cancel(); });
        piece.cancel = piece_cancel.token();

        std::size_t piece_end = 0;
        std::size_t length = 0;
        double shift = 0.0;
        const auto* clip = std::get_if<AudioClip>(&job->job.input);
        try {
            const Capabilities capabilities = backend->capabilities();
            if (clip == nullptr) {
                if (!capabilities.accepts_files) {
                    throw EngineError("This speech engine does not take audio files.");
                }
                piece.input = job->job.input;
            } else {
                if (clip->samples == nullptr || clip->end < clip->begin ||
                    clip->end > clip->samples->size()) {
                    throw std::invalid_argument("The audio clip is outside its samples");
                }
                length = clip->end - clip->begin;
                piece_end = length;
                const std::size_t max_window =
                    static_cast<std::size_t>(std::max(1.0, capabilities.max_window_s * clip->rate));
                if (capabilities.windows && length - job->next >= max_window) {
                    // The same cut transcribe_audio makes inside one window.
                    piece_end = job->next + dsp::split_point(std::span<const float>(
                                                clip->samples->data() + clip->begin + job->next,
                                                length - job->next));
                }
                AudioClip window = *clip;
                window.begin = clip->begin + job->next;
                window.end = clip->begin + piece_end;
                shift = static_cast<double>(job->next) / clip->rate;
                window.offset_s = clip->offset_s + shift;
                piece.input = std::move(window);
            }
            const auto started = std::chrono::steady_clock::now();
            DecodeResult result = backend->decode(piece);
            job->decode_s +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (piece.cancel.cancelled()) {
                complete(job, std::unexpected(canceled()));
                return false;
            }
            job->texts.push_back(std::move(result.transcript.text));
            for (Segment& segment : result.transcript.segments) {
                segment.start += shift;
                segment.end += shift;
                job->transcript.segments.push_back(std::move(segment));
            }
            for (Word& word : result.transcript.words) {
                word.start += shift;
                word.end += shift;
                job->transcript.words.push_back(std::move(word));
            }
        } catch (const EngineError& error) {
            complete(job, std::unexpected(piece.cancel.cancelled() ? canceled() : error));
            return false;
        } catch (const std::exception& error) {
            complete(job, std::unexpected(EngineError(error.what())));
            return false;
        }

        if (clip != nullptr && piece_end < length) {
            job->next = piece_end;
            return true;
        }
        DecodeResult done;
        done.transcript = std::move(job->transcript);
        done.transcript.text = join_texts(job->texts);
        done.transcript.audio_processed =
            clip != nullptr ? static_cast<double>(length) / clip->rate : 0.0;
        done.decode_s = job->decode_s;
        done.generation = job->generation;
        complete(job, std::move(done));
        return false;
    }

    void loop() {
        set_current_thread_name("fv-speech");
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return stopping || queued(); });
            if (stopping) {
                break;
            }
            set_busy(true);
            if (!controls.empty()) {
                ControlTask task = std::move(controls.front());
                controls.pop_front();
                running_control = true;
                lock.unlock();
                run_control(task);
                lock.lock();
                running_control = false;
            } else {
                auto& lane = *std::ranges::find_if(lanes, [](const auto& l) { return !l.empty(); });
                std::shared_ptr<JobState> job = std::move(lane.front());
                lane.pop_front();
                const std::size_t index = lane_index(job->job.lane);
                running = job;
                lock.unlock();
                bool again = false;
                if (job->warmup) {
                    run_warmup(job);
                } else {
                    again = run_step(job);
                }
                lock.lock();
                running.reset();
                if (again) {
                    if (stopping) {
                        lock.unlock();
                        complete(job, std::unexpected(canceled()));
                        lock.lock();
                    } else {
                        // Its next window keeps its place: it was submitted before the rest.
                        lanes[index].push_front(std::move(job));
                    }
                }
            }
            if (!queued()) {
                set_busy(false);
            }
            idle.notify_all();
        }
        std::vector<std::shared_ptr<Backend>> loaded;
        for (SlotState& slot : slots) {
            if (slot.backend) {
                loaded.push_back(std::move(slot.backend));
                slot.backend.reset();
            }
        }
        lock.unlock();
        for (auto& backend : loaded) {
            backend->release();
            backend.reset();
        }
    }

    // The slot's backend as loaded now; null when nothing is.
    std::shared_ptr<Backend> loaded(Slot slot) const {
        std::lock_guard lock(mutex);
        return slots[resolve(slot)].backend;
    }
};

} // namespace detail

// --- JobHandle ---

JobHandle::JobHandle(std::uint64_t id, std::weak_ptr<detail::ServiceState> service)
    : id_(id), service_(std::move(service)) {}

std::uint64_t JobHandle::id() const {
    return id_;
}

void JobHandle::cancel() {
    if (auto service = service_.lock()) {
        service->cancel_job(id_);
    }
}

// --- EngineLease ---

EngineLease::EngineLease(std::weak_ptr<detail::ServiceState> service, std::uint64_t id)
    : service_(std::move(service)), id_(id) {}

EngineLease::EngineLease(EngineLease&& other) noexcept
    : service_(std::move(other.service_)), id_(std::exchange(other.id_, 0)) {}

EngineLease& EngineLease::operator=(EngineLease&& other) noexcept {
    if (this != &other) {
        release();
        service_ = std::move(other.service_);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

EngineLease::~EngineLease() {
    release();
}

void EngineLease::release() {
    const std::uint64_t id = std::exchange(id_, 0);
    auto service = service_.lock();
    service_.reset();
    if (id == 0 || !service) {
        return;
    }
    std::vector<detail::Bumped> bumped;
    {
        std::unique_lock lock(service->mutex);
        std::erase_if(service->leases, [id](const auto& lease) { return lease.first == id; });
        std::vector<std::pair<std::size_t, Readiness>> loading;
        if (service->leases.empty()) {
            for (std::size_t slot = 0; slot < kSlots; ++slot) {
                if (auto pending = std::move(service->slots[slot].pending)) {
                    service->slots[slot].pending.reset();
                    bumped.push_back(service->apply_select(slot, std::move(*pending)));
                    loading.emplace_back(slot, service->slots[slot].readiness);
                }
            }
        }
        service->emit_after_unlock(lock, std::move(loading));
    }
    for (auto& entry : bumped) {
        service->after_bump(entry);
    }
}

// --- SpeechService ---

SpeechService::SpeechService(BackendFactory factory, Executor& notify)
    : state_(std::make_shared<detail::ServiceState>(std::move(factory), notify)) {
    state_->owner = this;
    state_->thread = std::thread([state = state_] { state->loop(); });
}

void SpeechService::set_backend_factory(std::string backend, BackendFactory factory) {
    std::lock_guard lock(state_->mutex);
    if (factory) {
        state_->feature_factories.insert_or_assign(std::move(backend), std::move(factory));
    } else {
        state_->feature_factories.erase(backend);
    }
}

SpeechService::~SpeechService() {
    std::vector<std::shared_ptr<detail::JobState>> queued;
    std::optional<CancelSource> running;
    std::optional<CancelSource> loading;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        queued = state_->take_queued(std::nullopt);
        state_->controls.clear();
        if (state_->running) {
            running = state_->running->cancel;
        }
        // A load running now is told to stop: the join below waits for it either way, and
        // nothing will use what it loads. (The runtime it loaded stays loaded.)
        loading = state_->loading;
        state_->wake.notify_all();
    }
    if (running) {
        running->cancel();
    }
    if (loading) {
        loading->cancel();
    }
    state_->end_all(queued);
    if (state_->thread.joinable()) {
        if (state_->thread.get_id() == std::this_thread::get_id()) {
            state_->thread.detach(); // destroyed from its own callback: it ends on its own
        } else {
            state_->thread.join();
        }
    }
    std::lock_guard guard(state_->emit_mutex);
    state_->owner = nullptr;
}

JobHandle
SpeechService::submit(DecodeJob job, Executor& reply_on,
                      std::function<void(std::expected<DecodeResult, EngineError>)> done) {
    auto state = std::make_shared<detail::JobState>();
    state->job = std::move(job);
    state->reply_on = &reply_on;
    state->callback = std::move(done);
    state->cancel = CancelSource(state->job.cancel);

    std::vector<std::shared_ptr<detail::JobState>> replaced;
    bool refused = false;
    {
        std::lock_guard lock(state_->mutex);
        state->id = state_->next_id++;
        state->slot = state_->resolve(state->job.slot);
        state->generation = state_->slots[state->slot].generation;
        if (state_->stopping) {
            refused = true;
        } else {
            const std::string& key = state->job.coalesce_key;
            if (!key.empty()) {
                for (auto& lane : state_->lanes) {
                    for (auto it = lane.begin(); it != lane.end();) {
                        if ((*it)->job.coalesce_key == key && !(*it)->started()) {
                            replaced.push_back(std::move(*it));
                            it = lane.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }
            state_->lanes[lane_index(state->job.lane)].push_back(state);
            state_->wake.notify_all();
        }
    }
    state_->end_all(replaced);
    if (refused) {
        state_->complete(state, std::unexpected(canceled()));
        return JobHandle(state->id, state_);
    }
    // The job's own token: cancelling it is cancelling the handle. Subscribed outside the lock,
    // since a token cancelled already runs this at once.
    state->token_link = state->job.cancel.on_cancel(
        [weak = std::weak_ptr<detail::ServiceState>(state_), id = state->id] {
            if (auto service = weak.lock()) {
                service->cancel_job(id);
            }
        });
    return JobHandle(state->id, state_);
}

DecodeResult SpeechService::run(DecodeJob job) {
    assert_not_gui_thread();
    if (std::this_thread::get_id() == state_->thread.get_id()) {
        throw std::logic_error("SpeechService::run on the speech thread would wait for itself");
    }
    auto promise = std::make_shared<std::promise<Outcome>>();
    std::future<Outcome> future = promise->get_future();
    auto state = std::make_shared<detail::JobState>();
    state->job = std::move(job);
    state->callback = [promise](Outcome outcome) {
        promise->set_value(std::move(outcome));
    };
    state->cancel = CancelSource(state->job.cancel);
    bool refused = false;
    {
        std::lock_guard lock(state_->mutex);
        state->id = state_->next_id++;
        state->slot = state_->resolve(state->job.slot);
        state->generation = state_->slots[state->slot].generation;
        refused = state_->stopping;
        if (!refused) {
            state_->lanes[lane_index(state->job.lane)].push_back(state);
            state_->wake.notify_all();
        }
    }
    if (refused) {
        state_->complete(state, std::unexpected(canceled()));
    } else {
        state->token_link = state->job.cancel.on_cancel(
            [weak = std::weak_ptr<detail::ServiceState>(state_), id = state->id] {
                if (auto service = weak.lock()) {
                    service->cancel_job(id);
                }
            });
    }
    Outcome outcome = future.get();
    if (!outcome) {
        throw outcome.error();
    }
    return std::move(*outcome);
}

void SpeechService::select(Slot slot, BackendConfig config) {
    std::optional<detail::Bumped> bumped;
    {
        std::unique_lock lock(state_->mutex);
        if (state_->stopping) {
            return;
        }
        const std::size_t index = slot_index(slot);
        if (!state_->leases.empty()) {
            kLog.info("Engine reload deferred: the engine is leased");
            state_->slots[index].pending = std::move(config);
            return;
        }
        bumped = state_->apply_select(index, std::move(config));
        // Reaches `notify` before anything the load reports.
        state_->emit_after_unlock(lock, {{index, state_->slots[index].readiness}});
    }
    state_->after_bump(*bumped);
}

bool SpeechService::with_model_unused(std::string_view model_key,
                                      const std::function<void()>& work) {
    std::lock_guard assets(state_->model_assets);
    {
        std::lock_guard lock(state_->mutex);
        const auto matching = [model_key](const std::optional<BackendConfig>& config) {
            return config && config->backend == "parakeet" && config->model_key == model_key &&
                   !config->model_path.empty() && !config->missing_message;
        };
        if (state_->stopping || std::ranges::any_of(state_->slots, [&](const auto& slot) {
                return slot.resident_model_key == model_key || matching(slot.pending) ||
                       (slot.readiness.state == ReadyState::Loading && matching(slot.config));
            })) {
            return false;
        }
    }
    work();
    return true;
}

EngineLease SpeechService::lease(std::string_view holder) {
    std::lock_guard lock(state_->mutex);
    const std::uint64_t id = state_->next_id++;
    state_->leases.emplace_back(id, std::string(holder));
    return EngineLease(state_, id);
}

std::vector<std::string> SpeechService::lease_holders() const {
    std::lock_guard lock(state_->mutex);
    std::vector<std::string> holders;
    for (const auto& lease : state_->leases) {
        holders.push_back(lease.second);
    }
    return holders;
}

void SpeechService::release_local(Slot slot) {
    std::optional<detail::Bumped> bumped;
    const std::size_t index = slot_index(slot);
    {
        std::unique_lock lock(state_->mutex);
        detail::SlotState& state = state_->slots[index];
        if (state_->stopping || !state.config || state.released) {
            return;
        }
        state.released = true;
        state.pending.reset();
        bumped = state_->bump(index);
        state.readiness = Readiness{ReadyState::Offline, std::string(kReleased), ""};
        state_->controls.push_back(
            detail::ControlTask{detail::ControlTask::Kind::Release, index, state.generation, {}});
        state_->wake.notify_all();
        state_->emit_after_unlock(lock, {{index, state.readiness}});
    }
    state_->after_bump(*bumped);
}

void SpeechService::restore_local(Slot slot) {
    std::optional<detail::Bumped> bumped;
    const std::size_t index = slot_index(slot);
    {
        std::unique_lock lock(state_->mutex);
        detail::SlotState& state = state_->slots[index];
        if (state_->stopping || !state.released || !state.config) {
            return;
        }
        bumped = state_->apply_select(index, *state.config);
        state_->emit_after_unlock(lock, {{index, state.readiness}});
    }
    state_->after_bump(*bumped);
}

Readiness SpeechService::readiness(Slot slot) const {
    std::lock_guard lock(state_->mutex);
    return state_->slots[state_->resolve(slot)].readiness;
}

std::uint64_t SpeechService::generation(Slot slot) const {
    std::lock_guard lock(state_->mutex);
    return state_->slots[state_->resolve(slot)].generation;
}

void SpeechService::warm_up(Slot slot) {
    std::size_t index = 0;
    bool skip = false;
    {
        std::lock_guard lock(state_->mutex);
        index = state_->resolve(slot);
        // OpenWhisper's warm-up skips when another decode holds the engine: that decode warms it.
        // A load still running or queued is no decode: the warm-up waits for it and then warms
        // what it loaded.
        skip = state_->stopping || state_->jobs_queued() || state_->running;
        if (!skip) {
            auto job = std::make_shared<detail::JobState>();
            job->id = state_->next_id++;
            job->warmup = true;
            job->slot = index;
            job->generation = state_->slots[index].generation;
            job->job.lane = Lane::Warmup;
            state_->lanes[lane_index(Lane::Warmup)].push_back(std::move(job));
            state_->wake.notify_all();
        }
    }
    if (skip) {
        state_->emit_warmed(index, false);
    }
}

bool SpeechService::supports_recognition(Slot slot) const {
    const auto backend = state_->loaded(slot);
    return backend != nullptr && backend->supports_recognition();
}

bool SpeechService::accepts_files(Slot slot) const {
    const auto backend = state_->loaded(slot);
    return backend != nullptr && backend->capabilities().accepts_files;
}

bool SpeechService::live_preview(Slot slot) const {
    const auto backend = state_->loaded(slot);
    return backend != nullptr && backend->capabilities().live_preview;
}

bool SpeechService::prepares_for_transcription(Slot slot) const {
    const auto backend = state_->loaded(slot);
    return backend != nullptr && backend->prepares_for_transcription();
}

void SpeechService::prepare_for_transcription(Slot slot) {
    assert_not_gui_thread();
    if (const auto backend = state_->loaded(slot)) {
        backend->prepare_for_transcription();
    }
}

std::optional<double> SpeechService::large_file_size_mb(Slot slot,
                                                        const std::filesystem::path& path) const {
    const auto backend = state_->loaded(slot);
    if (backend == nullptr) {
        return std::nullopt;
    }
    return backend->large_file_size_mb(path);
}

void SpeechService::cancel_all(std::chrono::milliseconds budget) {
    std::vector<std::shared_ptr<detail::JobState>> queued;
    std::optional<CancelSource> running;
    {
        std::lock_guard lock(state_->mutex);
        queued = state_->take_queued(std::nullopt);
        if (state_->running) {
            running = state_->running->cancel;
        }
    }
    if (running) {
        running->cancel();
    }
    state_->end_all(queued);
    // A model load running now is waited for as a decode window is (it cannot be cut short
    // without cancelling the slot's generation, which a later job would inherit).
    std::unique_lock lock(state_->mutex);
    state_->idle.wait_for(lock, budget,
                          [this] { return !state_->running && !state_->running_control; });
}

} // namespace fv::speech
