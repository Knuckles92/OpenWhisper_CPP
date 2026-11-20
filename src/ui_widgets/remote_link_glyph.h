#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

namespace fv {

// The Remote engine's link, drawn as this computer, a wire and the host
// (ui_qt/widgets/remote_link.py RemoteLinkGlyph). Every motion answers something that happened
// on the connection: a soft pulse runs home when a keepalive comes back, packets run out while a
// request waits on the host and one runs home when its reply lands, dots search along the wire
// while a connection is being made, and the first time a host answers the wire draws itself
// across and the host's light comes on. Offline or unpaired the wire is broken and nothing
// moves. Mirrored, it is the host's "serving" chip. Clicking asks to reconnect.
// Affinity: GUI thread.
class RemoteLinkGlyph : public QWidget {
    Q_OBJECT
public:
    static constexpr int kWidth = 44;
    static constexpr int kHeight = 16;
    // Milliseconds a pulse or packet takes to cross the wire, and the other animations' lengths.
    static constexpr int kHeartbeatMs = 900;
    static constexpr int kReplyMs = 420;
    static constexpr int kSendPeriodMs = 760;
    static constexpr int kSearchPeriodMs = 1100;
    static constexpr int kDrawMs = 520;
    static constexpr int kLightMs = 460;
    // The repaint period while something moves.
    static constexpr int kFrameMs = 33;

    explicit RemoteLinkGlyph(QWidget* parent = nullptr, bool mirrored = false);

    // "unpaired", "connecting", "connected" or "offline". busy: a request waits on the host;
    // replies and beat count replies and keepalive pongs on this connection, so a new one is a
    // packet or a pulse coming home.
    void setLink(const QString& state, bool busy = false, std::int64_t replies = 0,
                 std::int64_t beat = 0);

    const QString& state() const { return state_; }
    bool busy() const { return busy_; }
    bool mirrored() const { return mirrored_; }
    // Whether the frame timer runs: only while something moves and the glyph is shown.
    bool isAnimating() const { return timer_.isActive(); }
    // The wire is drawing itself across (the first answer from a host).
    bool isDrawingWire() const { return drawnAt_.has_value(); }
    // The kinds ("beat" or "reply") of the pulses and packets heading home, oldest first.
    QStringList flightKinds() const;

    // Advances the animation one frame: drops finished flights and the finished draw.
    void tick();
    // Replaces the elapsed-time clock (milliseconds); tests step it.
    void setClock(std::function<std::int64_t()> clock);

Q_SIGNALS:
    void clicked();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct Flight {
        std::int64_t start;
        int duration;
        QString kind;
    };

    std::int64_t now() const;
    bool animating() const;
    void syncAnimation();
    QColor wireColor() const;

    bool mirrored_;
    QString state_ = QStringLiteral("unpaired");
    bool busy_ = false;
    std::int64_t replies_ = 0;
    std::int64_t beat_ = 0;
    bool greeted_ = false;
    std::vector<Flight> flights_;
    std::optional<std::int64_t> drawnAt_;
    std::int64_t busySince_ = 0;
    QElapsedTimer elapsed_;
    std::function<std::int64_t()> clock_;
    QTimer timer_;
};

} // namespace fv
