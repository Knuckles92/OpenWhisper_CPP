// What FioriVoce itself adds to a dictation, with no speech engine: OpenWhisper's
// scripts/bench_dictation_overhead.py, its three parts against the real code.
//
// stop    A 30 s recording's stream closed -> 16 kHz audio ready to decode: the recording
//         journal's last write (finish), the WAV save (copy_into and commit, as
//         Recorder::save_recording), and the conversion to 16 kHz windows (speech::file_windows,
//         what every engine that takes a file and the remote engine read). The writer idles 60 to
//         160 ms first, as through a post-roll. 25 runs.
// remote  Round trips between the real host (remote_host::SpeechHost over FakeHostEngine, which
//         answers at once) and the real client (remote::RemoteConnection), paired with a code,
//         over TLS on 127.0.0.1: a request without audio, and a transcribe of 10 s of audio.
//         50 runs after 20 warm-up requests.
// api     One upload by the API engine's upload clients (speech::UploadClients over the HTTP
//         upload client) of a 10 s WAV to a server on 127.0.0.1 that answers at once, with the
//         client started for the upload and readied ahead (warm(), what a recording's start
//         does). 8 runs each, as OpenWhisper caps them.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../http/loopback_server.h"
#include "../remote_host/host_rig.h"
#include "audio/wav_writer.h"
#include "base/executor.h"
#include "http/client.h"
#include "recorder/journal.h"
#include "remote/client.h"
#include "remote/connection.h"
#include "speech/cloud_backend.h"
#include "speech/file_windows.h"
#include "support/perf_support.h"
#include "testkit/testkit.h"

namespace {

using namespace std::chrono_literals;
using fv::Json;

constexpr int kRate = 44100;
constexpr int kBlockFrames = 1024;

std::vector<std::vector<std::byte>> noiseBlocks() {
    std::mt19937 random(0);
    std::normal_distribution<double> noise(0.0, 3000.0);
    std::vector<std::vector<std::byte>> blocks(64, std::vector<std::byte>(kBlockFrames * 2));
    for (auto& block : blocks) {
        for (int frame = 0; frame < kBlockFrames; ++frame) {
            const auto sample =
                static_cast<std::int16_t>(std::clamp(noise(random), -32768.0, 32767.0));
            const auto bits = static_cast<std::uint16_t>(sample);
            block[static_cast<std::size_t>(frame) * 2] = static_cast<std::byte>(bits & 0xff);
            block[static_cast<std::size_t>(frame) * 2 + 1] = static_cast<std::byte>(bits >> 8);
        }
    }
    return blocks;
}

std::vector<float> noiseSeconds(double seconds, unsigned seed) {
    std::mt19937 random(seed);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::vector<float> out(static_cast<std::size_t>(seconds * 16000));
    for (auto& sample : out) {
        sample = noise(random);
    }
    return out;
}

void writeSilentWav(const std::filesystem::path& path, double seconds) {
    fv::audio::WavWriter writer(path, {kRate, 1, 2});
    const std::vector<std::int16_t> silence(static_cast<std::size_t>(seconds * kRate), 0);
    writer.write(std::span<const std::int16_t>(silence));
    writer.commit(std::nullopt);
}

} // namespace

TEST_CASE("dictation overhead: stop to 16 kHz audio", "[perf]") {
    constexpr double kSeconds = 30.0;
    constexpr int kRuns = 25;
    fv::testkit::TempDataRoot root;
    const auto blocks = noiseBlocks();
    const int count = static_cast<int>(kSeconds * kRate / kBlockFrames);
    const std::filesystem::path output = root.path() / "recorded.wav";
    std::mt19937 random(1);
    std::uniform_int_distribution<int> postRoll(60, 160);

    std::vector<double> finish, save, convert, total;
    std::size_t windows = 0;
    for (int run = 0; run < kRuns; ++run) {
        fv::RecordingJournal journal(output, kRate, 1, 2, [](const std::string&) {});
        for (int index = 0; index < count; ++index) {
            REQUIRE(journal.append(blocks[static_cast<std::size_t>(index) % blocks.size()]));
            if (index % 256 == 255) {
                // Far faster than a microphone; the writer keeps up.
                REQUIRE(journal.wait_until_written(10s));
            }
        }
        REQUIRE(journal.wait_until_written(10s));
        std::this_thread::sleep_for(std::chrono::milliseconds(postRoll(random)));

        const auto started = fvperf::Clock::now();
        REQUIRE(journal.finish());
        const auto finished = fvperf::Clock::now();
        {
            fv::audio::WavWriter writer(output, {kRate, 1, 2});
            journal.copy_into(writer);
            writer.commit(std::nullopt);
        }
        const auto saved = fvperf::Clock::now();
        windows = fv::speech::file_windows(output).size();
        const auto converted = fvperf::Clock::now();
        const auto ms = [](auto from, auto to) {
            return std::chrono::duration<double, std::milli>(to - from).count();
        };
        finish.push_back(ms(started, finished));
        save.push_back(ms(finished, saved));
        convert.push_back(ms(saved, converted));
        total.push_back(ms(started, converted));
        journal.close(true);
        std::filesystem::remove(output);
    }
    CHECK(windows > 0);

    const std::string test = "perf.dictation overhead: stop to 16 kHz audio";
    const Json notes{{"recording_s", kSeconds}, {"runs", kRuns}};
    const auto recorded = fvperf::record("dictation_stop_overhead_ms", "ms", total, test, notes);
    CHECK(fvperf::underCeiling(recorded, recorded.p50));
    fvperf::record("dictation_stop_journal_ms", "ms", finish, test, notes);
    fvperf::record("dictation_stop_wav_save_ms", "ms", save, test, notes);
    fvperf::record("dictation_stop_convert_ms", "ms", convert, test, notes);
}

TEST_CASE("dictation overhead: remote round trip", "[perf]") {
    constexpr int kRuns = 50;
    fv::remote_host::rig::HostRig host;
    fv::net::Reactor reactor{2};
    fv::remote::ClientEnv env{.reactor = reactor,
                              .discovery = {},
                              .lan_cards = [] { return std::vector<fv::remote::LanCard>{}; },
                              .remember_addresses = {}};
    const auto paired = fv::remote::pair_with_host(env, "127.0.0.1", host.port,
                                                   host.host->open_pairing(), "bench-client");
    REQUIRE_FALSE(paired.token.empty());
    std::vector<double> describe, transcribe;
    {
        fv::remote::RemoteConnection connection(env, "127.0.0.1", host.port, paired.token,
                                                paired.fingerprint);
        REQUIRE(connection.connect().value("type", "") == "ready");
        const std::vector<float> recording = noiseSeconds(10.0, 0);
        for (int warm = 0; warm < 20; ++warm) {
            connection.request("describe");
        }
        for (int run = 0; run < kRuns; ++run) {
            const auto started = fvperf::Clock::now();
            connection.request("describe");
            describe.push_back(fvperf::msSince(started));
        }
        for (int run = 0; run < kRuns; ++run) {
            const auto started = fvperf::Clock::now();
            connection.request("transcribe", Json{{"language", "en"}}, 30s, recording);
            transcribe.push_back(fvperf::msSince(started));
        }
        connection.close();
    }
    fv::remote::RemoteConnection::clear_route_memory();

    const std::string test = "perf.dictation overhead: remote round trip";
    const auto recorded = fvperf::record("remote_round_trip_ms", "ms", transcribe, test,
                                         Json{{"audio_s", 10}, {"runs", kRuns}, {"tls", true}});
    CHECK(fvperf::underCeiling(recorded, recorded.p50));
    fvperf::record("remote_request_ms", "ms", describe, test, Json{{"op", "describe"}});
}

TEST_CASE("dictation overhead: API upload", "[perf]") {
    constexpr int kRuns = 8;
    fv::testkit::TempDataRoot root;
    const std::filesystem::path upload = root.path() / "upload.wav";
    writeSilentWav(upload, 10.0);

    fv::testing::LoopbackServer server(
        [](const fv::testing::LoopbackServer::Request&, fv::testing::LoopbackServer::Reply& reply) {
            reply.write(fv::testing::LoopbackServer::response(
                200, "OK", {{"Content-Type", "application/json"}}, R"({"text": "hello world"})"));
        });
    fv::http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    auto http = fv::http::make_curl_client(std::move(options));

    fv::speech::CloudConnection connection;
    connection.base_url = server.url("/v1");
    connection.key = "sk-benchmark";
    connection.timeout_s = 30.0;
    fv::ThreadPool pool("perf-uploads", 1);
    fv::Strand timers(pool, "timers", 1, fv::ShutdownMode::CancelQueued);
    fv::speech::UploadClients clients(
        connection,
        [&http](const fv::speech::CloudConnection& to) {
            return fv::speech::make_http_upload_client(*http, to, *to.base_url);
        },
        timers);
    fv::speech::UploadRequest request;
    request.audio = upload;
    request.model = "whisper-1";
    request.response_format = "json";

    std::vector<double> cold, ready;
    for (int run = 0; run < kRuns; ++run) {
        const auto started = fvperf::Clock::now();
        const std::string text = clients.create(request, {});
        cold.push_back(fvperf::msSince(started));
        REQUIRE(text.find("hello world") != std::string::npos);
    }
    for (int run = 0; run < kRuns; ++run) {
        clients.warm();
        const auto started = fvperf::Clock::now();
        clients.create(request, {});
        ready.push_back(fvperf::msSince(started));
    }
    clients.close();
    timers.shutdown(2s);

    const std::string test = "perf.dictation overhead: API upload";
    const Json notes{{"audio_s", 10}, {"runs", kRuns}, {"server", "127.0.0.1, answers at once"}};
    const auto recorded = fvperf::record("api_upload_ms", "ms", ready, test, notes);
    CHECK(fvperf::underCeiling(recorded, recorded.p50));
    fvperf::record("api_upload_cold_ms", "ms", cold, test, notes);
}
