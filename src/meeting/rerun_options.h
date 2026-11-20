#pragma once

#include <functional>
#include <optional>
#include <string>

#include "base/clock.h"
#include "base/json.h"
#include "meeting/finalization.h"

// Settings for re-running a past meeting's post-meeting steps (services/meeting_rerun.py). The
// desktop Retry, the dashboard's re-run and crash recovery resume the same pipeline, so they
// resolve the ASR language and model and the speaker-pass gate here rather than each picking
// their own. The agent route (provider, model, endpoint, agent core) joins with the meeting
// intelligence; the OpenAI key is never read: FioriVoce's speaker backend is never "openai".
namespace fv::meeting {

// meeting/reinsight.py DEFAULT_TIMEOUT_S: the budget of a re-run's agent passes, in seconds.
inline constexpr double kRerunTimeoutS = 900.0;

struct RerunOptions {
    double timeout_s = kRerunTimeoutS;
    // The meeting's recorded ASR model, else the current meeting speech model.
    std::string asr_model_name;
    // The paired computer's route language ("auto" when it has none) for a meeting transcribed
    // remotely, else the meeting language setting.
    std::string language;
    // Speaker settings are always current: consent withdrawn since the meeting must stop a
    // retry from uploading its audio.
    std::string speaker_id_backend;
    bool speaker_audio_consent = false;
    bool redecode_coverage_guard = false;

    bool operator==(const RerunOptions&) const = default;
};

// The remote speech route a meeting row recorded (meeting/asr/remote.py saved_remote_route):
// nullopt for local speech. Throws the route's own error for an invalid one.
using SavedRemoteRoute = std::function<std::optional<Json>(const Json& meeting)>;

// Whether saved settings allow uploading system audio to OpenAI now. `settings` is a settings
// snapshot; the date is `clock`'s local one.
SpeakerPassGate resolve_speaker_pass(const Json& settings, const Clock& clock);

// The options rerun_finalization takes for `meeting` (the stored row): the row's recorded ASR
// model wins, current settings fill the rest. An empty `remote` reads every meeting as local.
RerunOptions rerun_options(const Json& meeting, const Json& settings, const Clock& clock,
                           const SavedRemoteRoute& remote = {});

} // namespace fv::meeting
