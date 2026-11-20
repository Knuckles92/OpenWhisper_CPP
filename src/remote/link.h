#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"

// What the engine card and the Model field show about the paired computer
// (transcriber/remote_backend.py's RemoteLink, RemoteTiming, HostModel, RemoteModels and
// parse_host_models). Plain values; thread-safe to copy.
namespace fv::remote {

// The backend key, and backend_id before the host has told its engine.
inline constexpr std::string_view REMOTE_BACKEND = "remote";

// The connection to the paired computer, as the engine card shows it.
struct RemoteLink {
    // "unpaired", "connecting", "connected" or "offline".
    std::string state = "unpaired";
    std::string host;
    // "Tailscale" or "local network" while connected.
    std::string route;
    std::optional<double> latency_ms;
    // A request is waiting on the host.
    bool busy = false;
    // Replies received so far; each new one is a packet coming home.
    std::int64_t replies = 0;
    // Keepalive pongs so far on this connection; each new one is a heartbeat the card pulses on.
    std::int64_t beat = 0;
    // Why it's offline, as the backend last said.
    std::string detail;
    // When the controller retries on its own.
    std::optional<std::chrono::steady_clock::time_point> retry_at;
    // What the host runs and on what, for the tooltip.
    std::string engine_label;
    std::string device;
    std::string address;
    std::string compute_type;
    std::string runtime_status;
    std::string gpu_name;
    std::int64_t gpu_memory_mib = 0;

    friend bool operator==(const RemoteLink&, const RemoteLink&) = default;
};

// Where one transcription's time went, from the requests it made.
struct RemoteTiming {
    std::string host;
    std::int64_t requests = 0;
    // Sending each window and waiting for its reply, summed, in seconds.
    double round_trip_s = 0.0;
    // The part the host says it spent, in seconds; nullopt from a host too old to say.
    std::optional<double> host_s;

    // max(0, round_trip_s - host_s), or nullopt without host_s.
    std::optional<double> network_s() const;
    friend bool operator==(const RemoteTiming&, const RemoteTiming&) = default;
};

// One model the paired computer can run, as its `ready` lists it.
struct HostModel {
    std::string family;
    std::string model;
    std::string label;

    std::pair<std::string, std::string> key() const { return {family, model}; }
    friend bool operator==(const HostModel&, const HostModel&) = default;
};

// What the Remote backend's Model field shows.
struct RemoteModels {
    // The paired computer's name; empty when this computer isn't paired.
    std::string host;
    // What it can switch to, or nullopt until it says (not reached yet, or a version that
    // doesn't list them).
    std::optional<std::vector<HostModel>> models;
    // What it serves this computer now, while connected.
    std::optional<HostModel> current;
    std::optional<Json> runtime;
    std::optional<Json> engine;

    friend bool operator==(const RemoteModels&, const RemoteModels&) = default;
};

// ready["models"] as HostModels: entries that are objects with non-empty string family and model,
// labelled str(label or model); nullopt for anything but a list (a host too old to list them).
std::optional<std::vector<HostModel>> parse_host_models(const Json& raw);

} // namespace fv::remote
