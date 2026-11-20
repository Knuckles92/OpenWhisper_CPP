#include "meeting/intelligence.h"

namespace fv::meeting {

namespace {

Json unavailable_outcome() {
    Json outcome = Json::object();
    outcome["status"] = "failed";
    outcome["message"] = std::string(kIntelligenceUnavailableMessage);
    return outcome;
}

class NullIntelligence final : public IntelligenceProvider {
public:
    bool available() const override { return false; }
    Json polish(const RerunContext&, const CancelToken&) override { return unavailable_outcome(); }
    Json consolidate(const RerunContext&, const CancelToken&) override {
        return unavailable_outcome();
    }
};

} // namespace

std::shared_ptr<IntelligenceProvider> null_intelligence() {
    static const std::shared_ptr<IntelligenceProvider> provider =
        std::make_shared<NullIntelligence>();
    return provider;
}

} // namespace fv::meeting
