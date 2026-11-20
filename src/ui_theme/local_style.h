#pragma once

#include <QString>

class QApplication;
class QWidget;

namespace fv::ui {

// Sets a widget-local stylesheet written with @tokens and designed font sizes. The source is
// kept in the widget's unscaledStyleSheet property and resolved against the current palette
// and UI scale; a theme or scale change re-resolves it from the source, so scales never
// compound. An unknown token is logged and left in place, never thrown.
void setLocalStyleSheet(QWidget* widget, const QString& tokenSource);

// Re-resolves every widget's local sheet at scale: widgets set through setLocalStyleSheet from
// their source, and a sheet holding "font-size" or "@" that was set directly (it is not the one
// last resolved) adopted as the new source (applyUiFontScale calls this).
void resolveLocalStyleSheets(QApplication& app, double scale);

} // namespace fv::ui
