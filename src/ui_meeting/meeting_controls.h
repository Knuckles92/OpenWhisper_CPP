#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "base/signal.h"
#include "meeting/cloud_pass.h"
#include "meeting/engine_options.h"
#include "meeting_agent/route.h"

namespace fv {

namespace meeting {
class MeetingRuntime;
}

// The runtime's signals the presenter listens to (MeetingRuntime's members of the same names).
struct MeetingControlSignals {
    Signal<Json>& meeting_state_changed;
    Signal<std::string>& meeting_status_update;
    Signal<std::string>& status_update;
    Signal<std::string>& meeting_error;
    Signal<Json>& meeting_server_started;
    Signal<std::vector<Json>>& meeting_recovery_found;
    Signal<std::string>& meeting_recovery_scan_failed;
    Signal<std::string>& meeting_guest_link_ready;
    Signal<>& meeting_platform_ack_requested;
    Signal<>& past_meetings_refresh_requested;
    Signal<std::string>& playback_stop_requested;
};

// What the desktop presenter asks of Meeting Mode's runtime (ui_controller.py's on_meeting_*
// callbacks): one meeting::MeetingRuntime in the app, a recording fake in the tests.
// Affinity: GUI thread, except copy_past_meeting_transcript (any thread, it blocks).
class MeetingControls {
public:
    virtual ~MeetingControls() = default;

    virtual bool is_active() const = 0;
    virtual bool is_claimed() const = 0;
    virtual void start_meeting(std::optional<bool> cloud, std::string system_audio_policy,
                               std::string intent) = 0;
    virtual void start_demo_meeting(std::optional<bool> cloud, std::string system_audio_policy) = 0;
    virtual void toggle_from_hotkey() = 0;
    virtual std::optional<std::pair<std::string, meeting_agent::MeetingAgentRoute>>
    live_intelligence_destination() const = 0;
    virtual void set_live_intelligence(const std::string& meeting_id,
                                       std::optional<meeting::LiveIntelligenceApproval>) = 0;
    virtual void set_start_intelligence_approval(meeting::LiveIntelligenceApproval) = 0;
    virtual void set_consent_gate(std::function<bool(bool, std::function<void(bool)>)> gate) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void end() = 0;
    virtual void open_dashboard() = 0;
    virtual void open_report() = 0;
    virtual void copy_guest_link() = 0;
    virtual void retry_insights() = 0;
    virtual void retry_speakers() = 0;
    virtual void retry_step(std::string step_id) = 0;
    virtual void continue_in_background() = 0;
    virtual void defer_finalization_card() = 0;
    virtual void show_past_meeting(std::string meeting_id) = 0;
    virtual std::string copy_past_meeting_transcript(std::string meeting_id) = 0;
    virtual void delete_past_meeting(std::string meeting_id, bool delete_recordings) = 0;
    virtual void clear_past_meetings(bool delete_recordings) = 0;
    virtual bool can_offer_speaker_upload() const = 0;
    virtual void
    request_speaker_upload(std::string meeting_id,
                           std::function<bool(const meeting::SpeakerUploadOffer&)> confirm) = 0;
    virtual void finalize_recovered(std::string meeting_id) = 0;
    virtual void discard_recovered(std::string meeting_id) = 0;
    virtual void retry_recovery_scan() = 0;

    virtual MeetingControlSignals events() = 0;
};

// The app's controls: every call goes to `runtime`, which must outlive them.
std::shared_ptr<MeetingControls> runtimeControls(meeting::MeetingRuntime& runtime);

} // namespace fv
