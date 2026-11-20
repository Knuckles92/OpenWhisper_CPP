#pragma once

#include <string_view>
#include <vector>

#include "appkit/feature.h"

namespace fv {

// The Personalize chapter of Settings (Dictionary, Snippets, Apps & styles, Learned rules,
// Profiles, Commands and the AI cleanup page). Each page registers itself in install() through
// registerPersonalizePage; this foundation registers none yet, and the packages that port the
// pages add their registrations to personalize_feature.cpp.
// Affinity: UI actor.
class PersonalizeFeature final : public Feature {
public:
    static constexpr std::string_view kId = "personalize";

    std::string_view id() const override { return kId; }
    // The pages are built inside the Settings window, which the Settings feature provides.
    std::vector<std::string_view> required_features() const override { return {"settings"}; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

} // namespace fv
