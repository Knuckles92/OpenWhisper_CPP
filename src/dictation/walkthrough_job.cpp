#include "dictation/walkthrough_job.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "appcore/activity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "dictation/job.h"
#include "dictation/messages.h"
#include "dictation/recognition.h"
#include "dictation/transcription_runtime.h"
#include "focus/capture_service.h"
#include "pyport/errors.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "walkthrough/files.h"
#include "walkthrough/finish.h"
#include "walkthrough/library.h"
#include "walkthrough/sections.h"
#include "walkthrough/write_up.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.runtime.transcription"};

// wave.Error: the recording a recovery would decode is not a WAV Python's wave module reads.
class WaveHeaderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::uint32_t le32(const char* bytes) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = value << 8 | static_cast<unsigned char>(bytes[i]);
    }
    return value;
}

std::uint16_t le16(const char* bytes) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]) |
                                      static_cast<unsigned char>(bytes[1]) << 8);
}

// wave.open(path).getnframes() / getframerate(): from the header alone, as Python reads it (the
// data chunk's declared size, in whole frames), so a long recording is never read here.
double wave_duration(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::filesystem::filesystem_error(
            "Could not open the walkthrough recording", path,
            std::make_error_code(std::errc::no_such_file_or_directory));
    }
    std::array<char, 12> riff{};
    if (!file.read(riff.data(), riff.size()) || std::memcmp(riff.data(), "RIFF", 4) != 0) {
        throw WaveHeaderError("file does not start with RIFF id");
    }
    if (std::memcmp(riff.data() + 8, "WAVE", 4) != 0) {
        throw WaveHeaderError("not a WAVE file");
    }
    bool have_format = false;
    int channels = 0;
    int width = 0;
    std::uint32_t rate = 0;
    std::optional<std::uint32_t> data_size;
    std::array<char, 8> header{};
    while (file.read(header.data(), header.size())) {
        const std::uint32_t size = le32(header.data() + 4);
        if (std::memcmp(header.data(), "fmt ", 4) == 0) {
            std::array<char, 40> format{};
            const std::uint32_t wanted = std::min(size, static_cast<std::uint32_t>(format.size()));
            if (size < 14 || !file.read(format.data(), wanted)) {
                throw WaveHeaderError("fmt chunk and/or data chunk missing");
            }
            const std::uint16_t tag = le16(format.data());
            channels = le16(format.data() + 2);
            rate = le32(format.data() + 4);
            const int bits = size >= 16 ? le16(format.data() + 14) : 0;
            // WAVE_FORMAT_PCM, or WAVE_FORMAT_EXTENSIBLE whose sub-format is PCM.
            if (tag != 1 && tag != 0xFFFE) {
                throw WaveHeaderError(std::format("unknown format: {}", tag));
            }
            if (tag == 0xFFFE) {
                if (wanted < 40) {
                    throw WaveHeaderError("fmt chunk and/or data chunk missing");
                }
                // KSDATAFORMAT_SUBTYPE_PCM as stored (uuid bytes_le).
                static constexpr std::array<unsigned char, 16> kPcm{
                    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                const char* sub = format.data() + 24;
                if (std::memcmp(sub, kPcm.data(), kPcm.size()) != 0) {
                    const auto byte = [sub](int i) {
                        return static_cast<unsigned char>(sub[i]);
                    };
                    throw WaveHeaderError(
                        std::format("unknown extended format: {:08x}-{:04x}-{:04x}-{:02x}{:02x}-"
                                    "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                                    le32(sub), le16(sub + 4), le16(sub + 6), byte(8), byte(9),
                                    byte(10), byte(11), byte(12), byte(13), byte(14), byte(15)));
                }
            }
            width = (bits + 7) / 8;
            if (width == 0) {
                throw WaveHeaderError("bad sample width");
            }
            if (channels == 0) {
                throw WaveHeaderError("bad # of channels");
            }
            have_format = true;
            file.seekg(static_cast<std::streamoff>(size - wanted + (size & 1U)), std::ios::cur);
            continue;
        }
        if (std::memcmp(header.data(), "data", 4) == 0) {
            if (!have_format) {
                throw WaveHeaderError("data chunk before fmt chunk");
            }
            data_size = size;
            break;
        }
        file.seekg(static_cast<std::streamoff>(size + (size & 1U)), std::ios::cur);
    }
    if (!have_format || !data_size) {
        throw WaveHeaderError("fmt chunk and/or data chunk missing");
    }
    if (rate == 0) {
        throw WaveHeaderError("bad sample rate");
    }
    const auto frames = *data_size / static_cast<std::uint32_t>(channels * width);
    return static_cast<double>(frames) / rate;
}

// The exceptions recover_walkthrough reports (Python's OSError, ValueError, KeyError, TypeError
// and wave.Error); any other is a fault and propagates.
bool expected_recovery_error(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        const std::string_view type = python->type_name();
        return type != "OverflowError" && type != "RecursionError" && type != "IndexError";
    }
    return dynamic_cast<const std::system_error*>(&error) != nullptr ||
           dynamic_cast<const std::invalid_argument*>(&error) != nullptr ||
           dynamic_cast<const walkthrough::WaveError*>(&error) != nullptr ||
           dynamic_cast<const WaveHeaderError*>(&error) != nullptr;
}

} // namespace

walkthrough::WalkthroughResult write_up_walkthrough(const WalkthroughWriteUpRequest& request) {
    return walkthrough::write_up::write_up(request.result, request.shots, request.agent,
                                           request.instructions, request.follow_notes,
                                           request.notes, request.settings, request.cancel);
}

// --- the session ------------------------------------------------------------------------

bool TranscriptionRuntime::start_walkthrough(
    std::shared_ptr<walkthrough::WalkthroughSession> session, std::string source) {
    StartOptions options;
    options.mode = JobMode::Walkthrough;
    options.source = std::move(source);
    if (!start_recording(std::move(options))) {
        return false;
    }
    {
        std::lock_guard lock(state_mutex_);
        walkthrough_ = session;
    }
    const std::string journal = recorder_.journal_id();
    if (!journal.empty() && session) {
        try {
            session->checkpoint_recording(journal);
        } catch (const std::system_error& error) {
            kLog.error_with(error, "Could not create walkthrough recovery folder");
        }
    }
    return true;
}

std::shared_ptr<walkthrough::WalkthroughSession>
TranscriptionRuntime::recording_walkthrough() const {
    auto session = walkthrough_session();
    return session && session->open() ? session : nullptr;
}

std::shared_ptr<walkthrough::WalkthroughSession> TranscriptionRuntime::walkthrough_session() const {
    std::lock_guard lock(state_mutex_);
    return walkthrough_;
}

void TranscriptionRuntime::end_walkthrough(bool keep) {
    std::shared_ptr<walkthrough::WalkthroughSession> session;
    {
        std::lock_guard lock(state_mutex_);
        session = std::exchange(walkthrough_, nullptr);
    }
    if (!session) {
        return;
    }
    try {
        if (keep) {
            session->end();
        } else {
            session->discard();
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not let go of the walkthrough's files");
    }
}

std::string TranscriptionRuntime::walkthrough_kept_note() const {
    try {
        const auto session = walkthrough_session();
        return walkthrough::kept_note(session.get());
    } catch (const std::exception& error) {
        kLog.error_with(error, "Walkthrough folder unavailable for the error status");
        return {};
    }
}

std::optional<std::string> TranscriptionRuntime::readiness_message() const {
    if (!services_.hooks.readiness_message) {
        return std::nullopt;
    }
    auto message = services_.hooks.readiness_message();
    if (message && message->empty()) {
        return std::nullopt;
    }
    return message;
}

void TranscriptionRuntime::set_walkthrough_delivery(WalkthroughDeliveryPort* delivery) {
    walkthrough_delivery_ = delivery;
}

WalkthroughDeliveryPort* TranscriptionRuntime::walkthrough_delivery() const {
    return walkthrough_delivery_.load();
}

void TranscriptionRuntime::set_walkthrough_strand(Executor* strand) {
    walkthrough_strand_ = strand;
}

void TranscriptionRuntime::set_walkthrough_write_up(WalkthroughWriteUp write_up) {
    std::lock_guard lock(state_mutex_);
    write_up_ = std::move(write_up);
}

// --- recovery ---------------------------------------------------------------------------

void TranscriptionRuntime::recover_walkthrough(std::filesystem::path folder) {
    const ActivitySnapshot activity = activity_.snapshot();
    if (activity.meeting_active || recorder_.is_recording()) {
        report_busy("recovering a walkthrough");
        return;
    }
    if (writing_up() || activity.library_change) {
        status_update.emit("Wait for the walkthrough being saved to finish before recovering one");
        return;
    }
    std::shared_ptr<walkthrough::WalkthroughSession> session;
    std::filesystem::path audio;
    double duration = 0.0;
    std::int64_t size = 0;
    try {
        session = walkthrough::recovery_session(folder);
        audio = folder / base_utf8::path_from_utf8(walkthrough::AUDIO_NAME);
        duration = wave_duration(audio);
        size = static_cast<std::int64_t>(std::filesystem::file_size(audio));
    } catch (const std::exception& error) {
        if (!expected_recovery_error(error)) {
            throw;
        }
        status_update.emit(std::format("Couldn't recover the walkthrough: {}", error.what()));
        return;
    }
    // _begin_job never raises: a walkthrough reads nothing on screen.
    std::shared_ptr<const DictationJob> job;
    const Json settings = settings_snapshot();
    try {
        job = std::make_shared<const DictationJob>(begin_job(JobMode::Walkthrough, settings,
                                                             recognition_for(nullptr, settings), {},
                                                             services_.capture));
    } catch (...) {
        kLog.warning("Dictation job context unavailable; using a bare job");
        DictationJob bare;
        bare.mode = JobMode::Walkthrough;
        job = std::make_shared<const DictationJob>(std::move(bare));
    }
    if (!claim_job(job, true)) {
        report_busy("recovering a walkthrough");
        return;
    }
    forget_recording();
    {
        std::lock_guard lock(state_mutex_);
        walkthrough_ = std::move(session);
        deliver_to_clipboard_ = true;
        pending_ = PendingAudio{};
        pending_.audio_duration = duration;
        pending_.file_size = size;
        pending_.source_name = std::string(WALKTHROUGH_LABEL);
        pending_streaming_text_.clear();
    }
    overlay_state_update.emit(appcore::OverlayState::Processing);
    status_update.emit("Recovering walkthrough...");
    try {
        raise_if_canceled();
        require_backend_ready();
        post_jobs([this, audio = std::move(audio)] { transcribe_audio_file(audio); });
    } catch (const std::exception& error) {
        kLog.error_with(error, "Failed to start walkthrough recovery");
        on_transcription_error(start_failure_message(error));
    }
}

// --- delivery ---------------------------------------------------------------------------

void TranscriptionRuntime::deliver_walkthrough(const std::string& transcript) {
    const auto session = walkthrough_session();
    const auto result = session ? session->result() : std::nullopt;
    WalkthroughDeliveryPort* delivery = walkthrough_delivery();
    if (!result) {
        // The document could not be written: the words are copied as text, and History keeps
        // them.
        const std::string error = session ? session->document_error() : std::string();
        const bool copied = !transcript.empty() && delivery != nullptr &&
                            delivery->copy_files_to_clipboard({}, transcript);
        const std::string status = walkthrough_missing_status(error, copied, !transcript.empty());
        status_update.emit(status);
        notify(status);
        if (delivery != nullptr) {
            delivery->dismiss_walkthrough_panel();
        }
        return;
    }
    const Json settings = settings_snapshot();
    if (keys::walkthrough_write_up.read(settings)) {
        start_write_up(*session, settings);
        return;
    }
    copy_walkthrough(*result);
}

void TranscriptionRuntime::copy_walkthrough(const walkthrough::WalkthroughResult& result) {
    // The Agent format with auto-paste pastes the PDF, then the instructions; every other copy,
    // and the Agent format with auto-paste off, stays on the clipboard.
    const Json settings = settings_snapshot();
    const std::string copy_format = keys::walkthrough_copy_format.read(settings);
    const bool auto_paste = copy_format == "agent_task" && keys::auto_paste.read(settings);
    std::optional<focus::AppIdentity> target;
    if (auto_paste) {
        // Before the ready card shows; the paste checks it again before each clipboard paste.
        focus::CaptureService* capture = services_.capture;
        std::shared_ptr<focus::CaptureService> installed;
        if (capture == nullptr) {
            installed = focus::get_service();
            capture = installed.get();
        }
        if (capture != nullptr) {
            target = capture->current_identity();
        }
    }
    const auto [files, text] = result.clipboard(copy_format);
    WalkthroughDeliveryPort* delivery = walkthrough_delivery();
    const bool copied = delivery != nullptr && delivery->copy_files_to_clipboard(files, text);
    WalkthroughCopy copy;
    copy.name = result.name();
    copy.shots = result.shots;
    copy.write_up_error = result.write_up_error;
    copy.copy_format = copy_format;
    copy.auto_paste = auto_paste;
    copy.copied = copied;
    copy.with_text = !text.empty();
    copy.lost_shots = result.lost_shots;
    status_update.emit(walkthrough_copy_status(copy));
    if (delivery == nullptr) {
        return;
    }
    delivery->show_walkthrough_ready(result, copied);
    if (auto_paste) {
        delivery->paste_walkthrough(result, target);
    }
}

// --- the AI write-up --------------------------------------------------------------------

bool TranscriptionRuntime::writing_up() const {
    std::lock_guard lock(state_mutex_);
    return write_up_cancel_ != nullptr;
}

void TranscriptionRuntime::start_write_up(const walkthrough::WalkthroughSession& session,
                                          const Json& settings) {
    // The job ends with the as-spoken document, so dictation keeps working for the minutes the
    // agent takes. A failure or a Cancel keeps the words as spoken and says why.
    WalkthroughWriteUpRequest request;
    request.result = session.result().value_or(walkthrough::WalkthroughResult{});
    auto input = session.write_up_input();
    request.shots = std::move(input.shots);
    request.notes = std::move(input.notes);
    request.agent = keys::walkthrough_write_up_agent.read(settings);
    request.instructions = keys::walkthrough_write_up_instructions.read(settings);
    request.follow_notes = keys::walkthrough_write_up_notes.read(settings);
    request.settings = settings;
    auto cancel = std::make_shared<CancelSource>();
    request.cancel = cancel->token();
    WalkthroughWriteUp write_up;
    {
        std::lock_guard lock(state_mutex_);
        write_up_cancel_ = cancel;
        write_up = write_up_ ? write_up_ : WalkthroughWriteUp(write_up_walkthrough);
    }
    activity_.set_write_up_running(true);
    std::string agent_name;
    try {
        agent_name = walkthrough::write_up::agent_name(request.agent);
    } catch (const std::out_of_range&) {
        agent_name = request.agent;
    }
    status_update.emit(std::format("Writing it up with {}...", agent_name));
    if (activity_.snapshot().shutting_down) {
        // Quitting: the strand takes no more work, so nothing will write it up, and nothing is
        // copied once quitting (Python's submit raising after its executor shut down).
        on_walkthrough_written_up(request.result);
        return;
    }
    Executor* strand = walkthrough_strand_.load();
    std::weak_ptr<int> alive = alive_;
    (strand != nullptr ? *strand : jobs_)
        .post([this, alive, request = std::move(request), write_up = std::move(write_up)] {
            if (!alive.lock()) {
                return;
            }
            walkthrough::WalkthroughResult written;
            try {
                written = write_up(request);
            } catch (const std::exception& error) {
                // Whatever stopped it, the walkthrough as spoken is still copied.
                kLog.warning("Walkthrough write-up failed: {}", error.what());
                written = request.result;
                written.write_up_error = error.what();
                if (written.write_up_error.empty()) {
                    written.write_up_error = "failed";
                }
            }
            walkthrough_written_up.emit(written);
            post_ui([this, written = std::move(written)] { on_walkthrough_written_up(written); });
        });
}

void TranscriptionRuntime::on_walkthrough_written_up(walkthrough::WalkthroughResult result) {
    {
        std::lock_guard lock(state_mutex_);
        write_up_cancel_.reset();
    }
    activity_.set_write_up_running(false);
    if (activity_.snapshot().shutting_down) {
        return;
    }
    copy_walkthrough(result);
}

bool TranscriptionRuntime::cancel_write_up() {
    std::shared_ptr<CancelSource> cancel;
    {
        std::lock_guard lock(state_mutex_);
        cancel = write_up_cancel_;
    }
    if (!cancel) {
        return false;
    }
    if (!cancel->cancelled()) {
        cancel->cancel();
        status_update.emit("Stopping the write-up...");
    }
    return true;
}

} // namespace fv::dictation
