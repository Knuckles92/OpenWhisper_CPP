#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

namespace fv {

class Clock;

// The walkthrough column's Snap menu: what Snap takes, from now on
// (ui_qt/widgets/walkthrough_snap_menu.py). It opens beside the column, away from the screen's
// edge, and lists the kinds of screenshot this desktop can take; picking one takes it at once
// and makes it what the Snap button and the Screenshot shortcut repeat. Like the column it
// never takes the focus (a Qt popup would take it on Windows, even told not to), so it cannot
// hear a click in another app: it closes once the pointer has been away from it and the column
// for a moment, as well as on a pick or another click in the column. Deletes itself on close.
// Affinity: GUI thread.
class SnapMenu : public QWidget {
    Q_OBJECT

public:
    static constexpr int kWidth = 250;
    static constexpr int kPad = 5;
    static constexpr int kHeaderH = 26;
    static constexpr int kItemH = 36;
    static constexpr int kRadius = 12;
    // Room around the menu for its shadow.
    static constexpr int kShadow = 12;
    // Its gap from the column's card.
    static constexpr int kGap = 10;
    // The pointer away from the menu and the column this long closes it, polled this often.
    static constexpr double kAwayS = 0.6;
    static constexpr int kWatchMs = 100;

    // modes are WalkthroughSnapMode values; clock times the pointer's absence and must outlive
    // the menu.
    SnapMenu(const Clock& clock, const QStringList& modes, const QString& current,
             const QString& shortcut = QString());

    const QStringList& modes() const { return modes_; }
    const QString& current() const { return current_; }
    const QString& shortcut() const { return shortcut_; }
    QRect nearRect() const { return near_; }
    void setNearRect(const QRect& rect) { near_ = rect; }
    // Where the pointer is, in global coordinates; QCursor::pos() unless replaced.
    void setPointerSource(std::function<QPoint()> pointer) { pointer_ = std::move(pointer); }

    // Opens left of card (global), its foot level with bottom; right of it where the screen
    // has no room on the left. The pointer may wander over nearRect (the column; card when
    // nullopt) without closing it.
    void openBeside(const QRect& card, int bottom, std::optional<QRect> nearRect = std::nullopt);

    // Mode index's row, in the menu's coordinates.
    QRectF item(int index) const;
    // One poll of the pointer: closes the menu once it has been away for kAwayS.
    void checkPointer();

Q_SIGNALS:
    // A WalkthroughSnapMode value.
    void chosen(const QString& mode);

protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QRectF menuRect() const;
    QString at(QPointF position) const;
    double now() const;

    const Clock& clock_;
    QStringList modes_;
    QString current_;
    QString shortcut_;
    QString hover_;
    QRect near_;
    std::optional<double> awaySince_;
    QTimer watch_;
    std::function<QPoint()> pointer_;
};

// The label the Snap menu shows for a WalkthroughSnapMode value ("Region", "Window under the
// pointer", "Whole screen", "In 3 seconds"); the value itself when it has none.
QString snapModeLabel(const QString& mode);

} // namespace fv
