#pragma once

#include <string_view>
#include <vector>

#include "appkit/feature.h"

namespace fv {

class AlphaFeature final : public Feature {
public:
    std::string_view id() const override { return "alpha"; }
    std::vector<std::string_view> required_features() const override { return {"beta"}; }
    void install(AppContext&) override {}
};

// The bytes of data/greeting.json, through fv_embed.
std::string_view alpha_greeting();

} // namespace fv
