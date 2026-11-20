#pragma once

#include <memory>

#include "base/env.h"
#include "net/tailscale.h"

// What the MCP runtime asks of Tailscale before it listens on this computer's Tailscale address
// (services/remote_asr/tailscale.py status, through the CLI's `status --json`).
namespace fv::agentapi {

// Affinity: thread-safe; blocking (the CLI may take up to 4 s), never on the GUI thread.
class TailscaleProbe {
public:
    virtual ~TailscaleProbe() = default;
    // Never throws: "not_installed" without a CLI, "unavailable" when it fails.
    virtual net::TailscaleStatus status() = 0;
};

// The `tailscale` CLI found through `env` (remote::ProcessTailscaleCli: PATH, then where the
// installers put it), run without a window for at most 4 s. `env` must outlive the probe.
std::unique_ptr<TailscaleProbe> make_tailscale_probe(const EnvReader& env = system_env());

} // namespace fv::agentapi
