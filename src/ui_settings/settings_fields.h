#pragma once

#include <QString>

class QLabel;
class QVBoxLayout;
class QWidget;

// Small layout pieces the Settings pages share: fields, captions, titles
// (ui_qt/dialogs/settings_fields.py). Search finds fields by the object names set here.
namespace fv {

class WrappedLabel;

// A control with its field label above it: a QWidget "modelManagerFieldGroup" (column, spacing
// 5) holding QLabel "textModelFieldLabel" and the control.
QWidget* settingsField(const QString& label, QWidget* widget);

// A wrapped secondary line under a control or group title (WrappedLabel "infoLabel").
WrappedLabel* settingsCaption(const QString& text);

// Adds a group heading (QLabel "settingsTileGroupTitle") to layout and returns it.
QLabel* groupTitle(QVBoxLayout* layout, const QString& text);

} // namespace fv
