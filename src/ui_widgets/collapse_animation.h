#pragma once

#include <functional>

#include <QEasingCurve>

class QObject;
class QPropertyAnimation;
class QWidget;

namespace fv {

// Keep in sync with the main window's section resize animations.
inline constexpr int kCollapseDurationMs = 300;
// SECTION_COLLAPSE_EASING: the curve the main window, the history sidebar and the export
// dialogs share with collapsible sections.
inline constexpr QEasingCurve::Type kCollapseEasing = QEasingCurve::InOutCubic;
// Qt's QWIDGETSIZE_MAX: the maximumHeight that means "no limit".
inline constexpr int kUnlimitedHeight = 16777215;

// A reusable maximumHeight animation for a section body (300 ms, InOutCubic), parented to
// parent, or to widget when none is given.
QPropertyAnimation* createMaxHeightAnimation(QWidget* widget, QObject* parent = nullptr);

// Runs anim from start to end pixels of maximumHeight, replacing any run in flight. onFinished
// is called once, when this run finishes; a run that is stopped or replaced never calls it.
void runMaxHeightAnimation(QPropertyAnimation* anim, int start, int end,
                           std::function<void()> onFinished);

} // namespace fv
