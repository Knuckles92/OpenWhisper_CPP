#include "remote/remote_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "base/identity.h"
#include "base/log.h"
#include "engine/error.h"
#include "remote/meeting_route.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "speech/dictation_language.h"
#include "speech/window_decode.h"

namespace fv::remote {
namespace {

using namespace std::chrono_literals;

constexpr log::Logger kLog{"transcriber.remote_backend"};

constexpr std::string_view kNotConnected = "Remote engine is not connected";
constexpr std::string_view kCanceled = "Transcription canceled";

const Json& member(const Json& object, std::string_view key) {
    static const Json empty;
    if (!object.is_object()) {
        return empty;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? empty : *found;
}

std::string text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

std::string field(const Json& object, std::string_view key) {
    return text(member(object, key));
}

bool flag(const Json& object, std::string_view key) {
    const auto& value = member(object, key);
    return value.is_boolean() && value.get<bool>();
}

void check_cancel(const CancelToken& cancel) {
    if (cancel.cancelled()) {
        throw EngineError(std::string(kCanceled));
    }
}

// _serves: the host already runs `model` and has it loaded.
bool serves(const Json& ready, const HostModel& model) {
    const auto& engine = member(ready, "engine");
    return flag(engine, "available") && field(engine, "family") == model.family &&
           field(engine, "model") == model.model;
}

Transcript transcript(const Json& result) {
    Transcript out;
    out.text = field(result, "text");
    const auto& segments = member(result, "segments");
    if (segments.is_array()) {
        for (const auto& item : segments) {
            const auto& start = member(item, "start");
            const auto& end = member(item, "end");
            if (start.is_number() && end.is_number()) {
                const double first = start.get<double>();
                const double last = end.get<double>();
                if (std::isfinite(first) && std::isfinite(last) && first >= 0 && last >= first) {
                    out.segments.push_back({field(item, "text"), first, last});
                }
            }
        }
    }
    return out;
}

bool same(const speech::Readiness& a, const speech::Readiness& b) {
    return a.state == b.state && a.device_info == b.device_info && a.message == b.message;
}

} // namespace

// What the SpeechService hears between loads. A report for a connection that has since been
// replaced is dropped, and so is one that repeats the last, so a host that stays away does not
// rewrite the status line at every retry.
struct RemoteSpeechBackend::LiveLinkWatch {
    std::atomic<std::uint64_t> generation{0};
    Signal<std::uint64_t, speech::Readiness> changes;
    std::mutex mutex;
    std::optional<speech::Readiness> last;

    // What a load returned: reported by the service itself.
    void note(speech::Readiness read) {
        std::lock_guard lock(mutex);
        last = std::move(read);
    }

    void report(std::uint64_t of, const speech::Readiness& read) {
        {
            std::lock_guard lock(mutex);
            if (of != generation.load() || (last && same(*last, read))) {
                return;
            }
            last = read;
        }
        changes.emit(of, read);
    }
};

// The link's own retries (services/runtime/remote.py's watch). Tasks posted to `executor` reach
// the backend through `owner`, which its destructor clears after waiting for a running one.
struct RemoteSpeechBackend::Keeper {
    explicit Keeper(Executor& on, RemoteSpeechBackend* backend) : executor(on), owner(backend) {}

    Executor& executor;
    std::mutex mutex;
    RemoteSpeechBackend* owner;
    // What a LinkHandle reads (link(), model_choices()): shared while a reader uses the backend,
    // taken alone by the destructor before it goes. Never held across a reconnect.
    std::shared_mutex readers;
    RemoteSpeechBackend* readable = owner;
    // Held while a task uses the owner.
    std::mutex running;
    // The thread inside a task, so that an executor that runs what it is handed at once (a
    // test's inline one) neither deadlocks nor retries in a loop: its nested task is skipped.
    std::atomic<std::thread::id> runner{};
    TimerHandle timer;
    std::size_t step = 0;
    bool restoring = false;
    // Released: nothing reconnects until the next load.
    bool stopped = false;
    std::optional<std::chrono::steady_clock::time_point> retry_at;

    template <class Work> static void run(const std::weak_ptr<Keeper>& weak, Work&& work) {
        const auto keeper = weak.lock();
        if (!keeper || keeper->runner.load() == std::this_thread::get_id()) {
            return;
        }
        std::lock_guard busy(keeper->running);
        RemoteSpeechBackend* backend = nullptr;
        {
            std::lock_guard lock(keeper->mutex);
            backend = keeper->owner;
        }
        if (backend) {
            keeper->runner = std::this_thread::get_id();
            struct Done {
                Keeper& keeper;
                ~Done() { keeper.runner = std::thread::id{}; }
            } done{*keeper};
            work(*backend);
        }
    }
};

RemoteSpeechBackend::RemoteSpeechBackend(PairingProvider pairing, ConnectionFactory connect,
                                         Clock& clock, std::shared_ptr<Json> meeting_route,
                                         bool watch_live_link, Executor* reconnect_on)
    : pairing_provider_(std::move(pairing)), connection_factory_(std::move(connect)), clock_(clock),
      meeting_route_(std::move(meeting_route)),
      live_link_(watch_live_link && !meeting_route_ ? std::make_shared<LiveLinkWatch>() : nullptr),
      keeper_(live_link_ && reconnect_on ? std::make_shared<Keeper>(*reconnect_on, this)
                                         : nullptr) {}

RemoteSpeechBackend::~RemoteSpeechBackend() {
    if (!keeper_) {
        return;
    }
    {
        std::unique_lock readers(keeper_->readers);
        keeper_->readable = nullptr;
    }
    {
        std::lock_guard lock(keeper_->mutex);
        keeper_->owner = nullptr;
        keeper_->timer.cancel();
    }
    // A reconnect that is under way gives up once its connection is closed.
    close_link();
    std::lock_guard wait(keeper_->running);
}

Subscription RemoteSpeechBackend::watch_readiness(Executor& executor,
                                                  std::function<void(speech::Readiness)> notify) {
    if (!live_link_) {
        return {};
    }
    return live_link_->changes.connect(
        executor, [weak = std::weak_ptr(live_link_),
                   notify = std::move(notify)](std::uint64_t generation, speech::Readiness read) {
            if (const auto watch = weak.lock(); watch && watch->generation == generation) {
                notify(std::move(read));
            }
        });
}

speech::Capabilities RemoteSpeechBackend::capabilities() const {
    std::lock_guard lock(mutex_);
    speech::Capabilities caps;
    caps.live_preview = field(engine_, "family") == "parakeet";
    const auto& codes = member(runtime_, "languages");
    if (codes.is_array()) {
        for (const auto& code : codes) {
            if (code.is_string()) {
                caps.languages.push_back(code.get<std::string>());
            }
        }
    }
    return caps;
}

bool RemoteSpeechBackend::supports_recognition() const {
    std::lock_guard lock(mutex_);
    return hints_ && connection_ && !connection_->closed();
}

std::string RemoteSpeechBackend::recognition_support() const {
    return supports_recognition() ? "model" : "after";
}

void RemoteSpeechBackend::set_connecting(bool connecting) {
    {
        std::lock_guard lock(mutex_);
        connecting_ += connecting ? 1 : -1;
    }
    link_changed.emit();
}

RemoteSpeechBackend::Opened RemoteSpeechBackend::open(const ClientPairing& pairing,
                                                      const CancelToken& cancel,
                                                      std::optional<std::uint64_t> generation) {
    check_cancel(cancel);
    std::shared_ptr<RemoteConnection> connection = connection_factory_(pairing);
    if (generation) {
        // A release() midway closes it.
        std::lock_guard lock(mutex_);
        if (*generation != generation_) {
            throw std::runtime_error(std::string(kCanceled));
        }
        pending_ = connection;
    }
    Json ready = connection->connect(cancel);
    check_cancel(cancel);
    if (meeting_route_) {
        *meeting_route_ = checked_meeting_engine(*meeting_route_, ready);
    }
    return {std::move(connection), std::move(ready)};
}

RemoteSpeechBackend::Opened
RemoteSpeechBackend::switch_host(Opened opened, const ClientPairing& pairing,
                                 const HostModel& requested, const CancelToken& cancel,
                                 std::uint64_t generation, const std::optional<Json>& runtime) {
    // The host answers once the load has settled. Whether it switched or said why it couldn't,
    // its engine may differ from the one this connection was opened on, so a fresh connection is
    // what gets adopted.
    const auto host = pairing.name_for(field(member(opened.ready, "host"), "name"));
    kLog.info("Asking {} to switch to {}", host, requested.label);
    try {
        if (runtime && !flag(member(opened.ready, "capabilities"), "engine_controls")) {
            throw std::runtime_error(std::format("Update {} on {} to change its runtime from here.",
                                                 identity().product, host));
        }
        Json fields{{"family", requested.family}, {"model", requested.model}};
        if (runtime) {
            fields["settings"] = *runtime;
        }
        (void)opened.connection->request_timed(runtime ? "configure_runtime" : "select_model",
                                               fields, SWITCH_REQUEST_TIMEOUT, {}, {}, cancel);
    } catch (const RemoteEngineError&) {
        throw;
    } catch (const std::exception& error) {
        if (opened.connection->closed() || cancel.cancelled()) {
            throw; // canceled, or the host never answered
        }
        std::string message = error.what();
        if (message.starts_with("Unknown operation")) {
            message = std::format("Update {} on {} to choose its model from here.",
                                  identity().product, host);
        }
        kLog.info("{} kept its model: {}", host, message);
        std::lock_guard lock(mutex_);
        switch_error_ = std::move(message);
    }
    opened.connection->close();
    return open(pairing, cancel, generation);
}

bool RemoteSpeechBackend::adopt(const ClientPairing& pairing, Opened opened, bool announce) {
    const auto& ready = opened.ready;
    const auto& engine = member(ready, "engine");
    const auto& runtime = member(ready, "runtime");
    const auto& capabilities = member(ready, "capabilities");
    const auto name = pairing.name_for(field(member(ready, "host"), "name"));
    const bool available = flag(engine, "available");
    std::shared_ptr<RemoteConnection> adopted;
    const auto generation = live_link_ ? ++live_link_->generation : 0;
    std::shared_ptr<RemoteConnection> old;
    {
        std::lock_guard lock(mutex_);
        pairing_ = pairing;
        adopted_ = pairing;
        unpaired_ = false;
        engine_ = engine.is_object() ? engine : Json::object();
        adopt_runtime(ready);
        models_ = parse_host_models(member(ready, "models"));
        host_name_ = name;
        hints_ = flag(capabilities, "recognition_hints");
        const auto status = field(engine, "status");
        error_ = available
                     ? std::string()
                     : name + ": " + (status.empty() ? "its speech engine is not loaded" : status);
        old = std::move(connection_);
        if (available) {
            connection_ = std::move(opened.connection);
            adopted = connection_;
        }
    }
    if (old) {
        old->close();
    }
    if (opened.connection) {
        opened.connection->close();
    }
    if (available) {
        kLog.info("Remote engine connected: {}", device_info());
    } else {
        kLog.info("Remote engine host has no engine ready: {}", last_error());
    }
    if (!available && live_link_) {
        speech::forget_remote_runtime(pairing.fingerprint);
    }
    if (adopted && live_link_) {
        speech::RemoteVoiceCapabilities announced{
            .host = name,
            .engine_label = field(engine, "label"),
            .family = field(engine, "family"),
            .own_language = field(member(runtime, "selected"), "language"),
            .recognition_hints = flag(capabilities, "recognition_hints"),
        };
        if (const auto& languages = member(runtime, "languages"); languages.is_array()) {
            announced.languages.emplace();
            for (const auto& code : languages) {
                if (code.is_string()) {
                    announced.languages->push_back(code.get<std::string>());
                }
            }
        }
        speech::note_remote_runtime(pairing.fingerprint, std::move(announced));
        if (announce) {
            live_link_->report(generation, {speech::ReadyState::Ready, device_info(), {}});
        }
        // The observer runs on the network's thread: it only reports, and hands the rest (the
        // drop, the retries) to the keeper's executor.
        adopted->watch_closed([weak = std::weak_ptr(live_link_), keeper = std::weak_ptr(keeper_),
                               generation, name,
                               fingerprint = pairing.fingerprint](std::uint16_t code) {
            const auto watch = weak.lock();
            // A connection this backend closed itself, or has replaced, is not a lost link.
            if (!watch || watch->generation != generation) {
                return;
            }
            speech::forget_remote_runtime(fingerprint);
            const auto message = code == CLOSE_ENGINE_CHANGED
                                     ? name + " switched engines. Reconnecting..."
                                     : name + " stopped answering.";
            watch->report(generation, {speech::ReadyState::Offline, message, message});
            if (const auto held = keeper.lock()) {
                held->executor.post(
                    [keeper] { Keeper::run(keeper, [](auto& self) { self.link_lost(); }); });
            }
        });
    }
    link_changed.emit();
    return available;
}

void RemoteSpeechBackend::close_link() {
    if (live_link_) {
        ++live_link_->generation;
    }
    cancel_transcription();
    std::shared_ptr<RemoteConnection> current;
    std::shared_ptr<RemoteConnection> pending;
    std::optional<CancelSource> attempt;
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        current = std::move(connection_);
        pending = std::move(pending_);
        attempt = std::move(pending_cancel_);
        pending_cancel_.reset();
        if (live_link_ && pairing_) {
            speech::forget_remote_runtime(pairing_->fingerprint);
        }
    }
    if (attempt) {
        attempt->cancel();
    }
    if (current) {
        current->close();
    }
    if (pending) {
        pending->close();
    }
}

void RemoteSpeechBackend::fail_load(std::uint64_t generation,
                                    const std::optional<ClientPairing>& pairing,
                                    std::string message, bool forget_models) {
    std::lock_guard lock(mutex_);
    if (generation != generation_) {
        return; // a release or another load took over
    }
    if (pairing) {
        host_name_ = pairing->host_name;
    }
    error_ = std::move(message);
    if (forget_models) {
        models_.reset();
    }
}

speech::Readiness RemoteSpeechBackend::load(const CancelToken& superseded) {
    const auto offline = [this] {
        const std::string message = device_info();
        return speech::Readiness{speech::ReadyState::Offline, message, message};
    };
    if (superseded.cancelled()) {
        return offline();
    }
    close_link();
    cancel_retry();
    if (keeper_) {
        std::lock_guard lock(keeper_->mutex);
        keeper_->stopped = false;
    }
    std::uint64_t generation = 0;
    std::optional<HostModel> requested;
    std::optional<Json> runtime;
    CancelSource attempt(superseded);
    {
        std::lock_guard lock(mutex_);
        generation = generation_;
        requested = std::exchange(requested_, std::nullopt);
        runtime = std::exchange(requested_runtime_, std::nullopt);
        switch_error_.clear();
        pending_cancel_ = attempt;
    }
    const auto cancel = attempt.token();
    bool available = false;
    bool incomplete = false;
    bool connecting = false;
    try {
        const auto pairing = pairing_provider_();
        {
            std::lock_guard lock(mutex_);
            unpaired_ = !pairing;
        }
        if (!pairing) {
            fail_load(generation, {}, "Pair with a host in Settings → Remote engine.", true);
        } else {
            {
                std::lock_guard lock(mutex_);
                pairing_ = pairing;
                if (host_name_.empty()) {
                    host_name_ = pairing->host_name;
                }
            }
            set_connecting(true);
            connecting = true;
            try {
                auto opened = open(*pairing, cancel, generation);
                if (requested && (runtime || !serves(opened.ready, *requested))) {
                    opened = switch_host(std::move(opened), *pairing, *requested, cancel,
                                         generation, runtime);
                }
                bool current = false;
                {
                    std::lock_guard lock(mutex_);
                    current = generation == generation_ && !cancel.cancelled();
                }
                if (current) {
                    available = adopt(*pairing, std::move(opened), false);
                } else {
                    opened.connection->close();
                }
            } catch (const RemoteHostBusy& busy) {
                std::string message = busy.what();
                if (busy.retry_after_s && std::isfinite(*busy.retry_after_s) &&
                    *busy.retry_after_s > 0) {
                    message += std::format(" Try again in about {:.0f} seconds.",
                                           std::ceil(*busy.retry_after_s));
                }
                fail_load(generation, pairing, std::move(message), true);
            } catch (const RemotePairingIncomplete& error) {
                incomplete = true;
                fail_load(generation, pairing, error.what(), true);
            } catch (const RemoteEngineError& error) {
                fail_load(generation, pairing, error.what(), true);
            }
        }
    } catch (const std::exception& error) {
        // A switch that outlasted its timeout, or a pairing the provider refuses. A release or
        // a newer load that canceled this one leaves no error behind.
        if (!cancel.cancelled()) {
            fail_load(generation, {}, error.what(), false);
        }
    }
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        if (generation == generation_) {
            pending_cancel_.reset();
        }
    }
    if (connecting) {
        set_connecting(false);
    } else {
        link_changed.emit();
    }
    speech::Readiness readiness = offline();
    if (available) {
        readiness = {speech::ReadyState::Ready, device_info(), switch_error()};
    }
    if (live_link_) {
        live_link_->note(readiness);
    }
    if (!available && !superseded.cancelled() && !incomplete) {
        // A host that could not be reached is tried again; one never paired, or whose pairing
        // lost its token, is not.
        schedule_retry();
    }
    return readiness;
}

void RemoteSpeechBackend::release() {
    if (keeper_) {
        std::lock_guard lock(keeper_->mutex);
        keeper_->stopped = true;
    }
    cancel_retry();
    close_link();
    {
        std::lock_guard lock(mutex_);
        pairing_.reset();
        adopted_.reset();
        engine_ = Json::object();
        runtime_ = Json::object();
        models_.reset();
        host_name_.clear();
        error_.clear();
        hints_ = false;
    }
    link_changed.emit();
}

void RemoteSpeechBackend::drop(const std::shared_ptr<RemoteConnection>& connection,
                               std::string reason) {
    bool dropped = false;
    std::string fingerprint;
    {
        std::lock_guard lock(mutex_);
        if (connection_ == connection) {
            dropped = connection_ != nullptr;
            connection_.reset();
            error_ = std::move(reason);
            if (pairing_) {
                fingerprint = pairing_->fingerprint;
            }
        }
    }
    if (connection) {
        connection->close();
    }
    if (dropped && live_link_) {
        speech::forget_remote_runtime(fingerprint);
        // Said now, not at the next recording; then tried again on the keeper's schedule.
        report(speech::ReadyState::Offline);
        schedule_retry();
    }
    link_changed.emit();
}

void RemoteSpeechBackend::cancel_transcription() {
    std::optional<CancelSource> cancel;
    {
        std::lock_guard lock(mutex_);
        cancel = active_;
    }
    if (cancel) {
        cancel->cancel();
    }
}

void RemoteSpeechBackend::set_busy_retry(double total_s, double max_delay_s) {
    std::lock_guard lock(mutex_);
    busy_retry_s_ = total_s;
    busy_retry_max_delay_s_ = max_delay_s;
}

RemoteLink RemoteSpeechBackend::link() const {
    bool known = false;
    {
        std::lock_guard lock(mutex_);
        known = unpaired_.has_value();
    }
    if (!known) {
        // Once, before the first load: the pairing comes from the settings file.
        std::optional<ClientPairing> pairing;
        bool unpaired = false;
        try {
            pairing = pairing_provider_();
            unpaired = !pairing;
        } catch (const std::exception&) {
            unpaired = false; // paired, though not with the host this backend is for
        }
        std::lock_guard lock(mutex_);
        if (!unpaired_) {
            unpaired_ = unpaired;
        }
        if (pairing && host_name_.empty()) {
            host_name_ = pairing->host_name;
        }
    }
    RemoteLink out;
    std::shared_ptr<RemoteConnection> current;
    bool connecting = false;
    bool unpaired = false;
    std::string detail;
    {
        std::lock_guard lock(mutex_);
        current = connection_;
        connecting = connecting_ > 0;
        unpaired = unpaired_.value_or(false);
        detail = error_;
        out.host = host_name_;
        out.busy = inflight_ > 0;
        out.replies = replies_;
        out.engine_label = field(engine_, "label");
        out.device = field(engine_, "device");
        out.compute_type = field(engine_, "compute_type");
        out.runtime_status = field(engine_, "status");
        const auto& gpu = member(runtime_, "gpu");
        out.gpu_name = field(gpu, "name");
        const auto& memory = member(gpu, "total_mib");
        if (memory.is_number_integer()) {
            out.gpu_memory_mib = memory.get<std::int64_t>();
        }
    }
    out.retry_at = retry_at();
    if (current && current->alive()) {
        out.state = "connected";
        out.address = current->where();
        out.route = current->via_tailscale() ? "Tailscale" : "local network";
        if (const auto latency = current->latency()) {
            out.latency_ms = *latency * 1000;
        }
        out.beat = current->beats();
    } else if (connecting) {
        out.state = "connecting";
    } else {
        out.state = unpaired ? "unpaired" : "offline";
        out.detail = std::move(detail);
    }
    return out;
}

RemoteModels RemoteSpeechBackend::model_choices() const {
    std::optional<ClientPairing> pairing;
    try {
        pairing = pairing_provider_();
    } catch (const std::exception&) {
        pairing.reset();
    }
    if (!pairing) {
        return {};
    }
    std::lock_guard lock(mutex_);
    RemoteModels result;
    result.host = pairing->host_name;
    result.models = models_;
    if (connection_ && !connection_->closed()) {
        const auto family = field(engine_, "family");
        const auto model = field(engine_, "model");
        const auto label = field(engine_, "label");
        result.current = HostModel{family.empty() ? std::string(REMOTE_BACKEND) : family, model,
                                   label.empty() ? model : label};
        result.engine = engine_;
        if (runtime_.is_object() && !runtime_.empty()) {
            result.runtime = runtime_;
        }
    }
    return result;
}

bool RemoteSpeechBackend::is_available() const {
    std::lock_guard lock(mutex_);
    return connection_ && !connection_->closed();
}

std::string RemoteSpeechBackend::name() const {
    std::lock_guard lock(mutex_);
    const auto label = field(engine_, "label");
    if (!label.empty() && !host_name_.empty()) {
        return std::format("{} on {}", label, host_name_);
    }
    return "Remote engine";
}

std::string RemoteSpeechBackend::device_info() const {
    {
        std::lock_guard lock(mutex_);
        if (!connection_ || connection_->closed()) {
            return error_.empty() ? std::string(kNotConnected) : error_;
        }
    }
    const auto shown = name();
    std::lock_guard lock(mutex_);
    auto device = field(engine_, "device");
    const auto compute = field(engine_, "compute_type");
    if (!device.empty() && !compute.empty()) {
        device = std::format("{} ({})", device, compute);
    }
    return device.empty() ? shown : std::format("{} | {}", shown, device);
}

std::string RemoteSpeechBackend::host_name() const {
    std::lock_guard lock(mutex_);
    return host_name_;
}

std::string RemoteSpeechBackend::family() const {
    std::lock_guard lock(mutex_);
    return field(engine_, "family");
}

std::string RemoteSpeechBackend::model_name() const {
    std::lock_guard lock(mutex_);
    return field(engine_, "model");
}

std::string RemoteSpeechBackend::last_error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

std::string RemoteSpeechBackend::switch_error() const {
    std::lock_guard lock(mutex_);
    return switch_error_;
}

HostModel RemoteSpeechBackend::request_model(std::string_view family, std::string_view model,
                                             std::optional<Json> runtime) {
    std::lock_guard lock(mutex_);
    HostModel choice{std::string(family), std::string(model), std::string(model)};
    if (models_) {
        const auto listed = std::ranges::find_if(*models_, [&](const HostModel& entry) {
            return entry.family == family && entry.model == model;
        });
        if (listed != models_->end()) {
            choice = *listed;
        }
    }
    requested_ = choice;
    requested_runtime_ = std::move(runtime);
    return choice;
}

RemoteModelSwitch plan_remote_model_switch(const RemoteModels& state, std::string_view family,
                                           std::string_view model, bool meeting_active,
                                           bool recording_or_transcribing) {
    RemoteModelSwitch plan;
    if (state.current && state.current->family == family && state.current->model == model) {
        plan.skip = true;
        return plan;
    }
    if (meeting_active) {
        plan.refusal = "End the meeting before changing the engine";
        return plan;
    }
    if (recording_or_transcribing) {
        plan.refusal = "Finish recording before changing the engine";
        return plan;
    }
    std::string label(model);
    if (state.models) {
        for (const auto& entry : *state.models) {
            if (entry.family == family && entry.model == model) {
                label = entry.label;
                break;
            }
        }
    }
    const std::string host = state.host.empty() ? "the remote computer" : state.host;
    plan.note = std::format("Switching {} to {}...", host, label);
    return plan;
}

RemoteModelSwitch plan_remote_runtime_change(const RemoteModels& state, std::string_view family,
                                             std::string_view model, bool busy) {
    RemoteModelSwitch plan;
    if (busy) {
        plan.refusal = "Finish the current activity before changing the remote runtime";
        return plan;
    }
    plan.refusal = runtime_request_refusal(state, family, model);
    if (plan.refusal.empty()) {
        plan.note = std::format("Updating the runtime on {}...",
                                state.host.empty() ? "the remote computer" : state.host);
    }
    return plan;
}

std::string runtime_request_refusal(const RemoteModels& state, std::string_view family,
                                    std::string_view model) {
    if (!state.current || state.current->family != family || state.current->model != model) {
        return "The host changed engines. Reconnect before changing its runtime.";
    }
    if (!state.runtime || !flag(*state.runtime, "can_configure")) {
        return std::format("Update {} on the host to change its runtime from here.",
                           identity().product);
    }
    return {};
}

void RemoteSpeechBackend::request_runtime(std::string_view family, std::string_view model,
                                          const Json& changes) {
    std::lock_guard lock(mutex_);
    const bool connected = connection_ && !connection_->closed();
    const auto served = field(engine_, "family");
    const auto current_family = served.empty() ? std::string(REMOTE_BACKEND) : served;
    if (!connected || current_family != family || field(engine_, "model") != model) {
        throw std::invalid_argument(
            "The host changed engines. Reconnect before changing its runtime.");
    }
    if (runtime_.empty() || !flag(runtime_, "can_configure")) {
        throw std::invalid_argument(std::format(
            "Update {} on the host to change its runtime from here.", identity().product));
    }
    const auto label = field(engine_, "label");
    requested_ =
        HostModel{current_family, std::string(model), label.empty() ? std::string(model) : label};
    requested_runtime_ = changes.is_object() ? changes : Json::object();
}

std::string RemoteSpeechBackend::request_language() const {
    std::lock_guard lock(mutex_);
    return field(member(runtime_, "selected"), "language");
}

void RemoteSpeechBackend::adopt_runtime(const Json& ready) {
    const auto& runtime = member(ready, "runtime");
    runtime_ = runtime.is_object() ? runtime : Json::object();
    if (!runtime_.empty()) {
        runtime_["can_configure"] = flag(member(ready, "capabilities"), "engine_controls");
    }
    hints_ = flag(member(ready, "capabilities"), "recognition_hints");
}

bool RemoteSpeechBackend::refresh_host_catalog(const ClientPairing& pairing, const Json& ready,
                                               const Json& catalog) {
    const auto& engine = member(catalog, "engine");
    {
        std::lock_guard lock(mutex_);
        if (!adopted_ || !(*adopted_ == pairing) || !connection_ || !connection_->alive()) {
            return false;
        }
        // Engine changes still go through the normal reconnect and load.
        for (const char* key : {"family", "model", "device", "compute_type"}) {
            if (member(engine, key) != member(engine_, key)) {
                return false;
            }
        }
        Json merged = ready.is_object() ? ready : Json::object();
        merged["runtime"] = member(catalog, "runtime");
        adopt_runtime(merged);
        if (flag(catalog, "can_select")) {
            Json usable = Json::array();
            const auto& listed = member(catalog, "models");
            if (listed.is_array()) {
                for (const auto& item : listed) {
                    if (flag(item, "cached") && flag(item, "runtime_ready")) {
                        usable.push_back(item);
                    }
                }
            }
            models_ = parse_host_models(usable);
        } else {
            models_ = std::vector<HostModel>{};
        }
    }
    link_changed.emit();
    return true;
}

RemoteSpeechBackend::TimingMark RemoteSpeechBackend::timing_mark() const {
    std::lock_guard lock(mutex_);
    return timing_;
}

RemoteTiming RemoteSpeechBackend::timing_since(const TimingMark& mark) const {
    std::lock_guard lock(mutex_);
    RemoteTiming out;
    out.host = host_name_;
    out.requests = std::get<0>(timing_) - std::get<0>(mark);
    out.round_trip_s = std::get<1>(timing_) - std::get<1>(mark);
    const auto timed = std::get<3>(timing_) - std::get<3>(mark);
    // Every request timed, or the split would be a guess.
    if (out.requests != 0 && timed == out.requests) {
        out.host_s = std::get<2>(timing_) - std::get<2>(mark);
    }
    return out;
}

void RemoteSpeechBackend::host_renamed(const ClientPairing& pairing) {
    {
        std::lock_guard lock(mutex_);
        if (!pairing_ || *pairing_ != pairing) {
            return;
        }
        pairing_ = pairing;
        if (adopted_) {
            adopted_ = pairing;
        }
        host_name_ = pairing.host_name;
    }
    link_changed.emit();
}

void RemoteSpeechBackend::keep(Subscription subscription) {
    kept_.push_back(std::move(subscription));
}

bool RemoteSpeechBackend::check_link() {
    std::shared_ptr<RemoteConnection> current;
    std::string host;
    {
        std::lock_guard lock(mutex_);
        current = connection_;
        host = host_name_;
    }
    if (!current || current->alive()) {
        return false;
    }
    if (host.empty()) {
        host = current->where();
    }
    if (current->closed_for_engine_change()) {
        // The host pushes this when its engine changes; the retry adopts the new one.
        kLog.info("Remote engine {} switched engines; reconnecting", host);
        drop(current, host + " switched engines. Reconnecting...");
        return true;
    }
    kLog.info("Remote engine connection to {} closed while idle", host);
    drop(current, host + " stopped answering.");
    return true;
}

bool RemoteSpeechBackend::link_alive() {
    (void)check_link();
    std::lock_guard lock(mutex_);
    return connection_ != nullptr;
}

std::string RemoteSpeechBackend::restore_link() {
    std::uint64_t generation = 0;
    std::optional<ClientPairing> previous;
    std::string known_family, known_model;
    {
        std::lock_guard lock(mutex_);
        if (connection_ && !connection_->closed()) {
            return "connected";
        }
        generation = generation_;
        previous = adopted_;
        known_family = field(engine_, "family");
        known_model = field(engine_, "model");
    }
    std::optional<ClientPairing> pairing;
    try {
        pairing = pairing_provider_();
    } catch (const std::exception& error) {
        fail_load(generation, {}, error.what(), false);
        return "offline";
    }
    {
        std::lock_guard lock(mutex_);
        unpaired_ = !pairing;
    }
    if (!pairing) {
        link_changed.emit();
        return "unpaired";
    }
    CancelSource attempt;
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_) {
            return "offline";
        }
        pairing_ = pairing;
        pending_cancel_ = attempt;
    }
    set_connecting(true);
    std::optional<Opened> opened;
    std::string outcome = "offline";
    try {
        opened = open(*pairing, attempt.token(), generation);
    } catch (const RemotePairingIncomplete& error) {
        // Nothing to present to the host: no retry can succeed until this computer pairs again.
        {
            std::lock_guard lock(mutex_);
            pending_.reset();
            if (generation == generation_) {
                pending_cancel_.reset();
                error_ = error.what();
            }
        }
        set_connecting(false);
        link_changed.emit();
        return "unpaired";
    } catch (const RemoteEngineError& error) {
        std::lock_guard lock(mutex_);
        if (generation == generation_) {
            if (host_name_.empty()) {
                host_name_ = pairing->host_name;
            }
            error_ = error.what();
        }
    } catch (const EngineError& error) {
        // A meeting's host that no longer serves the pinned model, or a cancel.
        if (!attempt.cancelled()) {
            fail_load(generation, {}, error.what(), false);
        }
    } catch (const std::exception&) {
        // A release or a load took over.
    }
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        if (generation == generation_) {
            pending_cancel_.reset();
        }
    }
    if (opened) {
        const auto& engine = member(opened->ready, "engine");
        const bool same_engine = previous && previous->fingerprint == pairing->fingerprint &&
                                 flag(engine, "available") &&
                                 field(engine, "family") == known_family &&
                                 field(engine, "model") == known_model;
        bool current = false;
        {
            std::lock_guard lock(mutex_);
            current = generation == generation_;
        }
        if (!current) {
            opened->connection->close();
        } else {
            const auto where = opened->connection->where();
            const bool available = adopt(*pairing, std::move(*opened));
            outcome = same_engine ? "connected" : available ? "changed" : "offline";
            kLog.info("Remote engine reconnect: {} ({})", outcome, where);
        }
    }
    set_connecting(false);
    return outcome;
}

bool RemoteSpeechBackend::reconnect(const std::shared_ptr<RemoteConnection>& stale,
                                    const CancelToken& cancel) {
    std::optional<ClientPairing> pairing;
    try {
        pairing = pairing_provider_();
    } catch (const std::exception&) {
        return false;
    }
    std::string old_family, old_model;
    {
        std::lock_guard lock(mutex_);
        if (!pairing || !pairing_ || *pairing != *pairing_ || connection_ != stale) {
            return false;
        }
        old_family = field(engine_, "family");
        old_model = field(engine_, "model");
    }
    try {
        auto opened = open(*pairing, cancel);
        const auto& engine = member(opened.ready, "engine");
        // A different engine changes which features apply; that takes a full reload, not a
        // silent swap mid-dictation.
        if (!flag(engine, "available") || field(engine, "family") != old_family ||
            field(engine, "model") != old_model) {
            return false;
        }
        const auto where = opened.connection->where();
        const bool adopted = adopt(*pairing, std::move(opened));
        kLog.info("Remote engine reconnected to {}", where);
        return adopted;
    } catch (const std::exception& error) {
        check_cancel(cancel);
        kLog.info("Remote engine reconnect failed: {}", error.what());
        return false;
    }
}

std::pair<Json, std::optional<double>>
RemoteSpeechBackend::send_window(std::span<const float> audio, const Json& fields,
                                 const CancelToken& cancel) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto pairing = pairing_provider_();
        std::shared_ptr<RemoteConnection> current;
        Json request = fields;
        {
            std::lock_guard lock(mutex_);
            if (!pairing || !pairing_ || *pairing_ != *pairing) {
                throw RemoteDecodeNotStarted(
                    "The paired host changed. Connect to the remote engine again.");
            }
            current = connection_;
            if (!hints_ && request.contains("phrases")) {
                request.erase("phrases"); // Capabilities can change when the host restarts.
            }
        }
        if (!current) {
            throw RemoteDecodeNotStarted(device_info());
        }
        try {
            return current->request_timed("transcribe", request, 300s, audio, {}, cancel);
        } catch (const RemoteConnectionLost& error) {
            // A connection that died while idle (the host restarted, the laptop slept) failed
            // before the host saw anything, so one reconnect and resend is safe. A request the
            // host may have started is not repeated.
            if (!error.sent && attempt == 0 && reconnect(current, cancel)) {
                continue;
            }
            drop(current, error.what());
            if (!error.sent) {
                throw RemoteDecodeNotStarted(error.what());
            }
            throw;
        } catch (const RemoteRequestError& error) {
            if (error.code == ENGINE_RELOADING) {
                drop(current, error.what());
                throw RemoteDecodeNotStarted(error.what());
            }
            throw;
        }
    }
    throw RemoteDecodeNotStarted(std::string(kNotConnected));
}

Transcript RemoteSpeechBackend::decode_window(std::span<const float> audio,
                                              const speech::DecodeJob& job,
                                              const CancelToken& cancel) {
    check_cancel(cancel);
    Json fields = Json::object();
    double busy_retry_s = BUSY_RETRY_S;
    double max_delay_s = BUSY_RETRY_MAX_DELAY_S;
    {
        std::lock_guard lock(mutex_);
        auto language = job.options.language;
        if (language.empty()) {
            language = field(member(runtime_, "selected"), "language");
        }
        fields["language"] = language.empty() ? Json() : Json(language);
        if (hints_ && !job.phrases.empty()) {
            fields["phrases"] = job.phrases;
        }
        busy_retry_s = busy_retry_s_;
        max_delay_s = busy_retry_max_delay_s_;
        ++inflight_;
    }
    struct Waiting {
        RemoteSpeechBackend& backend;
        ~Waiting() {
            {
                std::lock_guard lock(backend.mutex_);
                --backend.inflight_;
            }
            backend.link_changed.emit();
        }
    } waiting{*this};
    link_changed.emit();

    using Seconds = std::chrono::duration<double>;
    std::optional<std::chrono::steady_clock::time_point> busy_until;
    double delay = 1.0;
    while (true) {
        check_cancel(cancel);
        // Timed per attempt: a wait for room is not network time.
        const auto started = clock_.steady();
        try {
            auto result = send_window(audio, fields, cancel);
            const double round_trip = Seconds(clock_.steady() - started).count();
            {
                std::lock_guard lock(mutex_);
                auto& [requests, total, host_total, timed] = timing_;
                ++requests;
                total += round_trip;
                host_total += result.second.value_or(0.0);
                timed += result.second ? 1 : 0;
                ++replies_;
            }
            check_cancel(cancel);
            return transcript(result.first);
        } catch (const RemoteRequestError& error) {
            if (meeting_route_ && error.retryable()) {
                throw EngineError(error.what(), EngineError::Retry::Capacity, error.retry_after_s);
            }
            if (!error.retryable()) {
                throw;
            }
            // Busy with other computers' work: the host refused before starting this request,
            // so wait for room and send it again.
            // The wait below is real time, whatever clock times the requests.
            const auto now = std::chrono::steady_clock::now();
            if (!busy_until) {
                busy_until = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       Seconds(busy_retry_s));
                kLog.info("Remote host is busy; retrying for up to {:.0f} s", busy_retry_s);
            }
            if (now >= *busy_until) {
                throw EngineError(std::format("{} It stayed busy for {:.0f} seconds.", error.what(),
                                              busy_retry_s));
            }
            const double remaining = std::max(0.0, Seconds(*busy_until - now).count());
            const double wait = std::min(
                {std::max(error.retry_after_s.value_or(delay), delay), max_delay_s, remaining});
            // A Cancel ends the wait at once.
            cancel.wait_for(
                std::chrono::ceil<std::chrono::milliseconds>(std::chrono::duration<double>(wait)));
            delay = std::min(delay * 1.5, max_delay_s);
        }
    }
}

speech::DecodeResult RemoteSpeechBackend::decode(const speech::DecodeJob& job) {
    const auto* clip = std::get_if<speech::AudioClip>(&job.input);
    if (!clip || clip->rate != 16000 || !clip->samples || clip->end < clip->begin ||
        clip->end > clip->samples->size()) {
        throw EngineError("Remote speech requires a 16 kHz mono audio clip.");
    }
    check_cancel(job.cancel);
    std::optional<ClientPairing> pairing;
    try {
        pairing = pairing_provider_();
    } catch (const RemoteEngineError& error) {
        if (meeting_route_)
            throw EngineError(error.what(), EngineError::Retry::Connection);
        throw;
    }
    CancelSource active(job.cancel);
    {
        std::lock_guard lock(mutex_);
        if (!pairing || !pairing_ || *pairing_ != *pairing) {
            throw EngineError("The paired host changed. Connect to the remote engine again.",
                              meeting_route_ ? EngineError::Retry::Connection
                                             : EngineError::Retry::None);
        }
        if (!connection_ && !meeting_route_) {
            throw EngineError(error_.empty() ? std::string(kNotConnected) : error_);
        }
        active_ = active;
    }
    struct End {
        RemoteSpeechBackend& backend;
        ~End() {
            std::lock_guard lock(backend.mutex_);
            backend.active_.reset();
        }
    } end{*this};

    try {
        if (meeting_route_ && !link_alive()) {
            adopt(*pairing, open(*pairing, active.token()));
        }
        const auto started = clock_.steady();
        const auto audio = std::span(*clip->samples).subspan(clip->begin, clip->end - clip->begin);
        const auto cancel = active.token();
        speech::DecodeResult result;
        result.transcript = speech::transcribe_audio(
            audio,
            [&](std::span<const float> window) { return decode_window(window, job, cancel); },
            [&] { check_cancel(cancel); });
        check_cancel(cancel);
        result.decode_s = std::chrono::duration<double>(clock_.steady() - started).count();
        return result;
    } catch (const EngineError& error) {
        if (meeting_route_ && error.retry == EngineError::Retry::None && !link_alive() &&
            !active.token().cancelled()) {
            throw EngineError(error.what(), EngineError::Retry::Connection);
        }
        throw;
    } catch (const RemoteHostBusy& error) {
        check_cancel(active.token());
        if (meeting_route_)
            throw EngineError(error.what(), EngineError::Retry::Capacity, error.retry_after_s);
        throw EngineError(error.what());
    } catch (const RemoteDecodeNotStarted& error) {
        // The host never started it: a recording may reconnect and send it again
        // (transcription.py _decode_and_complete's RemoteDecodeNotStarted).
        check_cancel(active.token());
        throw EngineError(error.what(), EngineError::Retry::Connection);
    } catch (const RemoteEngineError& error) {
        check_cancel(active.token());
        if (meeting_route_)
            throw EngineError(error.what(), EngineError::Retry::Connection);
        throw EngineError(error.what());
    } catch (const std::exception& error) {
        check_cancel(active.token());
        throw EngineError(error.what());
    }
}

// ---- the link's own retries ----

void RemoteSpeechBackend::report(speech::ReadyState state) {
    if (!live_link_) {
        return;
    }
    const std::string info = device_info();
    live_link_->report(live_link_->generation.load(), state == speech::ReadyState::Ready
                                                          ? speech::Readiness{state, info, {}}
                                                          : speech::Readiness{state, info, info});
}

void RemoteSpeechBackend::link_lost() {
    // Closed between requests: check_link drops it, says so and schedules the retry.
    if (!check_link() && !is_available()) {
        schedule_retry();
    }
}

void RemoteSpeechBackend::schedule_retry() {
    if (!keeper_) {
        return;
    }
    bool unpaired = false;
    {
        std::lock_guard lock(mutex_);
        if (connection_ && !connection_->closed()) {
            return;
        }
        unpaired = unpaired_.value_or(false);
    }
    if (unpaired) {
        return; // nothing to connect to until this computer pairs
    }
    std::chrono::seconds delay{0};
    {
        std::lock_guard lock(keeper_->mutex);
        if (!keeper_->owner || keeper_->stopped || keeper_->restoring || keeper_->timer.active()) {
            return;
        }
        delay = REMOTE_RETRY_DELAYS[std::min(keeper_->step, REMOTE_RETRY_DELAYS.size() - 1)];
        ++keeper_->step;
        keeper_->retry_at = clock_.steady() + delay;
    }
    // Not under the lock: the executor is foreign code.
    auto timer = keeper_->executor.post_after(delay, [weak = std::weak_ptr(keeper_)] {
        Keeper::run(weak, [](auto& self) { self.run_retry(); });
    });
    {
        std::lock_guard lock(keeper_->mutex);
        keeper_->timer = std::move(timer);
    }
    link_changed.emit();
}

void RemoteSpeechBackend::cancel_retry() {
    if (!keeper_) {
        return;
    }
    std::lock_guard lock(keeper_->mutex);
    keeper_->timer.cancel();
    keeper_->retry_at.reset();
    keeper_->step = 0;
}

void RemoteSpeechBackend::retry_now() {
    link_handle().retry_now();
}

bool RemoteSpeechBackend::restoring() const {
    return link_handle().restoring();
}

RemoteSpeechBackend::LinkHandle RemoteSpeechBackend::link_handle() const {
    LinkHandle handle;
    handle.keeper_ = keeper_;
    return handle;
}

void RemoteSpeechBackend::LinkHandle::retry_now() const {
    const auto keeper = keeper_.lock();
    if (!keeper) {
        return;
    }
    {
        std::lock_guard lock(keeper->mutex);
        if (!keeper->owner) {
            return;
        }
    }
    keeper->executor.post(
        [weak = keeper_] { Keeper::run(weak, [](auto& self) { self.run_retry(); }); });
}

std::optional<RemoteLink> RemoteSpeechBackend::LinkHandle::link() const {
    const auto keeper = keeper_.lock();
    if (!keeper) {
        return std::nullopt;
    }
    std::shared_lock readers(keeper->readers);
    return keeper->readable ? std::optional<RemoteLink>(keeper->readable->link()) : std::nullopt;
}

std::optional<RemoteModels> RemoteSpeechBackend::LinkHandle::model_choices() const {
    const auto keeper = keeper_.lock();
    if (!keeper) {
        return std::nullopt;
    }
    std::shared_lock readers(keeper->readers);
    return keeper->readable ? std::optional<RemoteModels>(keeper->readable->model_choices())
                            : std::nullopt;
}

std::optional<RemoteSpeechBackend::TimingMark>
RemoteSpeechBackend::LinkHandle::timing_mark() const {
    const auto keeper = keeper_.lock();
    if (!keeper) {
        return std::nullopt;
    }
    std::shared_lock readers(keeper->readers);
    return keeper->readable ? std::optional<TimingMark>(keeper->readable->timing_mark())
                            : std::nullopt;
}

std::optional<RemoteTiming>
RemoteSpeechBackend::LinkHandle::timing_since(const TimingMark& mark) const {
    const auto keeper = keeper_.lock();
    if (!keeper) {
        return std::nullopt;
    }
    std::shared_lock readers(keeper->readers);
    return keeper->readable ? std::optional<RemoteTiming>(keeper->readable->timing_since(mark))
                            : std::nullopt;
}

bool RemoteSpeechBackend::LinkHandle::restoring() const {
    const auto keeper = keeper_.lock();
    if (!keeper) {
        return false;
    }
    std::lock_guard lock(keeper->mutex);
    return keeper->owner != nullptr && keeper->restoring;
}

std::optional<std::chrono::steady_clock::time_point> RemoteSpeechBackend::retry_at() const {
    if (!keeper_) {
        return std::nullopt;
    }
    std::lock_guard lock(keeper_->mutex);
    return keeper_->retry_at;
}

void RemoteSpeechBackend::run_retry() {
    {
        std::lock_guard lock(keeper_->mutex);
        if (keeper_->restoring || keeper_->stopped) {
            return;
        }
        keeper_->timer.cancel();
        keeper_->retry_at.reset();
    }
    if (is_available()) {
        cancel_retry();
        link_changed.emit();
        return;
    }
    {
        std::lock_guard lock(keeper_->mutex);
        keeper_->restoring = true;
    }
    link_changed.emit();
    std::string outcome = "offline";
    try {
        outcome = restore_link();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Remote engine reconnect failed");
    }
    settled(outcome);
}

void RemoteSpeechBackend::settled(const std::string& outcome) {
    {
        std::lock_guard lock(keeper_->mutex);
        keeper_->restoring = false;
    }
    if (is_available()) {
        // adopt() reported the engine; a reconnect needs nothing more.
        cancel_retry();
    } else {
        // Only when it reads differently, so a host that stays away does not clear the status
        // line every minute.
        report(speech::ReadyState::Offline);
        if (outcome != "unpaired") {
            schedule_retry();
        }
    }
    link_changed.emit();
}

} // namespace fv::remote
