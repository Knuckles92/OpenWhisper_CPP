#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/executor.h"
#include "base/json.h"
#include "meeting_state/repository.h"
#include "meeting_state/types.h"
#include "meeting_web/dashboard_engine.h"
#include "net/http_server.h"
#include "net/reactor.h"

namespace fv::meeting_web {

namespace detail {
class HubCore;
} // namespace detail

// meeting/web/ws.py WsHub: every dashboard WebSocket of one web server, from admission to
// disconnect, and the fan-out of broadcasts to them.
//
// A connecting socket is admitted (token, guest name, at most kMaxConnections), registered,
// identified (the host's "me" participant, or a guest joined through the engine), sent its hello
// snapshot, and only then goes live; broadcasts raised in between are buffered for it (up to
// kMaxPendingMessages, then it is closed 4409 to resync) and flushed right after the hello, so
// no change falls between the snapshot and the first live patch. Its messages are handled one at
// a time, in arrival order, on the hub's worker threads, which are also where every blocking
// engine, store and repository call runs; the reactor never blocks.
//
// Applied store batches reach clients through the hub's subscription to the engine's store, so
// an action is answered only with its action_result, which the acting socket receives after the
// patch the action caused (as OpenWhisper's recorded transcript has it). Broadcasts go out in the
// order they are made; the store makes its patches in seq order.
//
// The engine must outlive the hub. Affinity: thread-safe; the destructor waits for running
// worker tasks and is not called from them or from the reactor.
class WsHub {
public:
    WsHub(net::Reactor& reactor, DashboardEngine& engine,
          std::shared_ptr<meeting_state::MeetingRepository> repository);
    ~WsHub();
    WsHub(const WsHub&) = delete;
    WsHub& operator=(const WsHub&) = delete;

    // The /ws route: messages up to uvicorn's 16 MiB, 32 queued (websockets' max_queue).
    net::WsHandlers handlers();

    // Lifespan startup: broadcasts and invalidation work from here on, and the hub subscribes
    // to the engine's store when it has one. Shutdown: the subscription ends and later
    // broadcasts are dropped.
    void on_startup();
    void on_shutdown();

    // Queues `message` for every live socket (host sockets only with host_only). Host-only
    // messages are not buffered for sockets still in their hello. Any thread; nothing happens
    // outside startup and shutdown.
    void schedule_broadcast(const Json& message, bool host_only = false);
    // Closes every socket: 4401 after kRotationCloseDelay (links rotated), or 4409 at once (a
    // resync that keeps the token valid). The registry is emptied before the sockets close.
    void schedule_invalidate_connections(bool resync = false);
    // The store subscriber: split_store_batch's patches, broadcast.
    void on_store_batch(std::int64_t seq, const std::vector<meeting_state::OpResult>& applied);

    // The absolute guest link a hello carries (MeetingWebServer::guest_url); without one, or
    // when it is empty, the hello carries "/m/<guest token>".
    void set_guest_url_provider(std::function<std::string()> provider);
    // The voice command guide hellos carry; voice_command_guide over empty settings until set.
    void set_voice_commands(std::function<Json()> provider);
    Json voice_commands() const;

    // Sockets registered now, live or still in their hello.
    std::size_t connection_count() const;

private:
    std::unique_ptr<ThreadPool> pool_;
    std::shared_ptr<detail::HubCore> core_;
};

} // namespace fv::meeting_web
