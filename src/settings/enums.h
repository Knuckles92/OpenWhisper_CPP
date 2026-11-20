#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "settings/setting.h"

// The value classes of services/settings.py (RecordingRetentionMode .. HuggingFaceAccessPolicy)
// with what Settings shows for them, read from settings/data/enums.json, which
// tools/gen/settings_keys.py writes from the Python source.
namespace fv::enums {

enum class EnumId {
    RecordingRetentionMode,
    RecordingTriggerMode,
    UiFontScale,
    UiTheme,
    WalkthroughCopyFormat,
    WalkthroughSnapMode,
    WalkthroughPageSize,
    RecordingOverlayLook,
    StreamingOverlayLayout,
    TranscriptCleanupProvider,
    TranscriptCleanupLevel,
    TranscriptCleanupModelSort,
    TranscriptCleanupReasoning,
    MeetingAgentCore,
    TypeSafeProvider,
    TypeSafeTextModel,
    MeetingSpeakerIdBackend,
    MeetingLanguage,
    MeetingServerBind,
    HuggingFaceAccessPolicy,
};

inline constexpr std::array kAllEnums{
    EnumId::RecordingRetentionMode,
    EnumId::RecordingTriggerMode,
    EnumId::UiFontScale,
    EnumId::UiTheme,
    EnumId::WalkthroughCopyFormat,
    EnumId::WalkthroughSnapMode,
    EnumId::WalkthroughPageSize,
    EnumId::RecordingOverlayLook,
    EnumId::StreamingOverlayLayout,
    EnumId::TranscriptCleanupProvider,
    EnumId::TranscriptCleanupLevel,
    EnumId::TranscriptCleanupModelSort,
    EnumId::TranscriptCleanupReasoning,
    EnumId::MeetingAgentCore,
    EnumId::TypeSafeProvider,
    EnumId::TypeSafeTextModel,
    EnumId::MeetingSpeakerIdBackend,
    EnumId::MeetingLanguage,
    EnumId::MeetingServerBind,
    EnumId::HuggingFaceAccessPolicy,
};

// A value (a string, or an int for UiFontScale) and the text shown for it.
struct Entry {
    Json value;
    std::string text;
};

// The Python class name ("UiTheme").
std::string_view name(EnumId);
// ALL, in order: a JSON array of strings (ints for UiFontScale).
const Json& values(EnumId);
// LABELS (or MeetingLanguage.CHOICES), in order; empty for a class without labels.
const std::vector<Entry>& labels(EnumId);
// DETAILS (RecordingOverlayLook, StreamingOverlayLayout), in order.
const std::vector<Entry>& details(EnumId);
// ALIASES: a name an earlier build saved, and its successor (RecordingOverlayLook).
const std::vector<std::pair<std::string, std::string>>& aliases(EnumId);
// INSTALLED: the installed-agent ids (TranscriptCleanupProvider, MeetingAgentCore).
const Json& installed(EnumId);
// Members outside ALL that a stored value may still hold (MeetingAgentCore.DIRECT, "direct").
const Json& legacy(EnumId);
// The label of `value`, when the class has one for it.
std::optional<std::string> label(EnumId, const Json& value);

// Module constants of services/settings.py.
inline constexpr std::int64_t kMcpDefaultPort = 8767;         // MCP_DEFAULT_PORT
inline constexpr std::int64_t kRemoteHostDefaultPort = 47821; // REMOTE_HOST_DEFAULT_PORT
inline constexpr std::array<std::int64_t, 6> kWalkthroughCaptureDelays{0, 1, 2, 3, 5, 10};
inline constexpr std::int64_t kMeetingLinuxPreviewAckVersion = 1;
inline constexpr std::string_view kInsightReviewConsent = "typesafe-text-v1";
// DEFAULT_VOICE_COMMAND_NAMES, as Python has them (they include OpenWhisper's own name).
const std::vector<std::string>& default_voice_command_names();
// enums.json's "constants" object, by Python name.
const Json& constants();

} // namespace fv::enums
