#pragma once

#include <cstddef>
#include <string>

#include "base/json.h"

// The "New in FioriVoce" note on Settings → Home (ui_qt/whats_new.py). whats_new.cpp is the
// only place the note lives. To refresh it: rewrite `text` (one or two sentences naming the
// headline features of the current release line), set `updated` to today's date (anyone who
// dismissed the previous note sees the new one once, because dismissal remembers the `updated` it
// hid), and keep `release_line` equal to the app's major.minor. Fixing a typo needs no new
// `updated`. Affinity: thread-safe (constants).
namespace fv::appcore {

// One note: what it says, and what identifies it as seen.
struct WhatsNew {
    std::string release_line; // major.minor of the releases the text describes
    std::string updated;      // ISO date the text was last rewritten; saved on dismissal
    std::string text;
};

// WHATS_NEW. The text names both products, so it is built from identity() and
// legacy_identity() rather than spelled here.
const WhatsNew& whats_new();

// Longest text the Home tile shows without crowding the cards below it, in code points.
inline constexpr std::size_t kWhatsNewMaxTextLength = 280;

// is_seen: whether the stored whats_new_seen value dismissed the current note (Python ==, so
// only the same string counts).
bool is_seen(const Json& seen);

} // namespace fv::appcore
