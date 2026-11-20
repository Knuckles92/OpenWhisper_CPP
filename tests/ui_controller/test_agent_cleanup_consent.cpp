#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "appkit/app_context.h"
#include "clipboard/testing/fake_backend.h"
#include "history/manager.h"
#include "llm/transcript_cleanup.h"
#include "personalize_support.h"

using namespace std::chrono_literals;

namespace {
struct FakeAgent {
    std::mutex mutex;
    std::vector<fv::llm::CleanupAgentRequest> calls;
};
struct ConsentCase {
    bool enabled;
    const char* consent;
    bool called;
};
} // namespace

TEST_CASE("ordinary dictation sends only the transcript to an explicitly authorized agent",
          "[widget]") {
    const auto test =
        GENERATE(ConsentCase{true, "codex", true}, ConsentCase{false, "codex", false},
                 ConsentCase{true, "claude_code", false}, ConsentCase{true, nullptr, false});
    fvtest::MemoryKeys keys;
    fv::Json settings{
        {"transcript_cleanup_enabled", test.enabled},
        {"transcript_cleanup_provider", "codex"},
        {"transcript_cleanup_model", "agent-test-model"},
        {"transcript_cleanup_level", "medium"},
        {"app_context_enabled", true},
        {"app_context_read_text", true},
        {"transcript_cleanup_rules", fv::Json::array({"PRIVATE_RULE_MARKER"})},
        {"dictation_dictionary", fv::Json::array({{{"id", "private"},
                                                   {"term", "PRIVATE_DICTIONARY_MARKER"},
                                                   {"heard", fv::Json::array({"never-dictated"})},
                                                   {"starred", false},
                                                   {"learned", false}}})}};
    if (test.consent != nullptr)
        settings["fv_cleanup_agent_consent"] = test.consent;
    auto fake = std::make_shared<FakeAgent>();
    auto options = fvtest::personalizeOptions(keys, nullptr, settings);
    options.script.texts = {"um tomorrow at ten"};
    options.beforeFeatures = [fake](fv::AppContext& context) {
        auto hooks = std::make_shared<fv::llm::CleanupOptions>();
        hooks->agent_available = [](std::string_view) {
            return true;
        };
        hooks->run_agent = [fake](const fv::llm::CleanupAgentRequest& request,
                                  const fv::CancelToken&) -> std::optional<std::string> {
            std::lock_guard lock(fake->mutex);
            fake->calls.push_back(request);
            return "Tomorrow at 10.";
        };
        context.provide<fv::llm::CleanupOptions>(hooks);
    };
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    std::atomic_bool completed = false;
    auto completion = rig.runtime().transcription_completed.connect(
        rig.uiExecutor(),
        [&completed](const auto&, const auto&, const auto&) { completed = true; });
    REQUIRE(rig.app().start_recording("ui"));
    REQUIRE(fvtest::waitUntil([&] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    rig.app().stop_recording();
    REQUIRE(fvtest::waitUntil([&] { return completed.load() && !rig.runtime().has_active_job(); },
                              30s));

    std::vector<fv::llm::CleanupAgentRequest> calls;
    {
        std::lock_guard lock(fake->mutex);
        calls = fake->calls;
    }
    CHECK(calls.size() == (test.called ? 1 : 0));
    if (test.called && !calls.empty()) {
        CHECK(calls[0].provider == "codex");
        CHECK(calls[0].text == "um tomorrow at ten");
        CHECK(calls[0].system_prompt.find("PRIVATE_RULE_MARKER") == std::string::npos);
        CHECK(calls[0].system_prompt.find("PRIVATE_DICTIONARY_MARKER") == std::string::npos);
    }
    auto& history = rig.context().service<fv::HistoryManager>();
    const auto entries = fvtest::offGui([&history] { return history.get_history(); });
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == (test.called ? "Tomorrow at 10." : "um tomorrow at ten"));
    if (test.called) {
        CHECK(entries[0].raw_text == "um tomorrow at ten");
        CHECK(entries[0].cleanup_provider == "codex");
        CHECK(entries[0].cleanup_model == "agent-test-model");
    } else {
        CHECK_FALSE(entries[0].raw_text);
        CHECK_FALSE(entries[0].cleanup_provider);
    }
    CHECK(keys.offline.requests().empty());
}

TEST_CASE("revoking agent cleanup during a run cancels it and preserves the transcript",
          "[widget]") {
    fvtest::MemoryKeys keys;
    fv::Json settings{{"transcript_cleanup_enabled", true},
                      {"transcript_cleanup_provider", "codex"},
                      {"transcript_cleanup_model", "agent-test-model"},
                      {"fv_cleanup_agent_consent", "codex"}};
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto options = fvtest::personalizeOptions(keys, nullptr, settings);
    options.script.texts = {"um tomorrow at ten"};
    options.beforeFeatures = [entered, cancelled](fv::AppContext& context) {
        auto hooks = std::make_shared<fv::llm::CleanupOptions>();
        hooks->agent_available = [](std::string_view) {
            return true;
        };
        hooks->run_agent = [entered,
                            cancelled](const fv::llm::CleanupAgentRequest&,
                                       const fv::CancelToken& token) -> std::optional<std::string> {
            *entered = true;
            *cancelled = token.wait_for(5s);
            return "Late text must not be used.";
        };
        context.provide<fv::llm::CleanupOptions>(hooks);
    };
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    std::atomic_bool completed = false;
    auto completion = rig.runtime().transcription_completed.connect(
        rig.uiExecutor(),
        [&completed](const auto&, const auto&, const auto&) { completed = true; });
    REQUIRE(rig.app().start_recording("ui"));
    REQUIRE(fvtest::waitUntil([&] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    rig.app().stop_recording();
    REQUIRE(fvtest::waitUntil([&] { return entered->load(); }, 30s));
    rig.settings().save_setting("transcript_cleanup_enabled", false);
    REQUIRE(fvtest::waitUntil([&] { return completed.load() && !rig.runtime().has_active_job(); },
                              30s));
    CHECK(cancelled->load());
    auto& history = rig.context().service<fv::HistoryManager>();
    const auto entries = fvtest::offGui([&history] { return history.get_history(); });
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "um tomorrow at ten");
    CHECK_FALSE(entries[0].raw_text);
    CHECK_FALSE(entries[0].cleanup_provider);
    CHECK(keys.offline.requests().empty());
}
