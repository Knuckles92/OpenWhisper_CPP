#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "appkit/feature.h"

class QWidget;

namespace fv {

class AppContext;
class HostDashboard;
struct RemoteEnginePageServices;

// The remote engine's Settings page (Settings → Remote engine) under destinations::remote_engine
// with its rail value. The page looks up remote::RemoteEngineControl and
// remote::RecordSyncControl when it is built; until something provides them it is the unbound
// page. Affinity: UI actor.
class RemoteFeature final : public Feature {
public:
    static constexpr std::string_view kId = "remote";

    std::string_view id() const override { return kId; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

// Registers the page; `customize` adjusts what each built page gets (tests' prompts and URL
// opener). Called by RemoteFeature::install.
void registerRemoteEnginePage(AppContext& context,
                              std::function<void(RemoteEnginePageServices&)> customize = {});

// The Host Mode dashboard the main window shows (UIController.remote_engine's bind): over the
// context's RemoteEngineControl (the unbound page without one) and its MCP control, with the
// picker switching this computer's engine through select_host_model, the controller's engine
// busy state, shared history and Settings links. Called by the main window's factory.
HostDashboard* buildHostDashboard(AppContext& context, QWidget* parent = nullptr);

} // namespace fv
