#pragma once

#include <optional>
#include <string>
#include <string_view>

// Why the dictation preview cannot run on an engine (services/runtime/streaming.py 29-50).
// Affinity: thread-safe (pure functions).
namespace fv::speech {

// Lead decision: FioriVoce has one previewing engine, so the copy names only Parakeet.
inline constexpr std::string_view kPreviewUnavailableStatus = "Live preview needs Parakeet";
// {host} is the paired computer's name.
inline constexpr std::string_view kRemotePreviewUnavailableStatus =
    "Live preview needs Parakeet on {host}";

// The engines whose windows the preview decodes (STREAMING_PREVIEW_BACKENDS without the dropped
// ones).
bool streams_preview(std::string_view backend_id);

// Why the preview cannot run on `backend_id` (a selected_model value: "parakeet", "api",
// "remote"), or "" when it can. The Remote engine previews with whatever the paired host runs, so
// `host_family` (the engine family the host reports) decides: until it connects that is unknown
// (nullopt) and "" is returned, as setup checks again once it is. A host family OpenWhisper's
// peers preview with ("parakeet", "nemotron", "parakeet_mlx", "parakeet_unified") previews;
// any other gives kRemotePreviewUnavailableStatus for `host`, or "the paired computer" when
// `host` is empty.
std::string preview_unavailable_reason(std::string_view backend_id, std::string_view host = {},
                                       std::optional<std::string> host_family = {});

} // namespace fv::speech
