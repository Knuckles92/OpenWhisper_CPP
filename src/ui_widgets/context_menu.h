#pragma once

class QMenu;
class QWidget;

namespace fv {

// A new list context menu owned by parent: the right-click menu style shared by the history and
// Past Meetings lists, roomier than the global QMenu rule of theme.qss (translucent, larger
// items, rounded frame).
QMenu* contextMenu(QWidget* parent);

} // namespace fv
