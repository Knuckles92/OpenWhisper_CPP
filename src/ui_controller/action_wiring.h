#pragma once
#include "automation/confirmed_automation.h"
#include <functional>
#include <memory>

namespace fv {
class AppContext;
class ApplicationController;
class UiController;
class ClipboardService;
// Tests inject this; production asks the user on the GUI thread for each invocation.
struct ActionConsent {
    automation::ConfirmedAutomation::Confirm confirm;
};
class ActionWiring {
public:
    static std::shared_ptr<ActionWiring> make(AppContext&, ApplicationController&, UiController&,
                                              ClipboardService&);
    ~ActionWiring();
    void close();
    ActionWiring(const ActionWiring&) = delete;
    ActionWiring& operator=(const ActionWiring&) = delete;

private:
    struct Impl;
    explicit ActionWiring(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace fv
