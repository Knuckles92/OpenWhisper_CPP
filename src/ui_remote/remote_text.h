#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "net/tailscale.h"
#include "remote/control.h"
#include "remote/record_sync_control.h"

// What Settings → Remote engine says, as ui_qt/dialogs/settings_remote.py words it: the pure
// helpers of that module, Qt-free and UTF-8 (the Host Mode dashboard reuses paired_on and
// stored_phrase). Product names come from identity().
// Affinity: thread-safe (pure functions).
namespace fv::remote_text {

// The engine's MODEL_CHOICES label "Use for dictation" selects.
inline constexpr std::string_view REMOTE_ENGINE_DISPLAY = "Remote computer";
inline constexpr std::string_view TAILSCALE_DOWNLOAD_URL = "https://tailscale.com/download";
// Showing the page again within this long reuses the last search, in seconds.
inline constexpr double TAILNET_RESCAN_S = 20.0;
// Quiet time after the last port change before sharing restarts on it, in milliseconds.
inline constexpr int PORT_SETTLE_MS = 600;

// _paired_on: "Jan 5, 2025" in `zone` (astimezone(); a naive time is taken as local already), or
// "" when the text is not an ISO timestamp. A value that is not a string is "" (TypeError).
std::string paired_on(std::string_view iso, const std::chrono::time_zone& zone);
std::string paired_on(const Json& iso, const std::chrono::time_zone& zone);
inline std::string paired_on(const std::string& iso, const std::chrono::time_zone& zone) {
    return paired_on(std::string_view(iso), zone);
}
inline std::string paired_on(const char* iso, const std::chrono::time_zone& zone) {
    return paired_on(std::string_view(iso), zone);
}

// _stored_phrase: "12 dictations and 3 meetings (1.2 GB)", without the size when it is 0, or ""
// when there are none. `stored` is {"dictation": {"count", "bytes"}, "meeting": {...}}; a value
// that is not a dict counts as none. py::ValueError when a count or size is text int() refuses.
std::string stored_phrase(const Json& stored);
std::string stored_phrase(const std::map<std::string, remote::StoredCount>& stored);

// _records_location_description for "local", "host" or "both"; std::invalid_argument otherwise.
std::string records_location_description(std::string_view location, std::string_view host);

// _records_host_problem: why the host won't take this computer's records yet, or "".
std::string records_host_problem(const remote::SyncStatus& status, std::string_view host);

// _sharing_address: where other computers reach this one, for the sharing status line.
// `state.address` is the bare address the host found (else its name is used).
std::string sharing_address(const remote::HostState& state,
                            const std::optional<net::TailscaleStatus>& tailnet, bool on_tailnet);

// _sharing_status: the error, the sharing line, "Starting..." or "".
std::string sharing_status(const remote::HostState& state, std::string_view where);

// _engine_phrase: "Whisper turbo on cuda (int8_float32)", "Parakeet v3 on cpu", the label alone
// without a device, or "" without a label.
std::string engine_phrase(const Json& engine);

// The text of a found computer's row (_find_row): its name, what it serves, and whose it is,
// joined with " · ".
std::string find_row_detail(const remote::NearbyHost& host);
// Whether its row offers "Connect" (pairing without typing a code) rather than "Use a code".
bool find_row_connects(const remote::NearbyHost& host);

// What the pair-request dialog says Allow grants (pair_request_dialog.py): "Once allowed, it can
// dictate and transcribe with the engine selected here[. It can also a, b, and c]. You can remove
// it any time under Settings → Remote engine."
std::string pairing_grants_phrase(const std::vector<std::string>& grants);

} // namespace fv::remote_text
