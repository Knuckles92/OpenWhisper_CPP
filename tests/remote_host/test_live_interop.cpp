#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "base/json.h"
#include "base/utf8.h"
#include "host_rig.h"
#include "testkit/testkit.h"

using namespace std::chrono_literals;

TEST_CASE("live OpenWhisper client pairs and sends synthetic audio", "[platform][interop]") {
    const auto scratch = fv::testkit::get_env("FIORIVOCE_LOOPBACK_INTEROP_DIR");
    if (!scratch) {
        SKIP("Run tools/parity/run_remote_interop.py to start an isolated OpenWhisper client.");
    }
    const auto folder = fv::base_utf8::path_from_utf8(*scratch);
    const auto staging = folder / "endpoint.tmp";
    const auto endpoint = folder / "endpoint.json";
    fv::remote_host::rig::HostRig rig;
    {
        std::ofstream ready(staging);
        REQUIRE(ready);
        ready << fv::Json{{"port", rig.port}, {"code", rig.host->open_pairing()}}.dump();
        ready.close();
        REQUIRE(ready);
    }
    std::filesystem::rename(staging, endpoint);
    const auto deadline = std::chrono::steady_clock::now() + 35s;
    const auto acknowledged = folder / "confirmed";
    while (!std::filesystem::exists(acknowledged) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(25ms);
    }
    REQUIRE(std::filesystem::exists(acknowledged));
    const auto calls = rig.engine->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls.front().audio.size() == 1600);
}
