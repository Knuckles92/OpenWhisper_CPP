#pragma once

#include "appkit/feature.h"

namespace fv {

class MeetingFeature final : public Feature {
public:
    std::string_view id() const override { return "meeting"; }
    std::vector<std::string_view> required_features() const override {
        return {"controller", "history", "settings", "shell"};
    }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

} // namespace fv
