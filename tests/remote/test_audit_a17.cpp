// Audit of docs/port/map/a17-remote-engine-services.md: the client's timing constants, which the
// behaviour tests shorten or never reach (a 150 s approval wait, a 30 s search interval), checked
// against services/remote_asr/client.py and tailscale.py as written there.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "remote/client.h"
#include "remote/connection.h"
#include "remote/tailscale_cli.h"

using namespace std::chrono_literals;

TEST_CASE("a17 audit: the client waits as long as OpenWhisper's client does", "[audit]") {
    // A17-21: 6 s for the last address, 2.5 s for each one that has another after it, 10 s for
    // ready or paired; a missing host is searched for at most every 30 s.
    CHECK(fv::remote::CONNECT_TIMEOUT == 6000ms);
    CHECK(fv::remote::FALLBACK_CONNECT_TIMEOUT == 2500ms);
    CHECK(fv::remote::HANDSHAKE_TIMEOUT == 10s);
    CHECK(fv::remote::REDISCOVER_INTERVAL == 30s);
    // A17-14 and A17-36: a probe gives up after 2 s; a scan runs 12 at once.
    CHECK(fv::remote::PROBE_TIMEOUT == 2s);
    CHECK(fv::remote::PROBE_WORKERS == 12);
    // A17-23: a ping right away and every 20 s, the connection closed when a pong takes 20 s.
    CHECK(fv::remote::KEEPALIVE_INTERVAL == 20s);
    CHECK(fv::remote::KEEPALIVE_TIMEOUT == 20s);
    // A17-24: a waiting request looks for a cancel every 50 ms.
    CHECK(fv::remote::REQUEST_POLL == 50ms);
    // A17-11: an approval request waits 150 s for the host's owner ("The host didn't answer in
    // time. Try again."), looking for a cancel every 200 ms.
    CHECK(fv::remote::APPROVAL_WAIT == 150s);
    CHECK(fv::remote::APPROVAL_POLL == 200ms);
    // A17-25: a capture start waits 0.75 s for the connection's lock and the host's answer.
    CHECK(fv::remote::RECORDING_WAIT == 750ms);
    // A17-38: the Tailscale CLI gets 4 s.
    CHECK(fv::remote::TAILSCALE_TIMEOUT_S == 4.0);
}
