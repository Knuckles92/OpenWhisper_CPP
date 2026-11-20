#pragma once

#include <QObject>
#include <QPalette>
#include <QString>

#include "ui_theme/palette.h"

class QApplication;

namespace fv::ui {

// Owns the stylesheet template and the palette it resolves against. The template is
// :/ui_theme/theme.qss (OpenWhisper's sheet, unchanged) followed by every other directory's
// :/<dir>/<dir>.qss in sorted order, with url(ui_qt/...) rewritten to url(:/ui_theme/...) once
// at load. Applying the result to the application is applyUiFontScale's job, which also
// re-resolves the widget-local sheets; a theme change goes through applyUiTheme. Constructing
// one makes its palette current. Affinity: UI actor.
class ThemeManager : public QObject {
    Q_OBJECT
public:
    explicit ThemeManager(const QString& theme = QString::fromLatin1(kDark),
                          QObject* parent = nullptr);

    const Palette& palette() const { return *palette_; }
    QString currentTheme() const { return palette_->qName(); }

    // The template with the palette's tokens substituted; an unknown token is logged and left.
    QString stylesheet() const;
    // stylesheet() with every font-size multiplied by scale.
    QString scaledStylesheet(double scale) const;
    // The template as loaded: url() rewritten, tokens not yet substituted. "" when theme.qss
    // could not be read.
    const QString& stylesheetTemplate() const { return template_; }

    // Switches to the built-in palette of name. Returns true, and emits themeChanged once, only
    // when the palette changed by value.
    bool setTheme(const QString& name);

    // Pushes the palette into the application's QPalette (what message boxes, calendar popups
    // and item-view selections read) and, when pinColorScheme, pins the platform colour scheme,
    // which Windows uses for the title bar.
    void applyPlatformPalette(QApplication& app, bool pinColorScheme = true);

Q_SIGNALS:
    void themeChanged(const QString& name);

private:
    void loadStylesheet();

    QString template_;
    const Palette* palette_;
};

// The QPalette of a theme: 19 roles from tokens, and the disabled WindowText, Text and
// ButtonText in text-muted.
QPalette buildQPalette(const Palette& palette);

// url(...) references that are relative (not quoted-absolute, not ":/...", not "C:...")
// rewritten to the resource tree: url(ui_qt/assets/x.svg) -> url(:/ui_theme/assets/x.svg).
QString absolutizeUrls(const QString& stylesheet);

} // namespace fv::ui
