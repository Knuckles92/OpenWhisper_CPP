#pragma once

#include <QComboBox>
#include <QDateEdit>
#include <QSize>
#include <QSpinBox>

class QToolButton;

namespace fv {

// Widgets that ignore mouse-wheel value changes unless focused, so scrolling a parent page never
// changes a setting by accident.

// QComboBox that only scrolls its items when it has keyboard focus. Affinity: GUI thread.
class NoWheelComboBox : public QComboBox {
    Q_OBJECT
public:
    explicit NoWheelComboBox(QWidget* parent = nullptr);

protected:
    void wheelEvent(QWheelEvent* event) override;
};

// Combo whose width floor ignores how long its item text happens to be. A non-editable
// QComboBox reports the widest item as its minimum width and uses a Minimum horizontal policy,
// so one long model id or endpoint name raises the enclosing window's minimum width. These
// combos hold catalog values of unbounded length, so the floor is capped here and the
// closed-state text is elided rather than clipped mid-character. The popup still shows every
// item in full.
class ElidingComboBox : public NoWheelComboBox {
    Q_OBJECT
public:
    // Enough for a short value plus the arrow; long values elide.
    static constexpr int kMinimumWidth = 170;

    explicit ElidingComboBox(QWidget* parent = nullptr);

    QSize minimumSizeHint() const override;

    // Widens the popup to its longest item. Qt sizes it to the closed combo and middle-elides
    // rows that do not fit, so in a narrow field two long values became indistinguishable; Qt
    // still clamps the popup to the screen.
    void showPopup() override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

// QSpinBox that only changes value on the wheel when it has keyboard focus. A stacked chevron
// rail on the right steps the value; the native arrows stay hidden so the theme cannot draw
// them as two-border corner marks. Call setRange and the other setters through NoWheelSpinBox,
// not QSpinBox, so the rail follows. Affinity: GUI thread.
class NoWheelSpinBox : public QSpinBox {
    Q_OBJECT
public:
    static constexpr int kStepperWidth = 22;
    static constexpr int kStepperGap = 4;
    static constexpr int kIconSize = 14;

    explicit NoWheelSpinBox(QWidget* parent = nullptr);

    QToolButton* upButton() const { return upButton_; }
    QToolButton* downButton() const { return downButton_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    void setSpecialValueText(const QString& text);
    void setMinimum(int min);
    void setMaximum(int max);
    void setRange(int min, int max);
    void setWrapping(bool wrapping);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    QToolButton* makeStepButton(const char* objectName, const QString& icon,
                                const QString& tooltip);
    void step(int steps);
    void syncStepButtons();
    void layoutStepper();

    QWidget* stepper_;
    QToolButton* upButton_;
    QToolButton* downButton_;
};

// QDateEdit that only changes the date on the wheel when it has keyboard focus.
class NoWheelDateEdit : public QDateEdit {
    Q_OBJECT
public:
    explicit NoWheelDateEdit(QWidget* parent = nullptr);

protected:
    void wheelEvent(QWheelEvent* event) override;
};

} // namespace fv
