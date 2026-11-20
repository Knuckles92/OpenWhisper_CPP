#include "speech/preview_status.h"

#include <algorithm>
#include <array>

namespace fv::speech {

namespace {

// A paired host is an OpenWhisper-compatible peer, so the families it may report are Python's
// STREAMING_PREVIEW_BACKENDS in full.
constexpr std::array<std::string_view, 4> kHostPreviewFamilies{"parakeet", "nemotron",
                                                               "parakeet_mlx", "parakeet_unified"};

} // namespace

bool streams_preview(std::string_view backend_id) {
    return backend_id == "parakeet";
}

std::string preview_unavailable_reason(std::string_view backend_id, std::string_view host,
                                       std::optional<std::string> host_family) {
    if (backend_id == "remote") {
        if (!host_family ||
            std::ranges::find(kHostPreviewFamilies, *host_family) != kHostPreviewFamilies.end()) {
            return "";
        }
        std::string status(kRemotePreviewUnavailableStatus);
        const std::string_view name = host.empty() ? "the paired computer" : host;
        status.replace(status.find("{host}"), 6, name);
        return status;
    }
    if (streams_preview(backend_id)) {
        return "";
    }
    return std::string(kPreviewUnavailableStatus);
}

} // namespace fv::speech
