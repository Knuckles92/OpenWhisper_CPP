#include "agentapi/tailscale_probe.h"

#include <utility>

#include "remote/tailscale_cli.h"

namespace fv::agentapi {

namespace {

class CliProbe final : public TailscaleProbe {
public:
    explicit CliProbe(std::unique_ptr<remote::TailscaleCli> cli) : cli_(std::move(cli)) {}
    net::TailscaleStatus status() override { return cli_->status(); }

private:
    std::unique_ptr<remote::TailscaleCli> cli_;
};

} // namespace

std::unique_ptr<TailscaleProbe> make_tailscale_probe(const EnvReader& env) {
    return std::make_unique<CliProbe>(remote::make_tailscale_cli(env));
}

} // namespace fv::agentapi
