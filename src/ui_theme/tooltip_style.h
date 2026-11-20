#pragma once

#include <QObject>
#include <QProxyStyle>

class QApplication;

namespace fv::ui {

// Shortens the native tooltip wake-up delay (about 700 ms on Windows) so setToolTip hints
// appear as promptly as the hotkey hover pills. Everything else passes to the wrapped style.
class SnappyTooltipStyle : public QProxyStyle {
    Q_OBJECT
public:
    static constexpr int kWakeUpDelayMs = 200;

    explicit SnappyTooltipStyle(QStyle* base);

    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override;
};

// Makes Qt's tooltip windows (QTipLabel) translucent and shadowless when they are polished,
// before their native window exists, so the stylesheet's rounded corners show instead of square
// blocks. On Wayland, where that does not hold, it squares the tooltip's corners instead.
class RoundedTooltipFilter : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

// The base style the references pin (Qt 6.7 and later, Windows only).
inline constexpr char kBaseStyle[] = "windows11";

// Creates the windows11 style (or, where it does not exist, a fresh copy of the application's
// current one), wraps it in a SnappyTooltipStyle that the application then owns, and installs a
// RoundedTooltipFilter. Call it after QApplication exists, since styles need it.
void installTooltipStyle(QApplication& app);

} // namespace fv::ui
