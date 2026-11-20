// Shared precompiled header of every Qt target (fv_pch_qt and its variants): the core set plus
// the Qt modules the reusing targets link, among QtCore, QtGui and QtWidgets.
#pragma once

#include "core.h"

#include <QtCore>
#if defined(QT_GUI_LIB)
#include <QtGui>
#endif
#if defined(QT_WIDGETS_LIB)
#include <QtWidgets>
#endif
