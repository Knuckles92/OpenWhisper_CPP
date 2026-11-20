#pragma once

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/json.h"
#include "diarize/clustering.h"
#include "meeting_capture/capture_session.h"
#include "meeting_state/store.h"

// The live meeting's state store as the capture and the diarizer see it (Python hands both the
// store itself). The store is looked up on every call: it exists only once the meeting started,
// and an aborted start drops it.
// Affinity: thread-safe; the functions must outlive the adapters and must not block.
namespace fv::meeting {

using StoreLookup = std::function<std::shared_ptr<meeting_state::MeetingStateStore>()>;

// state.capture and its writes, and the engine's status and error events.
class StoreCaptureSink final : public CaptureStatusSink {
public:
    StoreCaptureSink(StoreLookup store, std::function<void(std::string note)> emit_status,
                     std::function<void(std::string code, std::string message)> emit_error);

    // dict(state.capture); null without a store.
    Json capture_state() override;
    // update_runtime_fields(capture=...); false without a store.
    bool update_capture(const Json& capture) override;
    void emit_status(std::string note) override;
    void emit_error(std::string code, std::string message) override;

private:
    StoreLookup store_;
    std::function<void(std::string)> emit_status_;
    std::function<void(std::string, std::string)> emit_error_;
};

// New speakers are provisional "Speaker N" participants (kind others_cluster) applied as
// system/diarizer; re-cluster relabels go to `relabel`, which the engine applies through the
// store.
class StoreDiarizerHost final : public diarize::DiarizerHost {
public:
    StoreDiarizerHost(StoreLookup store, std::function<void(const diarize::RelabelOps&)> relabel);

    std::expected<std::string, std::string> create_participant(std::string display_name) override;
    void relabel(const diarize::RelabelOps& segment_to_participant) override;
    std::map<std::string, std::string> participant_names() override;

private:
    StoreLookup store_;
    std::function<void(const diarize::RelabelOps&)> relabel_;
};

} // namespace fv::meeting
