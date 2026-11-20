#pragma once

#include "appkit/feature.h"
#include "hotkeys/mouse_hook.h"
#include <functional>
#include <memory>

namespace fv {
namespace walkthrough {
class SnapPort;
}
class WalkthroughFeature final : public Feature {
public:
    WalkthroughFeature();
    // Auto Mode's click watcher: make_mouse_hook_source(on_click) unless a test gives another.
    using ClickHook =
        std::function<std::unique_ptr<hotkeys::MouseHookSource>(hotkeys::ClickObserver)>;
    // The injected port must outlive this feature. Empty uses the desktop Snap adapter.
    explicit WalkthroughFeature(walkthrough::SnapPort* snap, ClickHook clicks = {});
    ~WalkthroughFeature() override;
    std::string_view id() const override { return "walkthrough"; }
    std::vector<std::string_view> required_features() const override {
        return {"controller", "history", "shell"};
    }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;

private:
    struct State;
    std::unique_ptr<State> state_;
    walkthrough::SnapPort* snap_ = nullptr;
    ClickHook clicks_;
};
} // namespace fv
