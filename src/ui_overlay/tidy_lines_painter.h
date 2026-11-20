#pragma once

class QPainter;

namespace fv {

// Cleaning up and Rewriting (ui_qt/overlays/tidy_lines.py): three lines of word-shaped strokes
// wobble out of place; a purple caret runs along them, and behind it each word settles onto its
// line at its full length and turns from faint to ink. Once the caret is through, the lines
// loosen and it starts over. Drawn around (cx, cy), now seconds into the stage.
// Affinity: GUI thread.
void drawTidyLines(QPainter& painter, double cx, double cy, double now);

} // namespace fv
