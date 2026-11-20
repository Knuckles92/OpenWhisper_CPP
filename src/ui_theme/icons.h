#pragma once

#include <QIcon>
#include <QPixmap>
#include <QString>

namespace fv::ui {

// Resource path of a bundled Tabler icon: :/ui_theme/assets/tabler/<file>.
QString tablerPath(const QString& file);

// A bundled Tabler icon, cached by file name (rendering the SVG costs about 0.2 ms and Settings
// asks for the same few dozen repeatedly). A QIcon is a shared value: do not add pixmaps to one
// returned here. Affinity: GUI thread.
QIcon tablerIcon(const QString& file);

// The icon rendered at size x size.
QPixmap tablerPixmap(const QString& file, int size);

// A Tabler icon that keeps its colour on a disabled button: Qt greys out disabled icons, which
// would erase the colour of a "current choice" button disabled because it is already chosen.
QIcon designIcon(const QString& file);

} // namespace fv::ui
