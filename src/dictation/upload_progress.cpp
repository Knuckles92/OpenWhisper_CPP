#include "dictation/upload_progress.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>

#include "pyport/errors.h"
#include "pyport/number.h"

namespace fv::dictation {

namespace {

// divmod of a whole, non-negative double by 60, on its exact decimal digits, so that values past
// int64 split as Python's int does.
std::string elapsed_of_whole(double total) {
    if (total < 9.2e18) {
        const auto whole = static_cast<std::int64_t>(total);
        return std::format("{}:{:02}", whole / 60, whole % 60);
    }
    std::string minutes;
    int rem = 0;
    for (const char digit : py::format_fixed(total, 0)) {
        rem = rem * 10 + (digit - '0');
        if (!minutes.empty() || rem / 60 != 0) {
            minutes += static_cast<char>('0' + rem / 60);
        }
        rem %= 60;
    }
    return std::format("{}:{:02}", minutes.empty() ? "0" : minutes, rem);
}

} // namespace

std::string_view to_string(ProgressStage stage) {
    switch (stage) {
    case ProgressStage::Preparing:
        return "preparing";
    case ProgressStage::Splitting:
        return "splitting";
    case ProgressStage::Transcribing:
        return "transcribing";
    case ProgressStage::Cleaning:
        return "cleaning";
    case ProgressStage::Done:
        return "done";
    case ProgressStage::Failed:
        return "failed";
    case ProgressStage::Canceled:
        return "canceled";
    }
    return "preparing";
}

std::string_view stage_title(ProgressStage stage) {
    switch (stage) {
    case ProgressStage::Preparing:
        return "Preparing audio";
    case ProgressStage::Splitting:
        return "Splitting large file";
    case ProgressStage::Transcribing:
        return "Transcribing";
    case ProgressStage::Cleaning:
        return "Cleaning up";
    case ProgressStage::Done:
        return "Done";
    case ProgressStage::Failed:
        return "Failed";
    case ProgressStage::Canceled:
        return "Canceled";
    }
    return "Preparing audio";
}

bool is_terminal(ProgressStage stage) {
    return stage == ProgressStage::Done || stage == ProgressStage::Failed ||
           stage == ProgressStage::Canceled;
}

std::optional<ProgressStage> stage_for_overlay_state(appcore::OverlayState state) {
    switch (state) {
    case appcore::OverlayState::Processing:
        return ProgressStage::Preparing;
    case appcore::OverlayState::Transcribing:
        return ProgressStage::Transcribing;
    case appcore::OverlayState::Cleaning:
    case appcore::OverlayState::Rewriting:
        return ProgressStage::Cleaning;
    case appcore::OverlayState::Canceling:
        return ProgressStage::Canceled;
    default:
        return std::nullopt;
    }
}

std::string format_elapsed(double seconds) {
    if (std::isnan(seconds)) {
        throw py::ValueError("cannot convert float NaN to integer");
    }
    if (std::isinf(seconds)) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    // max(0, int(seconds)): int() truncates toward zero.
    return elapsed_of_whole(std::max(0.0, std::trunc(seconds)));
}

std::string_view to_string(StepState state) {
    switch (state) {
    case StepState::Pending:
        return "pending";
    case StepState::Active:
        return "active";
    case StepState::Done:
        return "done";
    case StepState::Failed:
        return "failed";
    }
    return "pending";
}

StepStates step_states(ProgressStage stage, const StepStates& current) {
    StepStates next = current;
    if (stage == ProgressStage::Done) {
        next.fill(StepState::Done);
        return next;
    }
    int active = -1;
    switch (stage) {
    case ProgressStage::Preparing:
    case ProgressStage::Splitting:
        active = 0;
        break;
    case ProgressStage::Transcribing:
        active = 1;
        break;
    case ProgressStage::Cleaning:
        active = 2;
        break;
    default:
        break;
    }
    if (active < 0) {
        for (StepState& state : next) {
            if (state == StepState::Active) {
                state = StepState::Failed;
            }
        }
        return next;
    }
    for (int index = 0; index < static_cast<int>(next.size()); ++index) {
        next[static_cast<std::size_t>(index)] = index < active    ? StepState::Done
                                                : index == active ? StepState::Active
                                                                  : StepState::Pending;
    }
    return next;
}

int visible_steps(bool with_cleanup) {
    return with_cleanup ? 3 : 2;
}

double batch_stage_weight(ProgressStage stage) {
    switch (stage) {
    case ProgressStage::Preparing:
        return 0.05;
    case ProgressStage::Splitting:
        return 0.1;
    case ProgressStage::Transcribing:
        return 0.2;
    case ProgressStage::Cleaning:
        return 0.85;
    default:
        return 0.0;
    }
}

BatchPosition clamp_batch_position(int position, int total) {
    const int clamped_total = std::max(1, total);
    return {std::max(1, std::min(position, clamped_total)), clamped_total};
}

double batch_fraction(int position, int total, double weight) {
    return (static_cast<double>(position - 1) + weight) / static_cast<double>(total);
}

} // namespace fv::dictation
