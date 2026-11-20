#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "base/json.h"
#include "client_test_support.h"
#include "remote/client.h"
#include "remote/connection.h"
#include "testkit/testkit.h"

using namespace std::chrono_literals;
using namespace fv::remote;

TEST_CASE("live OpenWhisper host pairs and serves synthetic audio", "[platform][interop]") {
    const auto peer = fv::testkit::get_env("FIORIVOCE_LOOPBACK_INTEROP");
    if (!peer) {
        SKIP("Run tools/parity/run_remote_interop.py to start an isolated OpenWhisper host.");
    }
    const auto endpoint = fv::Json::parse(*peer);
    const int port = endpoint.at("port").get<int>();
    REQUIRE(port > 0);
    REQUIRE(port <= 65535);
    fv::remote::test::ClientRig rig;
    const auto probe = probe_host(rig.env, "127.0.0.1", port);
    REQUIRE(probe.host_name == "peer-host");
    REQUIRE(probe.fingerprint == endpoint.at("fingerprint").get<std::string>());
    const auto paired = pair_with_host(rig.env, "127.0.0.1", port,
                                       endpoint.at("code").get<std::string>(), "interop-client");
    REQUIRE(paired.fingerprint == probe.fingerprint);
    RemoteConnection connection(rig.env, "127.0.0.1", port, paired.token, probe.fingerprint);
    REQUIRE(connection.connect().at("type") == "ready");
    CHECK(connection.request("transcribe", fv::Json::object(), 5s, fv::remote::test::tone(1600))
              .at("text") == "heard 1600");
}
