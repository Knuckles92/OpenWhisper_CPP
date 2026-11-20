#pragma once

#include <string_view>

#include "appkit/feature.h"

namespace fv {

class BetaFeature final : public Feature {
public:
    std::string_view id() const override;
    void install(AppContext&) override {}
};

} // namespace fv
