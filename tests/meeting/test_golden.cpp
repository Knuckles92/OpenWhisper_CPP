// Golden vectors of meeting.finalization, meeting.end_run, meeting.platform,
// ui_qt.widgets.meeting_mode_tab and services.meeting_rerun (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "end_run_scenario.h"
#include "golden/golden.h"
#include "meeting/finalization.h"
#include "meeting/finalization_view.h"
#include "meeting/platform_policy.h"
#include "meeting/rerun_options.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace golden = fv::golden;
using fv::Json;
using namespace fv::meeting;

namespace {

// The Python exception's class name, or "" when nothing pyport-shaped was thrown.
template <class Fn> std::string raised(Fn&& fn) {
    try {
        fn();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name());
    }
    return "";
}

// Checks a case: the exception Python raised, or the result as JSON.
template <class Fn> void replay(const golden::Case& c, Fn&& fn) {
    INFO(c.label());
    if (c.raises()) {
        CHECK(raised(fn) == c.raises_type());
        return;
    }
    const Json actual = fn();
    INFO("actual " << actual.dump());
    CHECK(golden::equal(c.result<Json>(), actual));
}

Json ranges(const std::vector<std::pair<std::size_t, std::size_t>>& blocks) {
    Json out = Json::array();
    for (const auto& [start, end] : blocks) {
        out.push_back(Json::array({start, end}));
    }
    return out;
}

std::vector<Json> rows_of(const Json& list) {
    return std::vector<Json>(list.begin(), list.end());
}

std::chrono::year_month_day iso_day(const std::string& iso) {
    // "YYYY-MM-DD"
    const int y = std::stoi(iso.substr(0, 4));
    const auto m = static_cast<unsigned>(std::stoi(iso.substr(5, 2)));
    const auto d = static_cast<unsigned>(std::stoi(iso.substr(8, 2)));
    return std::chrono::year_month_day{std::chrono::year{y}, std::chrono::month{m},
                                       std::chrono::day{d}};
}

Json gate_json(const SpeakerPassGate& gate) {
    // The gate never holds the key; an eligible pass had one (or needed none).
    return {
        {"ok", gate.ok}, {"reason", gate.reason}, {"offered", gate.offered}, {"has_key", false}};
}

PlatformInfo platform_of(const golden::Case& c) {
    return {c.arg<std::string>(0), c.arg<std::string>(1), c.arg<std::string>(2)};
}

// OpenWhisper's redecode detail names Whisper; OpenWhisper_CPP's names Parakeet (lead decision).
Json parakeet_wording(Json value) {
    if (value.is_object() && value.contains("detail") &&
        value["detail"] == "High-accuracy full session Whisper decode") {
        value["detail"] = "High-accuracy full session Parakeet decode";
    }
    return value;
}

Json step_row_json(const StepRow& row) {
    return {{"accessible_name", row.name + ": " + row.status},
            {"accessible_description", row.detail},
            {"icon", row.icon.glyph},
            {"icon_color", row.icon.color},
            {"name", row.name},
            {"running", row.running},
            {"detail_text", row.detail_text},
            {"action_label", row.action_label},
            {"action_enabled", row.action_enabled},
            {"action_tooltip", row.action_tooltip}};
}

Json card_json(const CardDecision& card) {
    Json steps = Json::array();
    for (const auto& row : card.steps) {
        steps.push_back(step_row_json(row));
    }
    const bool indeterminate = card.progress_minimum == 0 && card.progress_maximum == 0;
    return {
        {"title", card.title},
        {"message", card.message},
        {"tone", card.tone},
        {"keep_hint", card.keep_hint},
        {"keep_hint_visible", card.show_keep_later},
        {"report_enabled", card.report_enabled},
        {"report_tip", card.report_tip},
        {"show_step_badge", card.show_step_badge},
        {"show_progress", card.show_progress},
        {"progress_minimum", card.progress_minimum},
        {"progress_maximum", card.progress_maximum},
        {"progress_value", indeterminate ? Json() : Json(card.progress_value)},
        {"show_detail", card.show_detail},
        {"detail", card.show_detail ? Json(card.detail) : Json()},
        {"show_steps", card.show_steps},
        {"steps", steps},
        {"show_active_box", card.show_active_box},
        {"show_retry", card.show_retry},
        {"show_background", card.show_background},
        {"show_keep_later", card.show_keep_later},
        {"show_done", card.show_done},
        {"show_open_report", card.show_open_report},
        {"show_rerun_speakers", card.show_rerun_speakers},
        {"rerun_speakers_enabled",
         card.show_rerun_speakers ? Json(card.rerun_speakers_enabled) : Json()},
        {"rerun_speakers_tip", card.rerun_speakers_tip},
    };
}

// The step badge keeps the text of an earlier render when it is hidden, so it is compared only
// while shown; the retry button is always enabled.
void check_card(const golden::Case& c, const CardDecision& card) {
    Json expected = c.result<Json>();
    Json actual = card_json(card);
    if (!expected.at("show_step_badge").get<bool>()) {
        expected.erase("step_badge");
    } else {
        actual["step_badge"] = card.step_badge;
    }
    CHECK(expected.at("retry_enabled") == true);
    expected.erase("retry_enabled");
    INFO(c.label());
    INFO("actual " << actual.dump());
    CHECK(golden::equal(expected, actual));
}

// meeting/asr/remote.py saved_remote_route with _is_this_computer False: the row's route when it
// names a fingerprint, a family and a model, ValueError otherwise, nullopt without one.
std::optional<Json> saved_remote_route(const Json& meeting) {
    if (!meeting.contains("asr_remote_json") || meeting.at("asr_remote_json").is_null()) {
        return std::nullopt;
    }
    Json route;
    try {
        route = fv::py::loads(meeting.at("asr_remote_json").get<std::string>());
    } catch (const std::exception&) {
        throw fv::py::ValueError("This meeting's remote speech configuration is invalid.");
    }
    for (const char* key : {"fingerprint", "family", "model"}) {
        if (!route.is_object() || !route.contains(key) || !route.at(key).is_string() ||
            route.at(key).get<std::string>().empty()) {
            throw fv::py::ValueError("This meeting's remote speech configuration is invalid.");
        }
    }
    return route;
}

// The services.meeting_rerun pin: 2027-03-01, after OpenAI's shutdown.
const fv::FakeClock& after_shutdown() {
    static const fv::FakeClock clock(
        std::chrono::sys_days{std::chrono::year{2027} / 3 / 1} + std::chrono::hours{12}, "UTC");
    return clock;
}

} // namespace

// ---- meeting.finalization ----

TEST_CASE("golden meeting.finalization polish_blocks", "[meeting][golden]") {
    golden::for_each("meeting.finalization", "polish_blocks_lengths", [](const golden::Case& c) {
        std::vector<Json> rows;
        int index = 0;
        for (const auto length : c.arg<std::vector<std::size_t>>(0)) {
            rows.push_back(
                {{"id", "sg_" + std::to_string(index++)}, {"text", std::string(length, 'x')}});
        }
        replay(c, [&] { return ranges(polish_blocks(rows)); });
    });
    golden::for_each("meeting.finalization", "polish_blocks", [](const golden::Case& c) {
        replay(c, [&] { return ranges(polish_blocks(rows_of(c.arg<Json>(0)))); });
    });
}

TEST_CASE("golden meeting.finalization summary and sentences", "[meeting][golden]") {
    golden::for_each("meeting.finalization", "summary_stats", [](const golden::Case& c) {
        replay(c, [&] {
            return summary_stats(c.arg<Json>(0), c.arg<Json>(1), rows_of(c.arg<Json>(2)),
                                 c.arg<double>(3));
        });
    });
    golden::for_each("meeting.finalization", "saved_state_detail", [](const golden::Case& c) {
        replay(c, [&] { return Json(saved_state_detail(c.arg<Json>(0))); });
    });
    golden::for_each("meeting.finalization", "insights_ready_message", [](const golden::Case& c) {
        replay(c, [&] { return Json(insights_ready_message(c.arg<Json>(0))); });
    });
    golden::for_each("meeting.finalization", "failed_steps_message", [](const golden::Case& c) {
        replay(c, [&] { return Json(failed_steps_message(c.arg<Json>(0))); });
    });
    golden::for_each("meeting.finalization", "sparse_redecode_detail", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(sparse_redecode_detail(c.arg<std::int64_t>(0), c.arg<std::int64_t>(1)));
        });
    });
}

TEST_CASE("golden meeting.finalization steps", "[meeting][golden]") {
    golden::for_each("meeting.finalization", "make_step", [](const golden::Case& c) {
        INFO(c.label());
        if (c.raises()) {
            CHECK(raised([&] { make_step(c.arg<std::string>(0), c.arg<std::string>(1)); }) ==
                  c.raises_type());
            return;
        }
        CHECK(golden::equal(parakeet_wording(c.result<Json>()),
                            make_step(c.arg<std::string>(0), c.arg<std::string>(1))));
    });
    golden::for_each("meeting.finalization", "speaker_step_outcome", [](const golden::Case& c) {
        replay(c, [&] {
            const auto [status, detail] = speaker_step_outcome(c.arg<Json>(0));
            return Json::array({status, detail});
        });
    });
}

TEST_CASE("golden meeting.finalization speaker_pass_gate", "[meeting][golden]") {
    golden::for_each("meeting.finalization", "speaker_pass_gate", [](const golden::Case& c) {
        const std::optional<std::string> key = c.arg<std::optional<std::string>>(2);
        std::function<bool()> key_present;
        if (key) {
            key_present = [key] {
                return !key->empty();
            };
        }
        const SpeakerPassGate gate = speaker_pass_gate(c.arg<std::string>(0), c.arg<bool>(1),
                                                       key_present, iso_day(c.arg<std::string>(3)));
        Json expected = c.result<Json>();
        INFO(c.label());
        // has_key is the Python gate carrying the key; OpenWhisper_CPP's never does.
        CHECK(expected.at("has_key") == (gate.ok && key.has_value()));
        expected["has_key"] = false;
        CHECK(golden::equal(expected, gate_json(gate)));
    });
}

// ---- meeting.end_run ----

TEST_CASE("golden meeting.end_run", "[meeting][golden]") {
    golden::for_each("meeting.end_run", "run", [](const golden::Case& c) {
        INFO(c.label());
        const Json actual = fv::meeting::testing::run_scenario(c.arg<Json>(0));
        Json expected = c.result<Json>();
        // The redecode step's detail as OpenWhisper_CPP words it.
        for (auto& event : expected.at("events")) {
            if (event.at(0) != "set_finalization") {
                continue;
            }
            Json& payload = event.at(1);
            if (payload.contains("steps")) {
                for (auto& step : payload.at("steps")) {
                    step = parakeet_wording(step);
                }
            }
            if (payload.at("step_details") == "High-accuracy full session Whisper decode") {
                payload["step_details"] = "High-accuracy full session Parakeet decode";
            }
        }
        INFO("actual " << actual.dump());
        CHECK(golden::equal(expected, actual));
    });
}

// ---- meeting.platform ----

TEST_CASE("golden meeting.platform", "[meeting][golden]") {
    golden::for_each("meeting.platform", "normalize_linux_machine", [](const golden::Case& c) {
        replay(c, [&] {
            const auto value = normalize_linux_machine(c.arg<std::string>(0));
            return value ? Json(*value) : Json();
        });
    });
    golden::for_each(
        "meeting.platform", "linux_meeting_implementation_ready", [](const golden::Case& c) {
            replay(c,
                   [&] { return Json(linux_meeting_implementation_ready(c.arg<std::string>(0))); });
        });
    golden::for_each("meeting.platform", "_macos_major", [](const golden::Case& c) {
        replay(c, [&] { return Json(macos_major(c.arg<std::string>(0))); });
    });
    golden::for_each("meeting.platform", "meeting_mode_supported", [](const golden::Case& c) {
        replay(c, [&] { return Json(meeting_mode_supported(platform_of(c))); });
    });
    golden::for_each("meeting.platform", "meeting_setup_available", [](const golden::Case& c) {
        replay(c, [&] { return Json(meeting_setup_available(platform_of(c))); });
    });
    golden::for_each("meeting.platform", "meeting_unsupported_os_name", [](const golden::Case& c) {
        replay(c, [&] { return Json(meeting_unsupported_os_name(c.arg<std::string>(0))); });
    });
    golden::for_each(
        "meeting.platform", "system_audio_permission_required", [](const golden::Case& c) {
            replay(c,
                   [&] { return Json(system_audio_permission_required(c.arg<std::string>(0))); });
        });
    golden::for_each(
        "meeting.platform", "system_audio_permission_granted", [](const golden::Case& c) {
            replay(c, [&] {
                return Json(
                    system_audio_permission_granted(PlatformInfo{c.arg<std::string>(0), "", ""}));
            });
        });
}

// ---- ui_qt.widgets.meeting_mode_tab ----

TEST_CASE("golden meeting_mode_tab helpers", "[meeting][golden]") {
    constexpr std::string_view kModule = "ui_qt.widgets.meeting_mode_tab";
    golden::for_each(kModule, "_finalization_heading", [](const golden::Case& c) {
        replay(c, [&] {
            const auto [title, message] =
                finalization_heading(c.arg<std::string>(0), c.arg<std::string>(1), c.arg<bool>(2),
                                     c.arg<bool>(3), c.arg<bool>(4));
            return Json::array({title, message});
        });
    });
    // A status that is not a string compares unequal to every name, as its str() does.
    const auto status_text = [](const Json& status) {
        return status.is_string() ? status.get<std::string>() : status.dump();
    };
    golden::for_each(kModule, "_step_icon", [&](const golden::Case& c) {
        replay(c, [&] {
            const StepIcon icon = step_icon(status_text(c.arg<Json>(0)));
            return Json::array({icon.glyph, icon.color});
        });
    });
    golden::for_each(kModule, "_step_detail_text", [&](const golden::Case& c) {
        replay(c, [&] { return Json(step_detail_text(status_text(c.arg<Json>(0)))); });
    });
    golden::for_each(kModule, "_step_action_label", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(
                step_action_label(c.arg<std::string>(0), c.arg<std::string>(1), c.arg<bool>(2)));
        });
    });
    golden::for_each(kModule, "progress_percent", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(progress_percent(c.arg<std::int64_t>(0), c.arg<std::int64_t>(1)));
        });
    });
    golden::for_each(kModule, "elapsed_text", [](const golden::Case& c) {
        replay(c, [&] { return Json(elapsed_text(c.arg<double>(0))); });
    });
    golden::for_each(kModule, "meeting_audio_support_copy", [](const golden::Case& c) {
        replay(c, [&] {
            const auto [subtitle, hint] = meeting_audio_support_copy(platform_of(c));
            return Json::array({subtitle, hint});
        });
    });
    golden::for_each(kModule, "meeting_audio_platform_tone", [](const golden::Case& c) {
        replay(c, [&] { return Json(meeting_audio_platform_tone(platform_of(c))); });
    });
    golden::for_each(kModule, "meeting_audio_shows_platform_warning", [](const golden::Case& c) {
        replay(c, [&] { return Json(meeting_audio_shows_platform_warning(platform_of(c))); });
    });
}

TEST_CASE("golden meeting_mode_tab finalization view", "[meeting][golden]") {
    golden::for_each("ui_qt.widgets.meeting_mode_tab", "finalization_view",
                     [](const golden::Case& c) {
                         replay(c, [&] {
                             const FinalizationView view = FinalizationView::read(c.arg<Json>(0));
                             return Json{{"status", view.status},
                                         {"message", view.message},
                                         {"current_step", view.current_step},
                                         {"total_steps", view.total_steps},
                                         {"step_details", view.step_details},
                                         {"steps", view.steps},
                                         {"content", view.content},
                                         {"meeting_failed", view.meeting_failed},
                                         {"empty_meeting", view.empty_meeting}};
                         });
                     });
}

TEST_CASE("golden meeting_mode_tab finalization card", "[meeting][golden]") {
    golden::for_each("ui_qt.widgets.meeting_mode_tab", "card", [](const golden::Case& c) {
        check_card(c, decide_card(FinalizationView::read(c.arg<Json>(0)), c.arg<bool>(1)));
    });
}

// ---- services.meeting_rerun ----

TEST_CASE("golden services.meeting_rerun", "[meeting][golden]") {
    golden::for_each("services.meeting_rerun", "rerun_options", [](const golden::Case& c) {
        replay(c, [&] {
            const RerunOptions options =
                rerun_options(c.arg<Json>(0), c.arg<Json>(1), after_shutdown(), saved_remote_route);
            return Json{{"timeout_s", options.timeout_s},
                        {"asr_model_name", options.asr_model_name},
                        {"language", options.language},
                        {"speaker_id_backend", options.speaker_id_backend},
                        {"speaker_audio_consent", options.speaker_audio_consent},
                        {"redecode_coverage_guard", options.redecode_coverage_guard}};
        });
    });
    golden::for_each("services.meeting_rerun", "resolve_speaker_pass", [](const golden::Case& c) {
        replay(c,
               [&] { return gate_json(resolve_speaker_pass(c.arg<Json>(0), after_shutdown())); });
    });
}
