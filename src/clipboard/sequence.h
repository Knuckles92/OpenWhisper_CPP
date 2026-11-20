#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace fv::clipboard {

// Returns the clipboard's change counter, or nullopt when it is unknown right now.
using SequenceSource = std::function<std::optional<std::uint64_t>()>;

// A reader for the OS clipboard change counter, or an empty function where there is none.
// Windows bumps GetClipboardSequenceNumber on every write by any process, while reads (even ones
// that make another app render a delayed format) leave it alone, so an unchanged number proves
// the clipboard still holds what an earlier snapshot captured. Only on Qt's "windows" platform:
// the offscreen platform (tests) keeps an in-process clipboard the Windows counter never sees.
// Elsewhere the result is empty and every paste captures the clipboard at stage time: trusting a
// stale snapshot would restore it over the user's newer copy.
// Affinity: call on the GUI thread (it reads the Qt platform name); the reader is thread-safe.
SequenceSource system_clipboard_sequence();

// Whether Qt's clipboard sees other apps' copies while this app is in the background: false on
// Wayland, where Qt then keeps a dead offer that reads back empty, so a copy sent to read the
// selection would land unseen, replacing a clipboard that could not be put back.
// Affinity: GUI thread.
bool foreign_copies_visible();

} // namespace fv::clipboard
