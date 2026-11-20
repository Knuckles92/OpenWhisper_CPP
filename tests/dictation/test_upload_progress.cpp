// The upload progress panel's pure rules (ui_qt/widgets/transcription_progress.py), replayed
// from tests/golden/ui_qt.widgets.transcription_progress.json.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "appcore/overlay_state.h"
#include "dictation/upload_progress.h"
#include "golden/golden.h"
#include "pyport/errors.h"

namespace {

using namespace fv::dictation;

constexpr const char* kModule = "ui_qt.widgets.transcription_progress";

ProgressStage stage_from(const std::string& value) {
    for (const auto stage : {ProgressStage::Preparing, ProgressStage::Splitting,
                             ProgressStage::Transcribing, ProgressStage::Cleaning,
                             ProgressStage::Done, ProgressStage::Failed, ProgressStage::Canceled}) {
        if (to_string(stage) == value) {
            return stage;
        }
    }
    throw std::invalid_argument("no stage " + value);
}

StepState step_from(const std::string& value) {
    for (const auto state :
         {StepState::Pending, StepState::Active, StepState::Done, StepState::Failed}) {
        if (to_string(state) == value) {
            return state;
        }
    }
    throw std::invalid_argument("no step state " + value);
}

// The bar after set_stage, as TranscriptionProgressPanel drives it: a terminal stage fills it
// (Done) or empties it, a batch moves it by the stage's weight, a single file sweeps.
fv::Json bar_after(ProgressStage stage, BatchPosition batch) {
    if (is_terminal(stage)) {
        return stage == ProgressStage::Done ? fv::Json(1.0) : fv::Json("reset");
    }
    if (batch.total > 1) {
        return batch_fraction(batch.position, batch.total, batch_stage_weight(stage));
    }
    return "indeterminate";
}

} // namespace

TEST_CASE("the stepper follows a job from start to end") {
    StepStates states = INITIAL_STEP_STATES;
    states = step_states(ProgressStage::Preparing, states);
    CHECK(states == StepStates{StepState::Active, StepState::Pending, StepState::Pending});
    states = step_states(ProgressStage::Transcribing, states);
    CHECK(states == StepStates{StepState::Done, StepState::Active, StepState::Pending});
    const StepStates failed = step_states(ProgressStage::Failed, states);
    CHECK(failed == StepStates{StepState::Done, StepState::Failed, StepState::Pending});
    CHECK(step_states(ProgressStage::Done, states) ==
          StepStates{StepState::Done, StepState::Done, StepState::Done});
    CHECK(visible_steps(true) == 3);
    CHECK(visible_steps(false) == 2);
}

TEST_CASE("only done, failed and canceled are terminal") {
    CHECK(is_terminal(ProgressStage::Done));
    CHECK(is_terminal(ProgressStage::Failed));
    CHECK(is_terminal(ProgressStage::Canceled));
    CHECK_FALSE(is_terminal(ProgressStage::Preparing));
    CHECK_FALSE(is_terminal(ProgressStage::Splitting));
    CHECK_FALSE(is_terminal(ProgressStage::Transcribing));
    CHECK_FALSE(is_terminal(ProgressStage::Cleaning));
}

TEST_CASE("elapsed time reads as minutes and seconds") {
    CHECK(format_elapsed(0) == "0:00");
    CHECK(format_elapsed(59.99) == "0:59");
    CHECK(format_elapsed(61.5) == "1:01");
    CHECK(format_elapsed(-3) == "0:00");
    CHECK(format_elapsed(3661.7) == "61:01");
    CHECK_THROWS_AS(format_elapsed(std::nan("")), fv::py::ValueError);
}

TEST_CASE("golden transcription_progress format_elapsed", "[golden]") {
    fv::golden::for_each(kModule, "format_elapsed", [](const fv::golden::Case& c) {
        if (c.raises()) {
            try {
                format_elapsed(c.arg<double>(0));
                FAIL("no exception");
            } catch (const fv::py::PyError& error) {
                CHECK(error.type_name() == c.raises_type());
            }
            return;
        }
        CHECK(format_elapsed(c.arg<double>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden transcription_progress stage_for_overlay_state", "[golden]") {
    fv::golden::for_each(kModule, "stage_for_overlay_state", [](const fv::golden::Case& c) {
        const auto state = fv::appcore::overlay_state_from(c.arg<std::string>(0));
        REQUIRE(state.has_value());
        const auto stage = stage_for_overlay_state(*state);
        const auto expected = c.result<std::optional<std::string>>();
        CHECK(stage.has_value() == expected.has_value());
        if (stage && expected) {
            CHECK(to_string(*stage) == *expected);
        }
    });
}

TEST_CASE("golden transcription_progress stage titles", "[golden]") {
    fv::golden::for_each(kModule, "stage_title", [](const fv::golden::Case& c) {
        CHECK(stage_title(stage_from(c.arg<std::string>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden transcription_progress step states", "[golden]") {
    fv::golden::for_each(kModule, "apply_steps", [](const fv::golden::Case& c) {
        const auto before = c.arg<std::vector<std::string>>(1);
        const StepStates current{step_from(before.at(0)), step_from(before.at(1)),
                                 step_from(before.at(2))};
        const StepStates after = step_states(stage_from(c.arg<std::string>(0)), current);
        std::vector<std::string> names;
        for (const StepState state : after) {
            names.emplace_back(to_string(state));
        }
        CHECK(names == c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden transcription_progress batch position", "[golden]") {
    fv::golden::for_each(kModule, "set_batch_position", [](const fv::golden::Case& c) {
        const BatchPosition batch = clamp_batch_position(c.arg<int>(1), c.arg<int>(2));
        const fv::Json expected = c.result<fv::Json>();
        CHECK(batch.position == expected.at(0).get<int>());
        CHECK(batch.total == expected.at(1).get<int>());
        CHECK(batch_fraction(batch.position, batch.total, 0.0) ==
              fv::golden::decode<double>(expected.at(2)));
    });
}

TEST_CASE("golden transcription_progress batch bar", "[golden]") {
    fv::golden::for_each(kModule, "set_stage_bar", [](const fv::golden::Case& c) {
        const BatchPosition batch = clamp_batch_position(c.arg<int>(1), c.arg<int>(2));
        const fv::Json actual = bar_after(stage_from(c.arg<std::string>(3)), batch);
        CHECK(fv::golden::equal(c.result<fv::Json>(), actual));
    });
}
