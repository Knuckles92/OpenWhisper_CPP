#include "clipboard/render.h"
#include "clipboard/sequence.h"

namespace fv::clipboard {

// X11 and Wayland have no clipboard change counter, so prefetching is off and every paste
// captures the clipboard at stage time. macOS's NSPasteboard.changeCount is not supported.
SequenceSource system_clipboard_sequence() {
    return {};
}

// Only Windows renders delayed clipboard formats on the reader's request.
std::function<void()> system_text_renderer() {
    return {};
}

std::function<void()> system_html_renderer() {
    return {};
}

} // namespace fv::clipboard
