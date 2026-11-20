#pragma once

#include <QAbstractButton>

namespace fv {

// A keyboard-accessible on/off switch shared by every Settings page, painted with the current
// theme's colours (object name "settingsSwitch"). Its size follows the UI font scale.
// Affinity: GUI thread.
class SettingsSwitch : public QAbstractButton {
    Q_OBJECT
public:
    explicit SettingsSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
};

} // namespace fv
