// Golden vectors of services/recorder.py and services/recording_journal.py
// (tools/parity/cases/services.recorder.py, services.recording_journal.py).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dsp/level.h"
#include "golden/golden.h"
#include "pyport/datetime.h"
#include "recorder/journal.h"
#include "recorder/post_roll.h"
#include "recorder/recorder.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
namespace golden = fv::golden;

Json optional_json(const std::optional<double>& value) {
    return value ? Json(*value) : Json(nullptr);
}

std::chrono::local_time<std::chrono::microseconds> local_time_of(const std::string& iso) {
    using namespace std::chrono;
    const fv::py::DateTime when = fv::py::fromisoformat(iso);
    const local_days date{year{when.year} / month{static_cast<unsigned>(when.month)} /
                          day{static_cast<unsigned>(when.day)}};
    return local_time<microseconds>(date) + hours{when.hour} + minutes{when.minute} +
           seconds{when.second} + microseconds{when.microsecond};
}

} // namespace

TEST_CASE("golden block level db", "[golden]") {
    golden::for_each("services.recorder", "block_level_db", [](const golden::Case& c) {
        const auto dtype = c.arg<std::string>(0);
        double level = 0;
        if (dtype == "int16") {
            level = fv::dsp::block_level_db(c.arg<std::vector<std::int16_t>>(1));
        } else {
            level = fv::dsp::block_level_db(c.arg<std::vector<float>>(1));
        }
        // numpy sums the squares in its own order: the last bit may differ.
        CHECK(golden::near(c.result<double>(), level, 1e-9));
    });
}

TEST_CASE("golden post roll gate", "[golden]") {
    golden::for_each("services.recorder", "post_roll_gate", [](const golden::Case& c) {
        fv::PostRollConfig config;
        if (c.arg_count() > 4) {
            config.adaptive = c.arg<bool>(4);
        }
        fv::PostRollGate gate(c.arg<int>(0), config);
        const auto frames = c.arg<std::int64_t>(3);
        for (const double level : c.arg<std::vector<double>>(1)) {
            CHECK_FALSE(gate.observe(level, frames));
        }
        gate.arm();
        Json ended = Json::array();
        const auto after = c.arg<std::vector<double>>(2);
        for (std::size_t i = 0; i < after.size(); ++i) {
            if (gate.observe(after[i], frames)) {
                ended.push_back(i);
            }
        }
        const Json expected = c.result<Json>();
        CHECK(golden::equal(expected.at("threshold_db"), optional_json(gate.threshold_db())));
        CHECK(golden::equal(expected.at("floor_db"), optional_json(gate.floor_db())));
        CHECK(golden::equal(expected.at("speech_db"), optional_json(gate.speech_db())));
        CHECK(expected.at("fallback").get<std::string>() == gate.fallback());
        CHECK(golden::equal(expected.at("ended_at"), ended));
        CHECK(expected.at("describe").get<std::string>() == gate.describe());
        CHECK(expected.at("frames_before_stop").get<std::int64_t>() == gate.frames_before_stop());
        CHECK(expected.at("frames_after_stop").get<std::int64_t>() == gate.frames_after_stop());
    });
}

TEST_CASE("golden format start error", "[golden]") {
    golden::for_each("services.recorder", "format_start_error", [](const golden::Case& c) {
        CHECK(fv::Recorder::format_start_error(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden recovery directory", "[golden]") {
    const fv::testkit::TempDataRoot root;
    golden::for_each(
        "services.recording_journal", "recovery_directory", [&](const golden::Case& c) {
            const auto folder = fv::recovery_directory(root.path() / c.arg<std::string>(0));
            CHECK(folder.parent_path() == std::filesystem::weakly_canonical(root.path()));
            CHECK(folder.filename().string() == c.result<std::string>());
        });
}

TEST_CASE("golden session json", "[golden]") {
    golden::for_each("services.recording_journal", "session_json", [](const golden::Case& c) {
        CHECK(fv::session_json(c.arg<int>(0), c.arg<int>(1), c.arg<int>(2),
                               local_time_of(c.arg<std::string>(3))) == c.result<std::string>());
    });
}

TEST_CASE("golden recovered wav path", "[golden]") {
    const fv::testkit::TempDataRoot root;
    golden::for_each(
        "services.recording_journal", "_recovered_wav_path", [&](const golden::Case& c) {
            const auto recordings = root.path() / "recordings";
            const auto session = root.path() / c.arg<std::string>(1);
            if (c.raises()) {
                CHECK_THROWS(fv::recovered_wav_path(c.arg<std::string>(0), recordings, session));
                return;
            }
            const auto path = fv::recovered_wav_path(c.arg<std::string>(0), recordings, session);
            CHECK(path.parent_path() == recordings);
            CHECK(std::filesystem::is_directory(recordings));
            CHECK(path.filename().string() == c.result<std::string>());
        });
}

TEST_CASE("golden session format", "[golden]") {
    golden::for_each("services.recording_journal", "_session_format", [](const golden::Case& c) {
        const auto text = c.arg<std::string>(0);
        if (c.raises()) {
            CHECK_THROWS(fv::session_format(text));
            return;
        }
        const fv::SessionFormat format = fv::session_format(text);
        const Json expected = c.result<Json>();
        // Python's `in (1, 2)` lets True stand for 1, and returns it as it was stored.
        const auto number = [](const Json& value) {
            return value.is_boolean() ? (value.get<bool>() ? 1 : 0) : value.get<int>();
        };
        CHECK(number(expected.at(0)) == format.rate);
        CHECK(number(expected.at(1)) == format.channels);
        CHECK(number(expected.at(2)) == format.sample_width);
    });
}
