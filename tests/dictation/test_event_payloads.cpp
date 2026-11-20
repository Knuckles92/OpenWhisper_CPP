#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "dictation/cleanup_info.h"
#include "dictation/events.h"
#include "dictation/job.h"
#include "golden/golden.h"
#include "pyport/json.h"

// Replays the job builders of tests/golden/services.automation.catalog.json
// (tools/parity/cases/services.automation.catalog.py): the payloads fv_dictation publishes for
// recordings, transcriptions and history entries, which fv_automation re-exports.

using namespace fv::dictation;
using fv::Json;
namespace golden = fv::golden;

namespace {

constexpr std::string_view kCatalog = "services.automation.catalog";

std::string text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

// A DictationJob from a dict of its fields, or nullptr for null.
const DictationJob* job_from(const Json& spec, DictationJob& storage) {
    if (spec.is_null()) {
        return nullptr;
    }
    storage = DictationJob{};
    storage.id = spec.value("id", storage.id);
    storage.mode = job_mode_from(spec.value("mode", "dictation")).value_or(JobMode::Dictation);
    storage.action_id = spec.value("action_id", "");
    storage.source = spec.value("source", "");
    storage.route = spec.value("route", "");
    storage.routed = spec.value("routed", false);
    storage.changed_by_program = spec.value("changed_by_program", false);
    return &storage;
}

std::optional<CleanupInfo> cleanup_from(const Json& spec) {
    if (spec.is_null()) {
        return std::nullopt;
    }
    CleanupInfo info;
    info.provider = text(spec.value("provider", Json()));
    info.model = text(spec.value("model", Json()));
    info.level = text(spec.value("level", Json()));
    return info;
}

} // namespace

// from tests/test_automation_foundation_events.py
TEST_CASE("test_completed_payload_for_a_file_and_for_a_job",
          "[test_automation_foundation_events]") {
    Json data = events::transcription_completed(nullptr, "hi", std::string("hi"), Json::object(),
                                                std::nullopt, std::nullopt);
    CHECK(data["mode"] == "file");
    CHECK(data["raw_text"].is_null());
    CHECK(data["app"].is_null());

    DictationJob job;
    job.mode = JobMode::Action;
    job.action_id = "a1";
    job.source = "api";
    job.routed = true;
    data = events::transcription_completed(&job, "Hi.", std::string("hi"),
                                           Json{{"app_id", "notepad.exe"},
                                                {"app_name", "Notepad"},
                                                {"app_category", "docs"},
                                                {"language", "en"}},
                                           std::nullopt, 1.23456);
    CHECK(data["job_id"] == job.id);
    CHECK(data["mode"] == "action");
    CHECK(data["action_id"] == "a1");
    CHECK(data["source"] == "api");
    CHECK(data["routed"] == true);
    CHECK(data["raw_text"] == "hi");
    CHECK(data["language"] == "en");
    CHECK(data["app"] == Json{{"id", "notepad.exe"}, {"name", "Notepad"}, {"category", "docs"}});
    CHECK(data["audio_s"] == 1.235);
    CHECK_NOTHROW(fv::py::dumps(data));
}

TEST_CASE("event_payloads job_fields and recording builders", "[golden]") {
    golden::for_each(kCatalog, "job_fields", [](const golden::Case& c) {
        DictationJob storage;
        CHECK(
            golden::equal(c.result<Json>(), events::job_fields(job_from(c.arg<Json>(0), storage))));
    });
    golden::for_each(kCatalog, "recording_started", [](const golden::Case& c) {
        DictationJob storage;
        CHECK(golden::equal(c.result<Json>(),
                            events::recording_started(job_from(c.arg<Json>(0), storage))));
    });
    golden::for_each(kCatalog, "recording_stopped", [](const golden::Case& c) {
        DictationJob storage;
        CHECK(golden::equal(c.result<Json>(),
                            events::recording_stopped(job_from(c.arg<Json>(0), storage),
                                                      c.arg<std::optional<double>>(1))));
    });
    golden::for_each(kCatalog, "recording_canceled", [](const golden::Case& c) {
        DictationJob storage;
        CHECK(golden::equal(c.result<Json>(),
                            events::recording_canceled(job_from(c.arg<Json>(0), storage))));
    });
}

TEST_CASE("event_payloads transcription builders", "[golden]") {
    golden::for_each(kCatalog, "transcription_preview", [](const golden::Case& c) {
        DictationJob storage;
        CHECK(golden::equal(c.result<Json>(),
                            events::transcription_preview(job_from(c.arg<Json>(0), storage),
                                                          text(c.arg<Json>(1)), c.arg<bool>(2))));
    });
    golden::for_each(kCatalog, "transcription_completed", [](const golden::Case& c) {
        DictationJob storage;
        const Json actual = events::transcription_completed(
            job_from(c.arg<Json>(0), storage), text(c.arg<Json>(1)),
            c.arg<std::optional<std::string>>(2), c.arg<Json>(3), cleanup_from(c.arg<Json>(4)),
            c.arg<std::optional<double>>(5));
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(kCatalog, "transcription_failed", [](const golden::Case& c) {
        DictationJob storage;
        const auto message = c.arg<std::optional<std::string>>(2);
        const Json actual = events::transcription_failed(
            job_from(c.arg<Json>(0), storage), text(c.arg<Json>(1)),
            message ? std::optional<std::string_view>(*message) : std::nullopt);
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("event_payloads history_saved and app_from_fields", "[golden]") {
    golden::for_each(kCatalog, "history_saved", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            events::history_saved(text(c.arg<Json>(0)), text(c.arg<Json>(1)),
                                                  text(c.arg<Json>(2)))));
    });
    golden::for_each(kCatalog, "app_from_fields", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), events::app_from_fields(c.arg<Json>(0))));
    });
}
