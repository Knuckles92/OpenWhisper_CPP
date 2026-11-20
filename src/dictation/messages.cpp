#include "dictation/messages.h"

#include <format>

#include "base/identity.h"

namespace fv::dictation {

namespace {

// actions/model.py AGENT_NAMES.
std::string_view agent_name(const Json& agent) {
    if (!agent.is_string()) {
        return {};
    }
    const auto& id = agent.get_ref<const std::string&>();
    if (id == "claude_code") {
        return "Claude Code";
    }
    if (id == "codex") {
        return "Codex";
    }
    if (id == "opencode_cli") {
        return "OpenCode";
    }
    return {};
}

} // namespace

std::string start_failure_message(const std::exception& error) {
    if (dynamic_cast<const EngineNotReadyError*>(&error) != nullptr) {
        return error.what();
    }
    return std::format("Failed to process audio: {}", error.what());
}

Json RecordingAction::param(std::string_view key) const {
    if (params.is_object()) {
        const auto found = params.find(std::string(key));
        if (found != params.end()) {
            return *found;
        }
    }
    if (kind == "agent") {
        if (key == "agent") {
            return "claude_code";
        }
        if (key == "include_selection") {
            return false;
        }
    }
    return nullptr;
}

std::string RecordingAction::profile_id() const {
    constexpr std::string_view prefix = "profile:";
    return shaping.starts_with(prefix) ? shaping.substr(prefix.size()) : std::string();
}

std::string RecordingAction::transform_id() const {
    constexpr std::string_view prefix = "transform:";
    return shaping.starts_with(prefix) ? shaping.substr(prefix.size()) : std::string();
}

bool reads_selection(const RecordingAction& action) {
    const Json value = action.param("include_selection");
    return value.is_boolean() && value.get<bool>();
}

std::string recording_destination(const RecordingAction* action, JobMode mode) {
    if (mode == JobMode::Walkthrough) {
        return std::string(WALKTHROUGH_LABEL);
    }
    if (action == nullptr || action->kind != "agent") {
        return {};
    }
    const std::string_view name = agent_name(action->param("agent"));
    return name.empty() ? std::string("Agent") : std::format("Agent · {}", name);
}

std::string busy_message(std::string_view action, bool host_update_active) {
    if (host_update_active) {
        return std::format("{} is preparing an update. Try again shortly.", identity().product);
    }
    return std::format("A transcription is already in progress — wait before {}", action);
}

Listening listening_status(JobMode mode, std::optional<std::string_view> action_name,
                           std::optional<std::string_view> profile_name) {
    using appcore::OverlayState;
    if (mode == JobMode::Command) {
        return {OverlayState::CommandListening, "Command Mode · Listening..."};
    }
    if (action_name) {
        return {OverlayState::Recording, std::format("{} · Listening...", *action_name)};
    }
    if (mode == JobMode::Walkthrough) {
        return {OverlayState::Recording, std::format("{} · Recording...", WALKTHROUGH_LABEL)};
    }
    return {OverlayState::Recording,
            profile_name ? std::format("Recording · {}...", *profile_name) : "Recording..."};
}

std::string error_status(std::string_view error_message, bool refused, JobMode mode,
                         std::string_view walkthrough_kept_note) {
    std::string status =
        refused ? std::string(error_message) : std::format("Error: {}", error_message);
    if (mode != JobMode::Walkthrough) {
        return status;
    }
    return status + std::string(walkthrough_kept_note);
}

StatusNotice failed_recording_status(std::string status, FailedRecordingSave outcome,
                                     bool remote_retry_exhausted) {
    switch (outcome) {
    case FailedRecordingSave::Saved:
        status += " — recording saved in History. Select Retry transcription.";
        return {std::move(status), remote_retry_exhausted
                                       ? "Reconnect failed. Audio saved in History."
                                       : "Transcription failed. Retry the recording from History."};
    case FailedRecordingSave::AudioNotRetained:
        status += " — audio could not be retained in History; recovery copy kept";
        break;
    case FailedRecordingSave::HistoryFailed:
        status += " — History could not be saved; recovery copy kept";
        break;
    }
    return {status, status};
}

std::string preserved_audio_status(std::string status, PreservedAudio outcome,
                                   std::string_view name) {
    switch (outcome) {
    case PreservedAudio::Saved:
        status += std::format(" — audio saved in Recordings as {}", name);
        break;
    case PreservedAudio::NotRetained:
        status += " — audio retention failed; original and recovery copy kept";
        break;
    case PreservedAudio::Missing:
        break;
    case PreservedAudio::Failed:
        status += " — could not retain audio; recovery copy kept";
        break;
    }
    return status;
}

std::string walkthrough_copy_status(const WalkthroughCopy& copy) {
    const std::string count =
        std::format("{} screenshot{}", copy.shots, copy.shots == 1 ? "" : "s");
    const bool agent_task = copy.copy_format == "agent_task";
    std::string status;
    if (!copy.write_up_error.empty()) {
        std::string done;
        if (copy.auto_paste) {
            done = "as spoken; preparing to paste PDF, then instructions";
        } else if (copy.copied && agent_task) {
            done = "as spoken; paste instructions and drag the PDF";
        } else {
            done = copy.copied ? "copied as spoken" : "saved as spoken; copying it failed";
        }
        status = std::format("Write-up failed — {} {} ({}): {}", copy.name, done, count,
                             copy.write_up_error);
    } else if (copy.copied) {
        std::string destination;
        if (copy.auto_paste) {
            destination = "preparing to paste PDF, then instructions";
        } else if (agent_task) {
            destination = "paste the instructions, then drag the PDF into your chat";
        } else {
            destination =
                copy.with_text ? "paste it into your agent" : "paste it where you need it";
        }
        status = std::format("{} copied ({}) — {}", copy.name, count, destination);
    } else {
        status = std::format("{} saved ({}); copying it failed — drag it from the card", copy.name,
                             count);
    }
    if (copy.lost_shots != 0) {
        const int lost = copy.lost_shots;
        status += std::format(". {} screenshot{} couldn't be saved and {} left out", lost,
                              lost == 1 ? "" : "s", lost == 1 ? "is" : "are");
    }
    return status;
}

std::string walkthrough_missing_status(std::string_view document_error, bool copied,
                                       bool has_words) {
    std::string status =
        document_error.empty()
            ? std::string("Walkthrough finished, but its document is missing")
            : std::format("Couldn't save the walkthrough document ({})", document_error);
    if (copied) {
        status += " — its words are copied and kept in History";
    } else if (has_words) {
        status += " — its words are kept in History";
    }
    return status;
}

std::string delivery_notice(std::string_view shaping_notice,
                            std::optional<std::string_view> formatting_profile,
                            std::optional<std::string_view> cleanup_failure) {
    if (!shaping_notice.empty()) {
        return std::string(shaping_notice);
    }
    if (formatting_profile && cleanup_failure && !cleanup_failure->empty()) {
        return std::format("{} formatting failed ({}); using raw transcript", *formatting_profile,
                           *cleanup_failure);
    }
    return {};
}

} // namespace fv::dictation
