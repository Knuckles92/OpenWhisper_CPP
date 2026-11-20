#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "dictation/job.h"
#include "dictation/messages.h"
#include "dictation/pipeline.h"
#include "focus/testing/fake_platform.h"
#include "golden/golden.h"

// Replays tests/golden/services.dictation_pipeline.json, services.runtime.transcription.json and
// services.automation.catalog.json (tools/parity/cases/*.py say how each case is built).

using namespace fv::dictation;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::FocusSnapshot;
using fv::focus::TextContext;
namespace golden = fv::golden;

namespace {

constexpr std::string_view kPipeline = "services.dictation_pipeline";
constexpr std::string_view kTranscription = "services.runtime.transcription";

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
    context.blocked = fields.value("blocked", false);
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

JobMode mode_from(const Json& value) {
    const auto mode = job_mode_from(value.get<std::string>());
    REQUIRE(mode.has_value());
    return *mode;
}

std::shared_ptr<const DictationJob> job_from(const Json& spec) {
    if (spec.is_null()) {
        return nullptr;
    }
    DictationJob job;
    job.mode = mode_from(spec.at("mode"));
    job.focus = request_from(spec.at("focus"));
    job.target = request_from(spec.at("target"));
    if (!spec.at("recognition").is_null()) {
        job.recognition = RecognitionContext{
            spec.at("recognition").at("language").get<std::string>(),
            spec.at("recognition").at("phrases").get<std::vector<std::string>>()};
    }
    job.source = spec.at("source").get<std::string>();
    job.route = spec.at("route").get<std::string>();
    return std::make_shared<const DictationJob>(std::move(job));
}

std::vector<std::string> keys_of(const Json& object) {
    std::vector<std::string> keys;
    for (const auto& item : object.items()) {
        keys.push_back(item.key());
    }
    return keys;
}

std::optional<std::string_view> optional_text(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return value.get_ref<const std::string&>();
}

} // namespace

// ---- services.dictation_pipeline ----

TEST_CASE("golden: text_for_paste", "[dictation][golden]") {
    golden::for_each(kPipeline, "text_for_paste", [](const golden::Case& c) {
        const auto job = job_from(c.args()[1]);
        CHECK(text_for_paste(c.arg<std::string>(0), job.get()) == c.result<std::string>());
    });
}

TEST_CASE("golden: html_for_paste", "[dictation][golden]") {
    golden::for_each(kPipeline, "html_for_paste", [](const golden::Case& c) {
        const auto job = job_from(c.args()[2]);
        CHECK(html_for_paste(c.arg<std::string>(0), c.arg<std::string>(1), job.get()) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden: paste_target and paste_target_ok", "[dictation][golden]") {
    golden::for_each(kPipeline, "paste_target", [](const golden::Case& c) {
        const auto job = job_from(c.args()[0]);
        fv::focus::FakeCaptureService service(FocusSnapshot{}, identity_from(c.args()[1]));
        const Json expected = c.result<Json>();
        CHECK(to_string(paste_target(job.get(), &service)) == expected.at(0).get<std::string>());
        CHECK(paste_target_ok(job.get(), &service) == expected.at(1).get<bool>());
    });
}

TEST_CASE("golden: history_fields", "[dictation][golden]") {
    golden::for_each(kPipeline, "history_fields", [](const golden::Case& c) {
        const auto job = job_from(c.args()[0]);
        const Json actual =
            history_fields(job.get(), optional_text(c.args()[1]), c.arg<bool>(2), c.args()[3]);
        const Json expected = c.result<Json>();
        CHECK(golden::equal(expected, actual));
        CHECK(keys_of(actual) == keys_of(expected));
    });
}

TEST_CASE("golden: stats_counts", "[dictation][golden]") {
    golden::for_each(kPipeline, "stats_counts", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        // dictation_stats' answers for these inputs, recorded with the vector.
        const Json words = expected.at("words");
        const WordCounter counter{[words](std::string_view text) {
                                      return words.at(std::string(text)).get<std::size_t>();
                                  },
                                  [corrections = expected.at("corrections").get<std::size_t>()](
                                      std::string_view, std::string_view) {
                                      return corrections;
                                  }};
        PreparedText prepared;
        prepared.original = c.arg<std::string>(0);
        prepared.text = c.arg<std::string>(1);
        prepared.cleanup_input = prepared.text;
        prepared.plan = SnippetPlan{prepared.text, std::nullopt, {}};
        const Json actual = stats_counts(prepared, counter);
        CHECK(golden::equal(expected.at("counts"), actual));
        CHECK(keys_of(actual) == keys_of(expected.at("counts")));
    });
}

TEST_CASE("golden: cleanup_level", "[dictation][golden]") {
    golden::for_each(kPipeline, "cleanup_level", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        const std::string custom = expected.at("custom").get<std::string>();
        CHECK(cleanup_level(c.args()[0], c.arg<bool>(1), [&custom](const Json&) {
                  return custom;
              }) == expected.at("level").get<std::string>());
    });
}

TEST_CASE("golden: _focus_requests", "[dictation][golden]") {
    golden::for_each(kPipeline, "_focus_requests", [](const golden::Case& c) {
        fv::focus::FakeCaptureService service(FocusSnapshot{});
        const FocusRequests requests =
            focus_requests(mode_from(c.args()[0]), c.args()[1], c.arg<std::string>(2),
                           c.arg<std::string>(3), c.arg<bool>(4), &service);
        const Json expected = c.result<Json>();
        std::vector<std::pair<bool, bool>> wanted;
        for (const char* slot : {"focus", "target"}) {
            if (!expected.at(slot).is_null()) {
                wanted.emplace_back(expected.at(slot).at(0).get<bool>(),
                                    expected.at(slot).at(1).get<bool>());
            }
        }
        CHECK(static_cast<bool>(requests.focus) == !expected.at("focus").is_null());
        CHECK(static_cast<bool>(requests.target) == !expected.at("target").is_null());
        CHECK(service.requests == wanted);
    });
}

TEST_CASE("golden: rich_paste joins", "[dictation][golden]") {
    golden::for_each(kPipeline, "rich_paste", [](const golden::Case& c) {
        TextContext context;
        context.before = c.arg<std::string>(1);
        context.after = c.arg<std::string>(2);
        context.caret_known = c.arg<bool>(3);
        context.selection_known = true;
        DictationJob job;
        job.focus = fv::focus::FocusRequest::resolved(FocusSnapshot{
            AppIdentity{"OUTLOOK.EXE", "Outlook", std::nullopt, "", "", false, "windows"},
            context});
        const Json expected = c.result<Json>();
        const std::string text = expected.at("finished_text").get<std::string>();
        CHECK(text_for_paste(text, &job) == expected.at("plain").get<std::string>());
        CHECK(html_for_paste(expected.at("finished_html").get<std::string>(), text, &job) ==
              expected.at("html").get<std::string>());
    });
}

// ---- services.runtime.transcription ----

TEST_CASE("golden: _start_failure_message", "[dictation][golden]") {
    golden::for_each(kTranscription, "_start_failure_message", [](const golden::Case& c) {
        const std::string kind = c.arg<std::string>(0);
        const std::string message = c.arg<std::string>(1);
        std::string actual;
        if (kind == "EngineNotReadyError") {
            actual = start_failure_message(EngineNotReadyError(message));
        } else if (kind == "ValueError") {
            actual = start_failure_message(std::invalid_argument(message));
        } else {
            actual = start_failure_message(std::runtime_error(message));
        }
        CHECK(actual == c.result<std::string>());
    });
}

namespace {

std::optional<RecordingAction> action_from(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    return RecordingAction{fields.at("name").get<std::string>(),
                           fields.at("kind").get<std::string>(),
                           fields.value("params", Json::object())};
}

} // namespace

TEST_CASE("golden: _recording_destination", "[dictation][golden]") {
    golden::for_each(kTranscription, "_recording_destination", [](const golden::Case& c) {
        const auto action = action_from(c.args()[0]);
        CHECK(recording_destination(action ? &*action : nullptr, mode_from(c.args()[1])) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden: _reads_selection", "[dictation][golden]") {
    golden::for_each(kTranscription, "_reads_selection", [](const golden::Case& c) {
        CHECK(reads_selection(*action_from(c.args()[0])) == c.result<bool>());
    });
}

TEST_CASE("golden: _report_busy", "[dictation][golden]") {
    golden::for_each(kTranscription, "_report_busy", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        const std::string actual = busy_message(c.arg<std::string>(0), c.arg<bool>(1));
        CHECK(golden::equal_rebranded(expected.at("returned").get<std::string>(), actual));
        REQUIRE(expected.at("status").size() == 1);
        CHECK(golden::equal_rebranded(expected.at("status").at(0).get<std::string>(), actual));
    });
}

TEST_CASE("golden: _show_listening", "[dictation][golden]") {
    golden::for_each(kTranscription, "_show_listening", [](const golden::Case& c) {
        const Listening listening = listening_status(
            mode_from(c.args()[0]), optional_text(c.args()[1]), optional_text(c.args()[2]));
        const Json expected = c.result<Json>();
        CHECK(expected.at("overlay") ==
              Json::array({std::string(fv::appcore::to_string(listening.overlay))}));
        CHECK(expected.at("status") == Json::array({listening.status}));
    });
}

TEST_CASE("golden: _error_status", "[dictation][golden]") {
    golden::for_each(kTranscription, "_error_status", [](const golden::Case& c) {
        // A job of no mode is no job: not a walkthrough. A walkthrough without a session has
        // no kept note.
        const JobMode mode = c.args()[1].is_null() ? JobMode::Dictation : mode_from(c.args()[1]);
        CHECK(error_status(c.arg<std::string>(0), c.arg<bool>(2), mode) == c.result<std::string>());
    });
}

TEST_CASE("golden: _save_failed_recording", "[dictation][golden]") {
    golden::for_each(kTranscription, "_save_failed_recording", [](const golden::Case& c) {
        const std::string outcome = c.arg<std::string>(1);
        const FailedRecordingSave save = outcome == "saved" ? FailedRecordingSave::Saved
                                         : outcome == "not_retained"
                                             ? FailedRecordingSave::AudioNotRetained
                                             : FailedRecordingSave::HistoryFailed;
        const StatusNotice actual =
            failed_recording_status(c.arg<std::string>(0), save, c.arg<bool>(2));
        const Json expected = c.result<Json>();
        REQUIRE(expected.size() == 1);
        CHECK(expected.at(0).at("error").get<std::string>() == actual.status);
        CHECK(expected.at(0).at("notice").get<std::string>() == actual.notice);
        CHECK(expected.at(0).at("refresh").get<bool>());
    });
}

TEST_CASE("golden: _preserve_failed_audio", "[dictation][golden]") {
    golden::for_each(kTranscription, "_preserve_failed_audio", [](const golden::Case& c) {
        const std::string outcome = c.arg<std::string>(1);
        const PreservedAudio preserved = outcome == "saved"          ? PreservedAudio::Saved
                                         : outcome == "not_retained" ? PreservedAudio::NotRetained
                                         : outcome == "missing"      ? PreservedAudio::Missing
                                                                     : PreservedAudio::Failed;
        const Json expected = c.result<Json>();
        REQUIRE(expected.size() == 1);
        CHECK(preserved_audio_status(c.arg<std::string>(0), preserved, c.arg<std::string>(2)) ==
              expected.at(0).at("error").get<std::string>());
    });
}

TEST_CASE("golden: _delivery_notice", "[dictation][golden]") {
    golden::for_each(kTranscription, "_delivery_notice", [](const golden::Case& c) {
        CHECK(delivery_notice(c.arg<std::string>(0), optional_text(c.args()[1]),
                              optional_text(c.args()[2])) == c.result<std::string>());
    });
}

TEST_CASE("golden: transcription constants", "[dictation][golden]") {
    golden::for_each(kTranscription, "constants", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        CHECK(expected.at("EMPTY_ASR_MESSAGE").get<std::string>() == EMPTY_ASR_MESSAGE);
        CHECK(expected.at("MIN_RECORDING_BYTES").get<std::size_t>() == MIN_RECORDING_BYTES);
        CHECK(expected.at("WALKTHROUGH_LABEL").get<std::string>() == WALKTHROUGH_LABEL);
    });
}

// ---- services.automation.catalog ----

TEST_CASE("golden: user_started", "[dictation][golden]") {
    golden::for_each("services.automation.catalog", "user_started", [](const golden::Case& c) {
        if (c.args()[0].is_string()) {
            CHECK(user_started(c.arg<std::string>(0)) == c.result<bool>());
        } else {
            // The C++ source is a string; Python's answer for anything else is "not the user".
            CHECK_FALSE(c.result<bool>());
        }
    });
}
