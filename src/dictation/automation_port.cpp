#include "dictation/automation_port.h"

namespace fv::dictation {

namespace {

class NullAutomationPort final : public AutomationPort {
public:
    std::unique_ptr<FilterStageRun> filter_stage(std::string_view, const Json&,
                                                 const CancelToken&) override {
        return nullptr;
    }
    RouteDecision route(std::string_view, const Json&, std::string_view, const CancelToken&,
                        std::string_view) override {
        return {};
    }
    std::optional<RecordingAction> find_action(const Json&, std::string_view) override {
        return std::nullopt;
    }
    bool program_words_allowed(const RecordingAction&, const Json&) override { return false; }
    bool accepting() override { return false; }
    void deliver(std::shared_ptr<const DictationJob>, ActionDelivery) override {}
    std::vector<ActionShortcut> action_hotkeys(const Json&) override { return {}; }
};

} // namespace

AutomationPort& null_automation_port() {
    static NullAutomationPort port;
    return port;
}

} // namespace fv::dictation
