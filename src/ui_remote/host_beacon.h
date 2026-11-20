#pragma once

#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace fv {

// This computer as a small server, lit when it shares (host_dashboard.py's HostBeacon).
// setState takes off / starting / on / error. While busy (a paired computer's audio is being
// decoded) waves run out from it; ripple() sends one ring out when a transcription finishes.
// It animates only while shown, on a 33 ms tick.
// Affinity: GUI thread.
class HostBeacon final : public QWidget {
    Q_OBJECT
public:
    static constexpr int kSize = 60;

    explicit HostBeacon(QWidget* parent = nullptr);

    QString state() const { return state_; }
    bool busy() const { return busy_; }
    // The rings still running out (their start times on the beacon's clock).
    int ripples() const { return static_cast<int>(ripples_.size()); }
    bool isAnimating() const { return timer_.isActive(); }

    void setState(const QString& state);
    void setBusy(bool busy);
    void ripple();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool animating() const;
    void sync();
    void tick();
    QColor lightColor() const;

    QString state_ = QStringLiteral("off");
    bool busy_ = false;
    qint64 busySince_ = 0;
    std::vector<qint64> ripples_;
    std::optional<qint64> litAt_;
    QElapsedTimer clock_;
    QTimer timer_;
};

} // namespace fv
