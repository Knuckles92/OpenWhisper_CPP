#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "appcore/overlay_state.h"

// The pure rules of the inline progress panel a file upload reports to
// (ui_qt/widgets/transcription_progress.py): its stages, the three-step stepper and the batch
// bar's fraction. Every function here is thread-safe.
namespace fv::dictation {

enum class ProgressStage { Preparing, Splitting, Transcribing, Cleaning, Done, Failed, Canceled };

// The stage's value, which the title label carries as its "stage" property: "preparing",
// "splitting", "transcribing", "cleaning", "done", "failed", "canceled".
std::string_view to_string(ProgressStage stage);
// _STAGE_TITLES: "Preparing audio", "Splitting large file", "Transcribing", "Cleaning up",
// "Done", "Failed", "Canceled".
std::string_view stage_title(ProgressStage stage);
// TERMINAL_STAGES: Done, Failed and Canceled.
bool is_terminal(ProgressStage stage);

// The stage an overlay state maps to (Processing to Preparing, Transcribing, Cleaning and
// Rewriting to Cleaning, Canceling to Canceled), or nullopt when it has none.
std::optional<ProgressStage> stage_for_overlay_state(appcore::OverlayState state);

// "m:ss" of the whole seconds in `seconds` (truncated; negative is "0:00"). NaN and infinity
// raise py::ValueError and py::OverflowError, as int() does.
std::string format_elapsed(double seconds);

// The state of one stepper chip, its "stepState" property.
enum class StepState { Pending, Active, Done, Failed };
// "pending", "active", "done", "failed".
std::string_view to_string(StepState state);

// _STEP_TITLES: "Prepare", "Transcribe", "Clean up".
inline constexpr std::array<std::string_view, 3> STEP_TITLES = {"Prepare", "Transcribe",
                                                                "Clean up"};
using StepStates = std::array<StepState, 3>;
// The chips' states when a job starts, before its first stage.
inline constexpr StepStates INITIAL_STEP_STATES = {StepState::Pending, StepState::Pending,
                                                   StepState::Pending};

// _apply_steps: the chips' states after `stage`, from their states before it. Done marks every
// step done; a stage in flight marks the steps before its own done, its own active and the rest
// pending; Failed and Canceled turn the active step failed and leave the others as they were, so
// the user sees how far the job got.
StepStates step_states(ProgressStage stage, const StepStates& current);
// Steps the stepper shows: the third ("Clean up") only with the AI cleanup pass on.
int visible_steps(bool with_cleanup);

// _BATCH_STAGE_WEIGHT: how far through one file of a batch a stage counts (Preparing 0.05,
// Splitting 0.1, Transcribing 0.2, Cleaning 0.85), 0 for a terminal stage.
double batch_stage_weight(ProgressStage stage);

// The position and total set_batch_position keeps: total at least 1, position within 1..total.
struct BatchPosition {
    int position = 1;
    int total = 1;
};
BatchPosition clamp_batch_position(int position, int total);

// The batch bar's fraction: (position - 1 + weight) / total, for a clamped position.
double batch_fraction(int position, int total, double weight);

} // namespace fv::dictation
