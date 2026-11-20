#pragma once

#include <functional>
#include <optional>

#include <QColor>
#include <QElapsedTimer>
#include <QPixmap>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include "ui_widgets/agent_picker_text.h"

class QLabel;
class QPainter;
class QPainterPath;
class QPushButton;
class QTimer;
class QVariantAnimation;
class QVBoxLayout;

namespace fv {

class AgentMark;

// A small rounded status label (object name "agentStatusPill"); the Found pill draws its check
// in and the warning pill an alert glyph. Its height is the font's plus 6 px. Transparent for the
// mouse.
// Affinity: GUI thread.
class StatusPill : public QWidget {
    Q_OBJECT
    // Read like a label's text, as OpenWhisper's pill offers text().
    Q_PROPERTY(QString text READ text)
public:
    static constexpr double kPad = 8.0;
    static constexpr double kGap = 4.0;

    explicit StatusPill(QWidget* parent = nullptr);

    const QString& tone() const { return tone_; }
    const QString& text() const { return text_; }
    // How far it has faded in, and how much of the check is drawn, 0 to 1.
    double appear() const { return appear_; }
    double check() const { return check_; }

    void setState(const QString& tone, const QString& text);
    // Both clamped to 0..1.
    void setProgress(double appear, double check);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool hasGlyph() const;
    double glyphSide() const;
    void paintCheck(QPainter& painter, const QRectF& box, const QColor& ink) const;
    void paintAlert(QPainter& painter, const QRectF& box, const QColor& ink) const;

    QString tone_;
    QString text_;
    double appear_ = 1.0;
    double check_ = 1.0;
};

// One choice in the agent picker (object name "agentTile"): a mark, a name, a status pill, a
// detail line and, for a missing agent, a "Get <name> ↗" link. The card is painted here rather
// than by the stylesheet so it can lift, glow in the agent's colour and shimmer while a scan
// runs. The widget is larger than the card it draws by BLEED on each side; that margin holds the
// lift and the shadow. Only a selectable tile takes clicks, Space, Return and Enter, and shows a
// focus ring, only for keyboard focus.
// Affinity: GUI thread.
class AgentTile : public QWidget {
    Q_OBJECT
public:
    static constexpr int kBleedX = 3;
    static constexpr int kBleedTop = 3;
    static constexpr int kBleedBottom = 7;
    static constexpr int kPadX = 14;
    static constexpr int kPadY = 12;
    static constexpr double kRadius = 12.0;
    static constexpr double kLiftPx = 2.0;

    AgentTile(const QString& tileId, const QString& name, const QString& monogram,
              const QColor& accent, const QPixmap& pixmap = {}, const QString& installUrl = {},
              const QString& installHint = {}, QWidget* parent = nullptr);

    const QString& tileId() const { return tileId_; }
    const QString& name() const { return name_; }
    // What the tile's agent would run ("AI insights" until the picker says otherwise); read in
    // the accessible description the next time the selection or the state changes.
    const QString& purpose() const { return purpose_; }
    void setPurpose(const QString& purpose);

    AgentMark* mark() const { return mark_; }
    StatusPill* pill() const { return pill_; }
    QLabel* nameLabel() const { return nameLabel_; }
    QLabel* detailLabel() const { return detailLabel_; }
    QPushButton* link() const { return link_; }

    const agent_picker::TileState& state() const { return state_; }
    bool isSelected() const { return selected_; }
    bool selectable() const { return state_.selectable; }
    bool scanning() const { return scanning_; }
    double shimmer() const { return shimmer_; }
    // Progress of the last result's reveal, 0 to 1.
    double arrival() const { return arrival_; }
    // True while a result waits for its turn to light up.
    bool arrivalPending() const;
    double hover() const { return hover_; }
    double lift() const { return lift_; }

    // The animations and the stagger timer, for tests that drive them to fixed times.
    QVariantAnimation* hoverAnimation() const { return hoverAnim_; }
    QVariantAnimation* liftAnimation() const { return liftAnim_; }
    QVariantAnimation* arrivalAnimation() const { return arrivalAnim_; }
    QVariantAnimation* shimmerAnimation() const { return shimmerAnim_; }
    QTimer* arrivalTimer() const { return arrivalTimer_; }

    // Shimmers from now until endScan; index is the tile's place in the row.
    void beginScan(int index);
    // One shimmer frame: moves the band and lets the mark breathe.
    void advanceScan();
    // The scan is back: lets the shimmer go, fading when animate.
    void endScan(bool animate);
    // Shows state; with animate, lights up after delayMs.
    void showState(const agent_picker::TileState& state, bool animate = false, int delayMs = 0);
    // Names the built-in tile accurately, including for screen readers.
    void setIdentity(const QString& name, const QString& monogram);
    void setSelected(bool selected, bool animate = true);

    // Where the shimmer band is, 0 to 1.
    double scanPhase() const;
    // 0 to 1 and back once per sweep while the shimmer shows: the mark and the border glow with
    // it.
    double scanBreath() const;
    // The card, in widget coordinates, raised by the current lift.
    QRectF cardRect() const;

Q_SIGNALS:
    void clicked(const QString& tileId);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    QVariantAnimation* makeAnimation(int durationMs, void (AgentTile::*slot)(double));
    // Runs animation from start to end, or sets end at once when they are within 1e-3.
    void run(QVariantAnimation* animation, double start, double end,
             void (AgentTile::*slot)(double));
    void beginArrival();
    void applyState(const agent_picker::TileState& state);
    QString accessibleDescriptionText() const;
    void setPill(const QString& tone, const QString& text);
    void setDetail(const QString& text, bool muted = false, bool warning = false);
    void syncInteraction();
    void syncMark();
    void onHoverValue(double value);
    void onLiftValue(double value);
    void onShimmerValue(double value);
    void onArrivalValue(double value);
    double liftAmount() const;
    void applyMargins();
    // How lit the card is: a found agent after its arrival.
    double warmth() const;
    void paintShadow(QPainter& painter, const QRectF& card, double amount,
                     const std::optional<QColor>& glow) const;
    void paintShimmer(QPainter& painter, const QPainterPath& path, const QRectF& card,
                      const QColor& accent) const;
    void openInstallPage();

    QString tileId_;
    QString name_;
    QString purpose_;
    QColor accent_;
    QString installUrl_;
    agent_picker::TileState state_;
    std::optional<agent_picker::TileState> pending_;
    bool selected_ = false;
    bool scanning_ = false;
    double shimmer_ = 0.0;
    double shimmerOffset_ = 0.0;
    QElapsedTimer clock_;
    double arrival_ = 1.0;
    double warmFrom_ = 0.0;
    double warmTo_ = 0.0;
    double missingFrom_ = 0.0;
    double hover_ = 0.0;
    double lift_ = 0.0;
    std::optional<int> marginShift_;
    bool focusVisible_ = false;

    QVBoxLayout* layout_;
    AgentMark* mark_;
    StatusPill* pill_;
    QLabel* nameLabel_;
    QLabel* detailLabel_;
    QPushButton* link_;
    QVariantAnimation* hoverAnim_;
    QVariantAnimation* liftAnim_;
    QVariantAnimation* arrivalAnim_;
    QVariantAnimation* shimmerAnim_;
    QTimer* arrivalTimer_;
};

} // namespace fv
