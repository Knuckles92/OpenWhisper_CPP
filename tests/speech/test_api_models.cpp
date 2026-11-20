// OpenAI's model choices and its 2027-02-26 shutdown of the older transcription models
// (services/openai_retirement.py, services/settings.py's API model functions).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "settings/store.h"
#include "speech/api_models.h"
#include "testkit/testkit.h"

using namespace fv;
using namespace fv::speech;
using namespace std::chrono;

namespace {

constexpr year_month_day kBefore{year{2027}, month{2}, day{25}};
constexpr year_month_day kAfter = kShutdownDay;
const std::vector<std::string> kRetiring{"gpt-4o-transcribe", "gpt-4o-mini-transcribe",
                                         "whisper-1"};

year_month_day iso_day(const std::string& text) {
    return year_month_day{year{std::stoi(text.substr(0, 4))},
                          month{static_cast<unsigned>(std::stoi(text.substr(5, 2)))},
                          day{static_cast<unsigned>(std::stoi(text.substr(8, 2)))}};
}

} // namespace

// from tests/test_openai_retirement.py::TestShutdownDate::test_boundary_is_the_shutdown_day
TEST_CASE("TestShutdownDate::test_boundary_is_the_shutdown_day", "[test_openai_retirement]") {
    CHECK_FALSE(retired(kBefore));
    CHECK(retired(kAfter));
}

// from tests/test_openai_retirement.py::TestShutdownDate::test_default_reads_the_pinned_clock
TEST_CASE("TestShutdownDate::test_default_reads_the_pinned_clock", "[test_openai_retirement]") {
    // The clock's local day decides: 2027-02-25 23:30 in UTC is already the 26th in Rome.
    FakeClock clock(sys_days{year{2027} / 2 / 25} + 23h + 30min, "Europe/Rome");
    CHECK(retired(local_today(clock)));
    clock.set_zone("UTC");
    CHECK_FALSE(retired(local_today(clock)));
}

// from tests/test_openai_retirement.py::TestApiModelChoices::
// test_retiring_models_stay_listed_with_a_label_until_shutdown
TEST_CASE("TestApiModelChoices::test_retiring_models_stay_listed_with_a_label_until_shutdown",
          "[test_openai_retirement]") {
    CHECK(api_model_choices(kBefore) ==
          std::vector<std::string>{"gpt-transcribe", "gpt-4o-transcribe", "gpt-4o-mini-transcribe",
                                   "whisper-1"});
    CHECK(api_model_label("whisper-1", kBefore) == "whisper-1 (retiring Feb 26, 2027)");
    CHECK(api_model_label("gpt-transcribe", kBefore) == "gpt-transcribe");
}

// from tests/test_openai_retirement.py::TestApiModelChoices::
// test_only_gpt_transcribe_remains_after_shutdown
TEST_CASE("TestApiModelChoices::test_only_gpt_transcribe_remains_after_shutdown",
          "[test_openai_retirement]") {
    CHECK(api_model_choices(kAfter) == std::vector<std::string>{"gpt-transcribe"});
}

// from tests/test_openai_retirement.py::TestApiModelChoices::
// test_saved_retiring_model_is_kept_until_shutdown
TEST_CASE("TestApiModelChoices::test_saved_retiring_model_is_kept_until_shutdown",
          "[test_openai_retirement]") {
    for (const std::string& model : kRetiring) {
        const Json settings{{"api_transcription_model", model}};
        CHECK(resolve_api_transcription_model(settings, kBefore) == model);
        CHECK(resolve_api_transcription_model(settings, kAfter) == "gpt-transcribe");
    }
}

// from tests/test_openai_retirement.py::TestApiModelChoices::
// test_legacy_selected_model_resolves_to_gpt_transcribe_after_shutdown
TEST_CASE("TestApiModelChoices::test_legacy_selected_model_resolves_to_gpt_transcribe_after_"
          "shutdown",
          "[test_openai_retirement]") {
    for (const char* legacy : {"api_whisper", "api_gpt4o", "api_gpt4o_mini"}) {
        const Json settings{{"selected_model", legacy}};
        CHECK(resolve_api_transcription_model(settings, kAfter) == "gpt-transcribe");
    }
}

// from tests/test_openai_retirement.py::TestModelGoneError::test_not_found_counts
TEST_CASE("TestModelGoneError::test_not_found_counts", "[test_openai_retirement]") {
    CHECK(is_model_gone_error(SpeechApiError("error", 404, std::nullopt)));
}

// from tests/test_openai_retirement.py::TestModelGoneError::
// test_model_not_found_code_counts_on_any_status
TEST_CASE("TestModelGoneError::test_model_not_found_code_counts_on_any_status",
          "[test_openai_retirement]") {
    CHECK(is_model_gone_error(SpeechApiError("error", 400, "model_not_found")));
}

// from tests/test_openai_retirement.py::TestModelGoneError::test_unsupported_format_does_not_count
TEST_CASE("TestModelGoneError::test_unsupported_format_does_not_count",
          "[test_openai_retirement]") {
    // The shape gpt-transcribe returns for response_format="diarized_json".
    CHECK_FALSE(is_model_gone_error(SpeechApiError("error", 400, "unsupported_value")));
    CHECK_FALSE(is_model_gone_error(std::runtime_error("down")));
}

// from tests/test_settings.py::test_api_model_resolution_defaults_and_preserves_explicit_choice
TEST_CASE("test_api_model_resolution_defaults_and_preserves_explicit_choice", "[test_settings]") {
    const year_month_day today{year{2026}, month{11}, day{15}};
    CHECK(resolve_api_transcription_model(Json::object(), today) == "gpt-transcribe");
    CHECK(resolve_api_transcription_model(Json{{"api_transcription_model", Json::array()}},
                                          today) == "gpt-transcribe");
    const testkit::TempDataRoot root;
    SettingsStore store(root.path() / "settings.json");
    store.update(Json{{"selected_model", "api_whisper"},
                      {"api_transcription_model", "gpt-4o-mini-transcribe"}});
    CHECK(load_model_selection(store, today) == "api");
    CHECK(store.get(keys::api_transcription_model) == "gpt-4o-mini-transcribe");
}

TEST_CASE("retired matches Python around the shutdown day", "[golden]") {
    golden::for_each("services.openai_retirement", "retired", [](const golden::Case& c) {
        CHECK(retired(iso_day(c.arg<std::string>(0))) == c.result<bool>());
    });
}

TEST_CASE("is_model_gone_error matches Python for status and code", "[golden]") {
    golden::for_each("services.openai_retirement", "is_model_gone_error",
                     [](const golden::Case& c) {
                         const SpeechApiError error("error", c.arg<std::optional<int>>(0),
                                                    c.arg<std::optional<std::string>>(1));
                         CHECK(is_model_gone_error(error) == c.result<bool>());
                     });
}

TEST_CASE("the API model functions match Python's settings module", "[golden]") {
    golden::for_each("services.settings", "api_model_choices", [](const golden::Case& c) {
        CHECK(api_model_choices(iso_day(c.arg<std::string>(0))) ==
              c.result<std::vector<std::string>>());
    });
    golden::for_each("services.settings", "api_model_label", [](const golden::Case& c) {
        CHECK(api_model_label(c.arg<std::string>(0), iso_day(c.arg<std::string>(1))) ==
              c.result<std::string>());
    });
    golden::for_each("services.settings", "serving_api_model", [](const golden::Case& c) {
        CHECK(serving_api_model(c.arg<std::string>(0), iso_day(c.arg<std::string>(1))) ==
              c.result<std::string>());
    });
    golden::for_each(
        "services.settings", "resolve_api_transcription_model", [](const golden::Case& c) {
            CHECK(resolve_api_transcription_model(c.arg<Json>(0), iso_day(c.arg<std::string>(1))) ==
                  c.result<std::string>());
        });
}
