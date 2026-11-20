#pragma once

#include <functional>

namespace fv::clipboard {

// Hands staged clipboard data to the OS before the paste keystroke; an empty function where
// there is nothing to do.
//
// Qt offers clipboard data with delayed rendering, so the app a paste lands in has its read
// answered by the GUI thread, which right after the paste keystroke is saving history. With a
// 90 ms save there, a Win32 reader got the text 96 ms after the transcript was ready; rendered
// before the keystroke, 6 ms. Reading a format ourselves makes Windows keep the rendered data,
// and neither bumps the sequence number nor notifies clipboard listeners. OLE readers (.NET,
// Office) still ask the GUI thread; only OleFlushClipboard frees those, and it announces a second
// clipboard change to every clipboard listener.
struct Renderer {
    // Renders CF_UNICODETEXT.
    std::function<void()> text;
    // Renders "HTML Format" (CF_HTML), which apps that take rich text (Office, browsers, mail)
    // ask for before plain text; called only when rich text was staged.
    std::function<void()> html;
};

// Only on Qt's "windows" platform; each renderer opens the clipboard without retrying (if
// another app has it open, the target just asks the GUI thread as it always did).
// Affinity: call on the GUI thread; the renderers run on the GUI thread.
std::function<void()> system_text_renderer();
std::function<void()> system_html_renderer();
inline Renderer system_renderer() {
    return Renderer{system_text_renderer(), system_html_renderer()};
}

} // namespace fv::clipboard
