#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <QElapsedTimer>
#include <QLabel>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "ui_widgets/no_wheel.h"

class QComboBox;

namespace fv {

// Building blocks for the engine card's field row. The card lays labeled fields across its width,
// so each one has to survive being narrow: combos elide instead of reporting their longest item
// as a minimum width, which is what lets Device sit in a quarter of the row.

// A non-editable QComboBox reports its widest item as a minimum width, which would let one long
// model id set the whole window's floor.
inline constexpr int kFieldMinimumWidth = 96;

inline constexpr int kDotDiameter = 8;

// What the status dots report about the engine. Unknown exists because nothing has loaded yet
// when the window first opens, and the API backends have no local engine to report on: a green
// dot in either case would be a guess rather than a reading.
enum class EngineStatus { Ready, Attention, Unknown };

// Engine readiness, with an orbiting blue tail while work is in flight. Affinity: GUI thread.
class StatusDot : public QLabel {
    Q_OBJECT
public:
    explicit StatusDot(QWidget* parent = nullptr, int diameter = kDotDiameter);

    EngineStatus status() const { return status_; }
    void setStatus(EngineStatus status);
    void setBusy(bool busy);
    // Whether the 33 ms repaint timer is running: it runs only while busy and shown.
    bool isAnimating() const { return timer_.isActive(); }

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void syncAnimation();

    EngineStatus status_ = EngineStatus::Unknown;
    bool busy_ = false;
    QElapsedTimer clock_;
    QTimer timer_;
};

// A field combo carrying a status dot inside its left padding. The dot describes the engine
// behind the current value, not the values themselves, so it cannot be an item icon: Qt would
// repeat it down the popup as though every backend had its own status. Affinity: GUI thread.
class StatusFieldCombo : public ElidingComboBox {
    Q_OBJECT
public:
    // Matches the left padding reserved for the dot in the theme.
    static constexpr int kDotLeft = 10;

    explicit StatusFieldCombo(QWidget* parent = nullptr);

    StatusDot* dot() const { return dot_; }
    void setStatus(EngineStatus status);
    // Shows or hides the dot, reclaiming its indent when hidden. The indent is a theme property
    // rather than a layout, so it has to be switched explicitly or the value stays pushed right
    // off an absent dot.
    void setStatusVisible(bool visible);

    // kFieldMinimumWidth, as the engine fields set it on every combo.
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void positionDot();

    StatusDot* dot_;
};

// A recessed combo for one field of the engine card, named engineField (engineFieldPrimary for
// the Backend field, a StatusFieldCombo that carries the status dot and heavier text because it
// governs the other fields). Pass a primary combo's result to static_cast<StatusFieldCombo*>.
QComboBox* engineCombo(const QStringList& items, bool primary = false);

// What Device fields show for each saved device. "cuda" is saved for the GPU whatever runs it:
// CUDA, Vulkan or the Apple GPU.
QString deviceLabel(const QString& device);

// Replaces a Device field's choices: each shows its label and keeps the saved device as data.
void setDeviceChoices(QComboBox* combo, const QStringList& devices);

// A help link of a field caption: the label of the link and the Settings destination it opens.
using FieldHelpLink = std::pair<QString, QString>;

// Stacks a dim caption above field as one column of the row. With help text the caption is a
// FieldHelp (a button that opens a card on hover or Space and Enter); its links call onHelp with
// their destination.
QWidget* engineField(const QString& caption, QWidget* field, const QString& helpText = {},
                     const std::vector<FieldHelpLink>& links = {},
                     std::function<void(const QString&)> onHelp = {});

} // namespace fv
