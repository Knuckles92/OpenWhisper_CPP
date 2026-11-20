// Replays the cleanup cases of tests/golden/services.dictation_pipeline.json
// (tools/parity/cases/services.dictation_pipeline.py says how each is built): the prompt of one
// cleanup pass, and what a prepared transcript gives Stats, both through the real dictionary and
// snippets.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dictation/cleanup_prompt.h"
#include "dictation/job.h"
#include "dictation/pipeline.h"
#include "dictation/text_preparer.h"
#include "focus/capture_service.h"
#include "golden/golden.h"
#include "history/dictation_stats.h"
#include "text/cleanup_profiles.h"

using namespace fv::dictation;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::FocusSnapshot;
using fv::focus::TextContext;
namespace golden = fv::golden;

namespace {

constexpr std::string_view kPipeline = "services.dictation_pipeline";

std::optional<AppIdentity> identity_from(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    AppIdentity identity;
    identity.app_id = fields.value("app_id", "");
    identity.name = fields.value("name", "");
    if (fields.contains("pid") && !fields.at("pid").is_null()) {
        identity.pid = fields.at("pid").get<std::uint32_t>();
    }
    identity.window = fields.value("window", "");
    identity.title_hint = fields.value("title_hint", "");
    identity.is_self = fields.value("is_self", false);
    identity.platform = fields.value("platform", "");
    return identity;
}

std::optional<TextContext> context_from(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    TextContext context;
    context.before = fields.value("before", "");
    context.selected = fields.value("selected", "");
    context.after = fields.value("after", "");
    context.caret_known = fields.value("caret_known", false);
    context.selection_known = fields.value("selection_known", false);
    context.source = fields.value("source", "");
    return context;
}

std::shared_ptr<fv::focus::FocusRequest> request_from(const Json& fields) {
    if (fields.is_null()) {
        return nullptr;
    }
    return fv::focus::FocusRequest::resolved(
        FocusSnapshot{identity_from(fields.value("identity", Json())),
                      context_from(fields.value("text", Json()))});
}

// The case file's _job.
std::shared_ptr<const DictationJob> job_from(const Json& spec) {
    if (spec.is_null()) {
        return nullptr;
    }
    DictationJob job;
    const auto mode = job_mode_from(spec.at("mode").get<std::string>());
    REQUIRE(mode);
    job.mode = *mode;
    job.focus = request_from(spec.at("focus"));
    job.target = request_from(spec.at("target"));
    job.source = spec.at("source").get<std::string>();
    job.route = spec.at("route").get<std::string>();
    return std::make_shared<const DictationJob>(std::move(job));
}

std::optional<fv::text::CleanupProfile> profile_from(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    return fv::text::CleanupProfile{
        fields.at("id").get<std::string>(), fields.at("name").get<std::string>(),
        fields.at("instructions").get<std::string>(), fields.at("hotkey").get<std::string>(),
        fields.at("use_learned_rules").get<bool>()};
}

} // namespace

TEST_CASE("golden: compose_cleanup_prompt", "[dictation][golden]") {
    golden::for_each(kPipeline, "compose_cleanup_prompt", [](const golden::Case& c) {
        const Json settings = c.args()[0];
        const auto job = job_from(c.args()[1]);
        const auto profile = profile_from(c.args()[2]);
        const auto rules = c.arg<std::vector<std::string>>(3);
        const auto batch_context = c.arg<std::optional<std::string>>(5);
        const PreparedText prepared =
            prepare_text(c.arg<std::string>(4), job.get(), settings, dictionary_preparer());

        const std::string prompt = compose_cleanup_prompt(
            settings, job.get(), profile ? &*profile : nullptr, rules, prepared,
            batch_context ? std::optional<std::string_view>(*batch_context) : std::nullopt);

        const Json expected = c.result<Json>();
        CHECK(prepared.cleanup_input == expected.at("cleanup_input").get<std::string>());
        CHECK(prompt == expected.at("prompt").get<std::string>());
    });
}

TEST_CASE("golden: stats_counts of a prepared transcript", "[dictation][golden]") {
    const WordCounter words{
        [](std::string_view text) { return fv::dictation_stats::count_words(text); },
        [](std::string_view before, std::string_view after) {
            const auto edited = fv::dictation_stats::words_edited(before, after);
            return edited > 0 ? static_cast<std::size_t>(edited) : std::size_t{0};
        }};
    golden::for_each(kPipeline, "prepared_stats_counts", [&words](const golden::Case& c) {
        const auto job = job_from(c.args()[1]);
        const PreparedText prepared =
            prepare_text(c.arg<std::string>(0), job.get(), c.args()[2], dictionary_preparer());

        const Json expected = c.result<Json>();
        CHECK(prepared.text == expected.at("text").get<std::string>());
        CHECK(prepared.cleanup_input == expected.at("cleanup_input").get<std::string>());
        CHECK(prepared.skip_cleanup == expected.at("skip_cleanup").get<bool>());
        CHECK(golden::equal(expected.at("counts"), stats_counts(prepared, words)));
    });
}
