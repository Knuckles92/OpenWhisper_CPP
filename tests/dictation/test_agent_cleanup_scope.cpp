#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <optional>
#include <string>

#include "cleanup_support.h"
#include "dictation/cleanup_stage.h"
#include "text/cleanup_profiles.h"

namespace {
struct ScopeCase {
    fv::dictation::JobMode mode;
    const char* source;
    bool changedByProgram;
    bool batch;
    bool hasJob;
};
} // namespace

TEST_CASE("installed agent cleanup is limited to locally initiated ordinary dictation", "[unit]") {
    using namespace fv::dictation;
    using fv::Json;
    const auto scope = GENERATE(ScopeCase{JobMode::Dictation, "api", false, false, true},
                                ScopeCase{JobMode::Dictation, "mcp", false, false, true},
                                ScopeCase{JobMode::Dictation, "hotkey", true, false, true},
                                ScopeCase{JobMode::Dictation, "ui", false, true, true},
                                ScopeCase{JobMode::Dictation, "ui", false, false, false},
                                ScopeCase{JobMode::Command, "ui", false, false, true},
                                ScopeCase{JobMode::Transform, "ui", false, false, true},
                                ScopeCase{JobMode::Action, "ui", false, false, true},
                                ScopeCase{JobMode::Walkthrough, "ui", false, false, true});
    DictationJob job;
    job.mode = scope.mode;
    job.source = scope.source;
    job.changed_by_program = scope.changedByProgram;
    CleanupStageInput input;
    input.settings = Json{{"transcript_cleanup_enabled", true},
                          {"transcript_cleanup_provider", "codex"},
                          {"transcript_cleanup_model", "agent-test-model"},
                          {"fv_cleanup_agent_consent", "codex"}};
    input.job = scope.hasJob ? &job : nullptr;
    if (scope.batch)
        input.batch_context = "PRIVATE_BATCH_CONTEXT";
    fv::dictation::testing::FakeCleaner fake([](const auto&) { return std::string("Changed."); });
    CleanupStageServices services;
    services.cleaner = &fake;
    const auto result = run_cleanup("Original words", input, services);
    CHECK(result.text == "Original words");
    CHECK(fake.calls().empty());
    CHECK(fake.availability_checks().empty());
}

TEST_CASE("authorized local ordinary dictation can use a selected cleanup profile", "[unit]") {
    using namespace fv::dictation;
    DictationJob job;
    job.mode = JobMode::Dictation;
    job.source = "hotkey";
    CleanupStageInput input;
    input.settings = fv::Json{{"transcript_cleanup_enabled", true},
                              {"transcript_cleanup_provider", "codex"},
                              {"transcript_cleanup_model", "agent-test-model"},
                              {"fv_cleanup_agent_consent", "codex"}};
    input.job = &job;
    REQUIRE_FALSE(fv::text::starter_profiles().empty());
    input.profile = fv::text::starter_profiles().front();
    fv::dictation::testing::FakeCleaner fake([](const auto&) { return std::string("Changed."); });
    CleanupStageServices services;
    services.cleaner = &fake;
    const auto result = run_cleanup("Original words", input, services);
    CHECK(result.text == "Changed.");
    REQUIRE(fake.calls().size() == 1);
    CHECK(fake.calls()[0].text == "Original words");
}

TEST_CASE("agent cleanup applies dictionary replacements locally after the response", "[unit]") {
    using namespace fv::dictation;
    DictationJob job;
    job.mode = JobMode::Dictation;
    job.source = "ui";
    CleanupStageInput input;
    input.settings = fv::Json{
        {"transcript_cleanup_enabled", true},
        {"transcript_cleanup_provider", "codex"},
        {"transcript_cleanup_model", "agent-test-model"},
        {"fv_cleanup_agent_consent", "codex"},
        {"dictation_dictionary",
         fv::Json::array(
             {{{"id", "acme"}, {"term", "Acme"}, {"heard", fv::Json::array({"ackme"})}}})}};
    input.job = &job;
    fv::dictation::testing::FakeCleaner fake(
        [](const auto&) { return std::string("Send ackme."); });
    CleanupStageServices services;
    services.cleaner = &fake;
    const auto result = run_cleanup("send ackme", input, services);
    REQUIRE(fake.calls().size() == 1);
    CHECK(fake.calls()[0].text == "send ackme");
    CHECK(result.text == "Send Acme.");
}

TEST_CASE("an imported agent selection without a model or consent never falls back to an API",
          "[unit]") {
    using namespace fv::dictation;
    DictationJob job;
    job.mode = JobMode::Dictation;
    job.source = "ui";
    CleanupStageInput input;
    input.settings =
        fv::Json{{"transcript_cleanup_enabled", true}, {"transcript_cleanup_provider", "codex"}};
    input.job = &job;
    fv::dictation::testing::FakeCleaner fake(
        [](const auto&) { return std::string("Unexpected."); });
    CleanupStageServices services;
    services.cleaner = &fake;
    const auto result = run_cleanup("Original words", input, services);
    CHECK(result.text == "Original words");
    CHECK(fake.calls().empty());
    CHECK(fake.availability_checks().empty());
}
