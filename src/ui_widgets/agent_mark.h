#pragma once

#include <QColor>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QWidget>

namespace fv {

// Painting helpers shared by the agent marks, tiles and pills (ui_qt/widgets/agent_picker.py
// _clamp, _ease_out, _ease_in_out, _mix, _alpha, _dark, _ring_color).
namespace agent_paint {
// value held to 0..1.
double clamp(double value);
// 1 - (1 - t)^3, t clamped.
double easeOut(double t);
// Cubic in-out, t clamped.
double easeInOut(double t);
// start blended toward end by t (clamped), each channel and the alpha rounded half to even.
QColor mix(const QColor& start, const QColor& end, double t);
// color with its alpha set to `alpha` (clamped).
QColor alpha(const QColor& color, double alpha);
// Whether the current palette is the dark one.
bool dark();
// The accent nudged so a 2 px ring holds against the card: lighter(118) on dark, darker(112) on
// light.
QColor ringColor(const QColor& accent);
} // namespace agent_paint

// The rounded monogram square, tinted with an agent's colour (object name "agentMark"): a soft
// tint of the accent with the monogram in that colour, or `pixmap` clipped to the rounded square.
// `warmth` and `missing` still track the card's state for callers, but every state paints the
// same, so all agent tiles match: whether an agent is installed or chosen is the card's badge
// and ring, not the mark. Its side is twice the font's height, at least 24 px, and follows font
// and style changes. Transparent for the mouse.
// Affinity: GUI thread.
class AgentMark : public QWidget {
    Q_OBJECT
public:
    AgentMark(const QString& monogram, const QColor& accent, const QPixmap& pixmap = {},
              QWidget* parent = nullptr);

    double warmth() const { return warmth_; }
    double missing() const { return missing_; }
    const QString& monogram() const { return monogram_; }
    const QColor& accent() const { return accent_; }

    void setMonogram(const QString& monogram);
    // 0 to 1, clamped.
    void setWarmth(double value);
    // Toward the empty-slot look, 0 to 1, clamped.
    void setMissing(double value);

    int side() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QString monogram_;
    QColor accent_;
    QPixmap pixmap_;
    double warmth_ = 0.0;
    double missing_ = 0.0;
};

} // namespace fv
