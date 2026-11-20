#include "automation/confirmed_automation.h"
#include "support/action_support.h"
#include <catch2/catch_test_macros.hpp>

namespace {
class Delegate final : public fv::dictation::AutomationPort {
public:
    int filters = 0, routes = 0;
    std::unique_ptr<fv::dictation::FilterStageRun> filter_stage(std::string_view, const fv::Json&,
                                                                const fv::CancelToken&) override {
        ++filters;
        return {};
    }
    fv::dictation::RouteDecision route(std::string_view, const fv::Json&, std::string_view,
                                       const fv::CancelToken&, std::string_view) override {
        ++routes;
        return {};
    }
    std::optional<fv::dictation::RecordingAction> find_action(const fv::Json&,
                                                              std::string_view) override {
        return {};
    }
    bool program_words_allowed(const fv::dictation::RecordingAction&, const fv::Json&) override {
        return true;
    }
    bool accepting() override { return true; }
    void deliver(std::shared_ptr<const fv::dictation::DictationJob>,
                 fv::dictation::ActionDelivery) override {}
    std::vector<fv::dictation::ActionShortcut> action_hotkeys(const fv::Json&) override {
        return {};
    }
};
} // namespace
TEST_CASE("declined action and routing never invoke a filter or external destination",
          "[automation]") {
    using namespace fv;
    using namespace fv::automation;
    using namespace fv::automation::test;
    Harness harness;
    harness.add(an_action("a1", "Private service", ActionKind::WEBHOOK,
                          Json{{"url", "https://example.test/endpoint"}}));
    Delegate delegate;
    std::vector<ActionDisclosure> prompts;
    std::string refusal;
    ConfirmedAutomation port(
        delegate, *harness.rt, harness.settings,
        [&prompts](const auto& prompt, const auto&) {
            prompts.push_back(prompt);
            return false;
        },
        [](const Json&) { return std::string("Judge at https://example.test"); },
        [&refusal](std::string text) { refusal = std::move(text); });
    CHECK_FALSE(port.filter_stage("before_cleanup", Json::object(), {}));
    CHECK(delegate.filters == 0);
    port.deliver(action_job("a1"), {.text = "private words", .raw_text = "raw words"});
    REQUIRE(prompts.size() == 1);
    CHECK(prompts[0].destination.find("example.test") != std::string::npos);
    CHECK(prompts[0].material.find("private words") != std::string::npos);
    CHECK(prompts[0].material.find("raw words") != std::string::npos);
    CHECK(refusal == "Action canceled.");
    CHECK(harness.ran().empty());
    harness.set("voice_routing_enabled", true);
    const auto decision =
        port.route("route text", *harness.settings.snapshot(), "hotkey", {}, "job");
    CHECK(decision.failure == "consent_declined");
    CHECK(delegate.routes == 0);
    REQUIRE(prompts.size() == 2);
    CHECK(prompts.back().material.find("route text") != std::string::npos);
    (void)port.route("ordinary dictation", Json::object(), "auto", {}, "job");
    CHECK(delegate.routes == 0);
    CHECK(prompts.size() == 2);
}

TEST_CASE("accepted action uses the destination shown before settings changed", "[automation]") {
    using namespace fv;
    using namespace fv::automation;
    using namespace fv::automation::test;
    Harness harness;
    harness.add(an_action("a1", "First", ActionKind::OPEN_LINK,
                          Json{{"template", "https://first.test/?q={text}"}}));
    Delegate delegate;
    std::mutex mutex;
    std::string opened;
    harness.open_uri.set([&](const std::string& uri) {
        std::lock_guard lock(mutex);
        opened = uri;
    });
    ConfirmedAutomation port(
        delegate, *harness.rt, harness.settings,
        [&harness](const auto& prompt, const auto&) {
            CHECK(prompt.destination.find("first.test") != std::string::npos);
            harness.set("actions",
                        Json::array({an_action("a1", "Second", ActionKind::OPEN_LINK,
                                               Json{{"template", "https://second.test/?q={text}"}})
                                         .to_dict()}));
            return true;
        },
        [](const Json&) { return std::string("judge"); }, [](std::string) {});
    port.deliver(action_job("a1"), {.text = "hello"});
    (void)harness.finished();
    std::lock_guard lock(mutex);
    CHECK(opened == "https://first.test/?q=hello");
}

TEST_CASE("agent consent shows the selected context that the agent receives", "[automation]") {
    using namespace fv;
    using namespace fv::automation;
    using namespace fv::automation::test;
    Harness harness;
    harness.add(
        an_action("a1", "Explain", ActionKind::AGENT,
                  Json{{"agent", "claude_code"}, {"mode", "ask"}, {"include_selection", true}}));
    Delegate delegate;
    std::mutex mutex;
    std::string disclosed, sent;
    harness.ask.set(
        [&](const std::string&, const std::string& text, const std::string&, const CancelToken&) {
            std::lock_guard lock(mutex);
            sent = text;
            return asked(true, "ok");
        });
    ConfirmedAutomation port(
        delegate, *harness.rt, harness.settings,
        [&disclosed](const auto& prompt, const auto&) {
            disclosed = prompt.material;
            return true;
        },
        [](const Json&) { return std::string("judge"); }, [](std::string) {});
    auto job =
        action_job("a1", JobSpec{.text = selection("selected private text", true, false, "uia")});
    port.deliver(job, {.text = "explain this"});
    (void)harness.finished();
    std::lock_guard lock(mutex);
    CHECK(disclosed.find("selected private text") != std::string::npos);
    CHECK(sent == disclosed);
}

TEST_CASE("a canceled action cannot send even if its confirmation returns yes", "[automation]") {
    using namespace fv;
    using namespace fv::automation;
    using namespace fv::automation::test;
    Harness harness;
    harness.add(an_action("a1", "Service", ActionKind::WEBHOOK,
                          Json{{"url", "https://example.test/hook"}}));
    Delegate delegate;
    CancelSource cancel;
    ConfirmedAutomation port(
        delegate, *harness.rt, harness.settings,
        [&cancel](const auto&, const auto&) {
            cancel.cancel();
            return true;
        },
        [](const Json&) { return std::string("judge"); }, [](std::string) {});
    dictation::ActionDelivery delivery;
    delivery.text = "private";
    delivery.cancel = cancel.token();
    port.deliver(action_job("a1"), std::move(delivery));
    CHECK(harness.ran().empty());
}
