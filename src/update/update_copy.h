#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "update/check.h"

// The update dialog's words, decided from a check result (ui_qt/dialogs/app_update_dialog.py:
// _title_text ... _progress_detail_text and _PHASE_TEXT). `result` is nullptr when the check
// failed; the dialog widget only shows what these return.
namespace fv::update {

// "Could not check for updates", "FioriVoce 0.2.0 is available" (" beta" before "is" for a
// prerelease), "This is a development build" or "FioriVoce is up to date".
std::string title_text(const UpdateCheckResult* result);

// The failure (or "The update server could not be reached."), else "You have <version>
// (<channel label>)." and, two spaces on, the download size, how this copy compares, or the
// latest release.
std::string body_text(const UpdateCheckResult* result, std::string_view error,
                      std::string_view platform);
// The same for the platform the result was decided for (UpdateCheckResult::platform), this
// build's when it names none.
std::string body_text(const UpdateCheckResult* result, std::string_view error);

// Bytes of the asset the apply mode downloads; 0 for notify-only.
std::int64_t download_size_bytes(const UpdateCheckResult* result);

// Extra guidance under the notes. No Windows apply mode has any, so it is always "" here (the
// macOS and git-checkout hints have no FioriVoce counterpart).
std::string hint_text(const UpdateCheckResult* result);

// "Open releases page" after a failure, "Download and install" when the release can be
// applied, "Open release notes" otherwise, "" (no primary button) when nothing is new.
std::string primary_label(const UpdateCheckResult* result);
// "Later" while an update is offered, else "Close".
std::string later_label(const UpdateCheckResult* result);

// "<done> of <total>" while downloading with a known size, else "FioriVoce <version>", else "".
std::string progress_detail_text(const UpdateCheckResult* result, DownloadPhase phase,
                                 std::int64_t done, std::int64_t total);

// The body line shown during a phase; nullopt for Opening, which keeps the previous line.
std::optional<std::string> phase_text(DownloadPhase phase);

} // namespace fv::update
