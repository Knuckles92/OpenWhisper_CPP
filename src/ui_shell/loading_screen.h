#pragma once

#include <chrono>
#include <optional>

#include <QColor>
#include <QPoint>
#include <QString>
#include <QWidget>

class QTimer;

namespace fv {

// The custom-painted splash (ui_qt/loading_screen.py, the classic variant): a 450 x 300
// frameless, stay-on-top, translucent tool window centred on the primary screen, with a
// rounded gradient card, a microphone whose glow pulses at 4.5 rad/s, five orbiting dots, the
// product name, a status line and a progress line. It can be dragged with the left button.
// Colours are read once from the current palette: the splash is built after the theme is
// chosen and is gone before anyone could change it.
// Affinity: GUI thread.
class LoadingScreen : public QWidget {
    Q_OBJECT
public:
    static constexpr int kWidth = 450;
    static constexpr int kHeight = 300;
    static constexpr double kGlowRadiansPerSecond = 4.5;
    // Repaint interval of the glow (Qt::PreciseTimer).
    static constexpr int kFrameMs = 33;

    explicit LoadingScreen(QWidget* parent = nullptr);
    ~LoadingScreen() override;

    const QString& statusText() const { return statusText_; }
    const QString& progressText() const { return progressText_; }
    void updateStatus(const QString& status);
    void updateProgress(const QString& progress);

    // The glow's phase in radians: from the wall clock, so a delayed tick still paints the
    // right frame, unless pinned.
    double glowPhase() const;
    // Paints every frame at `phase` (reference renders and tests); nullopt follows the clock.
    void pinGlowPhase(std::optional<double> phase);

    // Where a left-button press grabbed the window, relative to its top-left; empty when no
    // drag is under way.
    std::optional<QPoint> dragPosition() const { return dragPosition_; }
    // Stops the glow timer: the splash is about to go.
    void stopAnimation();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    QColor accent(int alpha) const;
    QColor glow(int alpha) const;

    QString statusText_;
    QString progressText_;
    bool isDark_;
    QColor background_;
    QColor border_;
    QColor accent_;
    QColor accentGlow_;
    QColor text_;
    QColor subtext_;
    std::chrono::steady_clock::time_point glowStartedAt_;
    std::optional<double> pinnedPhase_;
    QTimer* glowTimer_;
    std::optional<QPoint> dragPosition_;
};

} // namespace fv
