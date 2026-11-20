#include "meeting/runtime.h"

#include <format>
#include <stdexcept>
#include <utility>

#include "audio/upload_encoder.h"
#include "meeting/detail/runtime_internal.h"
#include "speech/api_models.h"

namespace fv::meeting {

bool MeetingRuntime::can_offer_speaker_upload() const {
    return deps_.approved_speaker_decoder && audio::has_upload_flac() &&
           !speech::retired(speech::local_today(clock_));
}

void MeetingRuntime::request_speaker_upload(
    std::string meeting_id, std::function<bool(const SpeakerUploadOffer&)> confirm) {
    if (!confirm || !can_offer_speaker_upload() || meeting_id.empty()) {
        return;
    }
    spawn("meeting-speaker-approval", [this, id = std::move(meeting_id),
                                       confirm = std::move(confirm)] {
        try {
            if (meeting_busy(id)) {
                throw FinalizationBusyError("Wait for this meeting's post-processing to finish.");
            }
            auto offer = describe_speaker_upload(*deps_.repository, id, deps_.meetings_root);
            if (!deps_.approved_speaker_decoder()) {
                throw std::runtime_error("Add an OpenAI API key in Settings to identify speakers.");
            }
            post_ui([this, offer = std::move(offer), confirm]() mutable {
                if (shutdown_.cancelled() || !confirm(offer) || shutdown_.cancelled()) {
                    return;
                }
                meeting_status_update.emit("Identifying speakers with OpenAI…");
                spawn("meeting-speaker-upload", [this, offer = std::move(offer)] {
                    try {
                        auto decoder = deps_.approved_speaker_decoder();
                        if (!decoder) {
                            throw std::runtime_error("The OpenAI API key is no longer available.");
                        }
                        RerunRequest request;
                        request.repository = deps_.repository;
                        request.clock = &clock_;
                        request.settings = settings_snapshot();
                        request.cancel = shutdown_.token();
                        std::shared_ptr<RuntimeEngine> engine;
                        std::shared_ptr<RuntimeArchive> archive;
                        {
                            const std::lock_guard lock(mutex_);
                            engine = engine_;
                            archive = archive_;
                        }
                        if (engine && engine->meeting_id() == offer.meeting_id) {
                            request.store = engine->store();
                        } else if (archive && archive->meeting_id() == offer.meeting_id) {
                            request.store = archive->store();
                        }
                        const auto result = rerun_approved_speakers(
                            offer, deps_.meetings_root, request,
                            {std::move(decoder), audio::encode_upload_flac},
                            [this](const std::string& line, int, int) {
                                post_ui([this, line] { meeting_status_update.emit(line); });
                            });
                        const auto message =
                            result.value("ok", false)
                                ? std::format("Updated {} saved speaker labels.",
                                              result.value("applied", 0))
                                : result.value("error", "Speaker identification could not finish.");
                        post_ui([this, message] { meeting_status_update.emit(message); });
                    } catch (const std::exception& error) {
                        const std::string message(error.what());
                        post_ui([this, message] { meeting_status_update.emit(message); });
                    }
                });
            });
        } catch (const std::exception& error) {
            const std::string message(error.what());
            post_ui([this, message] { meeting_status_update.emit(message); });
        }
    });
}

} // namespace fv::meeting
