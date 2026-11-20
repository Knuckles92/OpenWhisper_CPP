#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "base/clock.h"
#include "base/subscription.h"
#include "remote/connection.h"
#include "remote/link.h"
#include "remote/pairing_settings.h"
#include "speech/service.h"

namespace fv::remote {

// Seconds before each automatic reconnect to an unreachable host; the last repeats until it
// answers (services/runtime/remote.py REMOTE_RETRY_DELAYS_S).
inline constexpr std::array<std::chrono::seconds, 5> REMOTE_RETRY_DELAYS{
    std::chrono::seconds(2), std::chrono::seconds(5), std::chrono::seconds(15),
    std::chrono::seconds(30), std::chrono::seconds(60)};
// How long a request the host refused as busy keeps being retried, and the longest pause
// between two tries whatever the host suggests, in seconds (transcriber/remote_backend.py
// BUSY_RETRY_S and BUSY_RETRY_MAX_DELAY_S).
inline constexpr double BUSY_RETRY_S = 90.0;
inline constexpr double BUSY_RETRY_MAX_DELAY_S = 5.0;
// Waiting for the host to load a model this computer chose. Longer than the host's own limit,
// so its reason arrives.
inline constexpr std::chrono::seconds SWITCH_REQUEST_TIMEOUT{300};

// Why the paired host cannot take a runtime change for (family, model) now, as the sentence to
// show, or "" when it can: it serves another model now (or none), or does not take runtime
// changes from here (RemoteSpeechBackend.request_runtime's ValueErrors). `state` is
// model_choices().
std::string runtime_request_refusal(const RemoteModels& state, std::string_view family,
                                    std::string_view model);

// What choosing one of the paired computer's models does (RemoteRuntime.select_remote_model).
struct RemoteModelSwitch {
    // Nothing to do: Remote isn't the engine, or the host already runs that model.
    bool skip = false;
    // Why not now ("End the meeting before changing the engine", "Finish recording before
    // changing the engine"): shown on the status line, and the Model field shows the host's
    // model again. Empty when the switch goes ahead.
    std::string refusal;
    // The reload's note while the host switches: "Switching <host> to <label>...".
    std::string note;
};

// Decides a Model field choice: skipped for the model the host serves now, refused during a
// meeting, a recording or a transcription, else the note of the reload that switches the host.
// `state` is model_choices(); the caller then calls request_model and reloads with the note.
RemoteModelSwitch plan_remote_model_switch(const RemoteModels& state, std::string_view family,
                                           std::string_view model, bool meeting_active,
                                           bool recording_or_transcribing);

// The runtime fields' counterpart (RemoteRuntime.select_remote_runtime): refused while anything
// runs or a reload is due, then runtime_request_refusal; the note is "Updating the runtime on
// <host>...".
RemoteModelSwitch plan_remote_runtime_change(const RemoteModels& state, std::string_view family,
                                             std::string_view model, bool busy);

// The paired host's window decoder (transcriber/remote_backend.py RemoteSpeechBackend). Load,
// decode and release run on the speech lane; everything else can be read or requested from
// another thread while they run.
//
// id() stays "remote", the engine the settings select; family() is what OpenWhisper's backend_id
// becomes after connecting (the host's engine family), which decides the preview.
//
// With `reconnect_on` (and `watch_live_link`) the backend keeps its own link: a connection that
// closes between requests is reported at once and tried again after REMOTE_RETRY_DELAYS, on that
// executor, without a reload (services/runtime/remote.py's watch, _schedule_retry and
// _on_settled). Each change reaches the SpeechService through watch_readiness.
class RemoteSpeechBackend final : public speech::Backend {
    struct LiveLinkWatch;
    struct Keeper;

public:
    using PairingProvider = std::function<std::optional<ClientPairing>()>;
    using ConnectionFactory =
        std::function<std::unique_ptr<RemoteConnection>(const ClientPairing&)>;
    // requests, round trip (s), host time (s), requests the host timed.
    using TimingMark = std::tuple<std::int64_t, double, double, std::int64_t>;

    RemoteSpeechBackend(PairingProvider pairing, ConnectionFactory connect, Clock& clock,
                        std::shared_ptr<Json> meeting_route = {}, bool watch_live_link = false,
                        Executor* reconnect_on = nullptr);
    // Stops the link's retries and waits for one that is running.
    ~RemoteSpeechBackend() override;

    std::string_view id() const override { return REMOTE_BACKEND; }
    speech::Capabilities capabilities() const override;
    // reload_model: connects, switches the host to a requested model first, and adopts what
    // the host then serves. Ready's message is why the host kept its model, when it did.
    speech::Readiness load(const CancelToken& superseded) override;
    speech::DecodeResult decode(const speech::DecodeJob& job) override;
    void release() override;
    Subscription watch_readiness(Executor&, std::function<void(speech::Readiness)>) override;
    bool supports_recognition() const override;
    std::string recognition_support() const override;

    // ---- the link, for the engine card ----
    // A snapshot of the connection. Cheap once a load or restore has looked for the pairing;
    // before that the first call asks the pairing provider (settings), so not on the GUI thread.
    RemoteLink link() const;
    // What the Remote backend's Model field shows.
    RemoteModels model_choices() const;
    // Connected to a host whose engine is loaded (is_available).
    bool is_available() const;
    // "<label> on <host>" once the host said what it runs, else "Remote engine".
    std::string name() const;
    // "<name> | <device>" ("<device> (<compute_type>)" when the host says both) while
    // connected, else the last error or "Remote engine is not connected".
    std::string device_info() const;
    std::string host_name() const;
    // The host's engine family (OpenWhisper's backend_id once connected); "" before.
    std::string family() const;
    // The host's model key; "" before it said.
    std::string model_name() const;
    // Why this computer is not connected, as the backend last said; "" while it is.
    std::string last_error() const;
    // Why the host kept its model when the last load asked it to switch; "" otherwise.
    std::string switch_error() const;

    // Notices a connection that closed between requests: true when it just did. Only reads the
    // socket's state.
    bool check_link();
    // One quiet attempt to reconnect, off the GUI thread: "connected" when the connection is up
    // again (or never went down) with the engine it had, "changed" when the host came back
    // running something else and this backend adopted it, "offline" when the host still can't
    // be reached or has nothing loaded, "unpaired" when there is nothing to connect to.
    std::string restore_link();
    // Reconnects now rather than at the next scheduled retry (RemoteRuntime.retry_now); a
    // no-op without `reconnect_on`, while connected or while a reconnect runs. Any thread.
    void retry_now();
    // A background reconnect is running.
    bool restoring() const;
    // What the app keeps of a dictation engine the SpeechService owns (RemoteRuntime.retry_now
    // and .restoring for the readiness check): safe from any thread, a no-op (false) once the
    // backend is gone. Never waits for a reconnect that is running.
    class LinkHandle {
    public:
        void retry_now() const;
        bool restoring() const;
        // link() and model_choices() while the backend lives; nullopt after.
        std::optional<RemoteLink> link() const;
        std::optional<RemoteModels> model_choices() const;
        // timing_mark() and timing_since() for the stats line (transcription.py _timed_decode);
        // nullopt once the backend is gone.
        std::optional<TimingMark> timing_mark() const;
        std::optional<RemoteTiming> timing_since(const TimingMark& mark) const;

    private:
        friend class RemoteSpeechBackend;
        std::weak_ptr<Keeper> keeper_;
    };
    LinkHandle link_handle() const;
    // When the next automatic retry is due; nullopt when none is scheduled.
    std::optional<std::chrono::steady_clock::time_point> retry_at() const;

    // Has the next load switch the host to `model`; returns the choice as the host lists it.
    // It rides on one load only. With `runtime` the load asks the host to apply those runtime
    // settings to it (configure_runtime) instead, unchecked: a fresh backend carrying the choice
    // of the one it replaces has no state to check against. Without, it drops a runtime change
    // requested before it.
    HostModel request_model(std::string_view family, std::string_view model,
                            std::optional<Json> runtime = std::nullopt);
    // Has the next load ask the host to apply `changes` (runtime settings such as {"device":
    // "cpu"}) to the model it serves (RemoteSpeechBackend.request_runtime). Throws
    // std::invalid_argument with the sentence to show when the host now serves another model,
    // or when it does not take runtime changes from here.
    void request_runtime(std::string_view family, std::string_view model, const Json& changes);
    // The language the host's runtime is set to, for a request made without one; "" when the
    // host reports none (the caller then uses the dictation language).
    std::string request_language() const;
    // Takes the runtime choices and the selectable models from the host's model catalog
    // (refresh_host_catalog), without touching the connection: false, and nothing changed, when
    // `pairing` is not the adopted host, no connection is up, or the catalog's engine differs
    // from the one adopted (a reconnect adopts that).
    bool refresh_host_catalog(const ClientPairing& pairing, const Json& ready, const Json& catalog);

    // ---- timing, for the stats line ----
    TimingMark timing_mark() const;
    // The requests made since timing_mark() returned `mark`; host_s only when the host timed
    // every one of them.
    RemoteTiming timing_since(const TimingMark& mark) const;

    // Stops waiting for the host and keeps the connection: the host finishes the request it
    // has either way.
    void cancel_transcription();
    void host_renamed(const ClientPairing& pairing);
    // Keeps a subscription for as long as this backend lives (the service's "host_renamed"
    // forwarding); it ends before anything else here is destroyed.
    void keep(Subscription subscription);
    // How long a busy host is waited for, and the longest pause between tries (tests).
    void set_busy_retry(double total_s, double max_delay_s);
    Signal<> link_changed;

private:
    struct Opened {
        std::shared_ptr<RemoteConnection> connection;
        Json ready;
    };

    Opened open(const ClientPairing& pairing, const CancelToken& cancel,
                std::optional<std::uint64_t> generation = {});
    Opened switch_host(Opened opened, const ClientPairing& pairing, const HostModel& requested,
                       const CancelToken& cancel, std::uint64_t generation,
                       const std::optional<Json>& runtime = std::nullopt);
    // _adopt_runtime: the ready's runtime with can_configure. Caller holds the mutex.
    void adopt_runtime(const Json& ready);
    // `announce` false while a load runs: the load's own answer is the report.
    bool adopt(const ClientPairing& pairing, Opened opened, bool announce = true);
    void drop(const std::shared_ptr<RemoteConnection>& connection, std::string reason);
    // cleanup(): a new generation, the connection and one still handshaking closed.
    void close_link();
    void set_connecting(bool connecting);
    void fail_load(std::uint64_t generation, const std::optional<ClientPairing>& pairing,
                   std::string message, bool forget_models);
    bool reconnect(const std::shared_ptr<RemoteConnection>& stale, const CancelToken& cancel);
    // check_link(), then whether a connection is still held.
    bool link_alive();
    Transcript decode_window(std::span<const float> audio, const speech::DecodeJob& job,
                             const CancelToken& cancel);
    std::pair<Json, std::optional<double>>
    send_window(std::span<const float> audio, const Json& fields, const CancelToken& cancel);

    // The link's own retries (a no-op without the keeper).
    void link_lost();
    void schedule_retry();
    void cancel_retry();
    void run_retry();
    void settled(const std::string& outcome);
    void report(speech::ReadyState state);

    PairingProvider pairing_provider_;
    ConnectionFactory connection_factory_;
    Clock& clock_;
    // Only the speech lane mutates the route. Its consumer reads after load has settled.
    std::shared_ptr<Json> meeting_route_;
    std::shared_ptr<LiveLinkWatch> live_link_;
    std::shared_ptr<Keeper> keeper_;

    mutable std::mutex mutex_;
    // The pairing the last load or restore looked up, which every decode checks again.
    std::optional<ClientPairing> pairing_;
    // The pairing whose host this backend last adopted (OpenWhisper's _pairing).
    std::optional<ClientPairing> adopted_;
    std::shared_ptr<RemoteConnection> connection_;
    // A connection still handshaking or switching, closed by release().
    std::shared_ptr<RemoteConnection> pending_;
    std::optional<CancelSource> pending_cancel_;
    std::optional<CancelSource> active_;
    Json engine_ = Json::object();
    Json runtime_ = Json::object();
    std::optional<std::vector<HostModel>> models_;
    std::optional<HostModel> requested_;
    std::optional<Json> requested_runtime_;
    mutable std::string host_name_;
    std::string error_;
    std::string switch_error_;
    // nullopt until a load or restore has looked for a pairing.
    mutable std::optional<bool> unpaired_;
    std::uint64_t generation_ = 0;
    int connecting_ = 0;
    int inflight_ = 0;
    bool hints_ = false;
    std::int64_t replies_ = 0;
    TimingMark timing_{0, 0.0, 0.0, 0};
    double busy_retry_s_ = BUSY_RETRY_S;
    double busy_retry_max_delay_s_ = BUSY_RETRY_MAX_DELAY_S;
    // Last: disconnected first, so nothing it runs reaches a backend being destroyed.
    std::vector<Subscription> kept_;
};

} // namespace fv::remote
