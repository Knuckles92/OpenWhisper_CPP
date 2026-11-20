#include "ui_meeting/meeting_controls.h"

#include "meeting/runtime.h"

namespace fv {
namespace {

class RuntimeControls final : public MeetingControls {
public:
    explicit RuntimeControls(meeting::MeetingRuntime& runtime) : runtime_(runtime) {}

    bool is_active() const override { return runtime_.is_active(); }
    bool is_claimed() const override { return runtime_.is_claimed(); }
    void start_meeting(std::optional<bool> cloud, std::string policy, std::string intent) override {
        runtime_.start_meeting(cloud, std::move(policy), std::move(intent));
    }
    void start_demo_meeting(std::optional<bool> cloud, std::string policy) override {
        runtime_.start_demo_meeting(cloud, std::move(policy));
    }
    void toggle_from_hotkey() override { runtime_.toggle_from_hotkey(); }
    std::optional<std::pair<std::string, meeting_agent::MeetingAgentRoute>>
    live_intelligence_destination() const override {
        return runtime_.live_intelligence_destination();
    }
    void set_live_intelligence(const std::string& id,
                               std::optional<meeting::LiveIntelligenceApproval> approval) override {
        runtime_.set_live_intelligence(id, std::move(approval));
    }
    void set_start_intelligence_approval(meeting::LiveIntelligenceApproval approval) override {
        runtime_.set_start_intelligence_approval(std::move(approval));
    }
    void set_consent_gate(std::function<bool(bool, std::function<void(bool)>)> gate) override {
        runtime_.set_consent_gate(std::move(gate));
    }
    void pause() override { runtime_.pause(); }
    void resume() override { runtime_.resume(); }
    void end() override { runtime_.end(); }
    void open_dashboard() override { runtime_.open_dashboard(); }
    void open_report() override { runtime_.open_report(); }
    void copy_guest_link() override { runtime_.copy_guest_link(); }
    void retry_insights() override { runtime_.retry_insights(); }
    void retry_speakers() override { runtime_.retry_speakers(); }
    void retry_step(std::string step) override { runtime_.retry_step(std::move(step)); }
    void continue_in_background() override { runtime_.continue_in_background(); }
    void defer_finalization_card() override { runtime_.defer_finalization_card(); }
    void show_past_meeting(std::string id) override { runtime_.show_past_meeting(std::move(id)); }
    std::string copy_past_meeting_transcript(std::string id) override {
        return runtime_.copy_past_meeting_transcript(std::move(id));
    }
    void delete_past_meeting(std::string id, bool recordings) override {
        runtime_.delete_past_meeting(std::move(id), recordings);
    }
    void clear_past_meetings(bool recordings) override { runtime_.clear_past_meetings(recordings); }
    bool can_offer_speaker_upload() const override { return runtime_.can_offer_speaker_upload(); }
    void request_speaker_upload(
        std::string id, std::function<bool(const meeting::SpeakerUploadOffer&)> confirm) override {
        runtime_.request_speaker_upload(std::move(id), std::move(confirm));
    }
    void finalize_recovered(std::string id) override { runtime_.finalize_recovered(std::move(id)); }
    void discard_recovered(std::string id) override { runtime_.discard_recovered(std::move(id)); }
    void retry_recovery_scan() override { runtime_.retry_recovery_scan(); }

    MeetingControlSignals events() override {
        return {runtime_.meeting_state_changed,
                runtime_.meeting_status_update,
                runtime_.status_update,
                runtime_.meeting_error,
                runtime_.meeting_server_started,
                runtime_.meeting_recovery_found,
                runtime_.meeting_recovery_scan_failed,
                runtime_.meeting_guest_link_ready,
                runtime_.meeting_platform_ack_requested,
                runtime_.past_meetings_refresh_requested,
                runtime_.playback_stop_requested};
    }

private:
    meeting::MeetingRuntime& runtime_;
};

} // namespace

std::shared_ptr<MeetingControls> runtimeControls(meeting::MeetingRuntime& runtime) {
    return std::make_shared<RuntimeControls>(runtime);
}

} // namespace fv
