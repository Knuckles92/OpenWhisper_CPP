#pragma once

// OpenWhisper's paired_backend fixture (tests/test_remote_engine.py): this computer paired by
// code with a HostRig's host, and RemoteSpeechBackends over that pairing.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "base/clock.h"
#include "host_rig.h"
#include "remote/client.h"
#include "remote/connection.h"
#include "remote/remote_backend.h"
#include "speech/service.h"

namespace fv::remote_host::rig {

using fv::remote::RemoteSpeechBackend;

// tests/test_remote_engine.py's paired_backend: this computer paired with the rig's host by
// code, and a RemoteSpeechBackend over that pairing.
struct PairedBackend {
    explicit PairedBackend(std::function<void(SpeechHostDeps&)> configure = {},
                           Executor* reconnect_on = nullptr, Clock& clock = system_clock())
        : host(std::move(configure)), env{host.reactor} {
        const auto result = remote::pair_with_host(env, "127.0.0.1", host.port,
                                                   host.host->open_pairing(), "laptop");
        token = result.token;
        remote::ClientPairing saved;
        saved.host = "127.0.0.1";
        saved.port = host.port;
        saved.fingerprint = result.fingerprint;
        saved.host_name = result.host_name;
        saved.device_id = result.device_id;
        saved.paired_at = "2026-01-01T00:00:00+00:00";
        pairing = saved;
        backend = make(reconnect_on, clock);
    }
    ~PairedBackend() {
        backend.reset();
        remote::RemoteConnection::clear_route_memory();
    }

    // A backend that watches its link (as the dictation slot's does) when it may reconnect.
    std::unique_ptr<RemoteSpeechBackend> make(Executor* reconnect_on = nullptr,
                                              Clock& clock = system_clock(),
                                              std::shared_ptr<Json> route = {}) {
        return std::make_unique<RemoteSpeechBackend>(
            [this] {
                std::lock_guard lock(mutex);
                return pairing;
            },
            [this, meeting = route != nullptr](const remote::ClientPairing& peer) {
                ++connections;
                return std::make_unique<remote::RemoteConnection>(env, peer.host, peer.port, token,
                                                                  peer.fingerprint, peer.alternates,
                                                                  std::nullopt, !meeting);
            },
            clock, std::move(route), reconnect_on != nullptr, reconnect_on);
    }

    void stop_host() { host.host->stop(); }
    void start_host() {
        REQUIRE(host.host->start(host.port, "127.0.0.1", std::nullopt).has_value());
    }
    void forget_pairing() {
        std::lock_guard lock(mutex);
        pairing.reset();
    }

    HostRig host;
    remote::ClientEnv env;
    std::string token;
    std::mutex mutex;
    std::optional<remote::ClientPairing> pairing;
    std::atomic<int> connections{0};
    std::unique_ptr<RemoteSpeechBackend> backend;
};

inline speech::DecodeJob job_of(std::vector<float> audio, std::string language = {},
                                std::vector<std::string> phrases = {}) {
    auto samples = std::make_shared<const std::vector<float>>(std::move(audio));
    speech::DecodeJob job;
    job.input = speech::AudioClip{samples, 0, samples->size(), 16000};
    job.options.language = std::move(language);
    job.phrases = std::move(phrases);
    return job;
}

} // namespace fv::remote_host::rig
