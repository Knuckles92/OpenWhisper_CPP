#include "ui_snap/geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace fv::ui_snap {
namespace {

constexpr double kEpsilon = 1e-6;

// -1 near low, 1 near high, else 0; the nearer wins on a thin range.
int side(int value, int low, int high, int reach) {
    const bool nearLow = low - kGrabOutside <= value && value <= low + reach;
    const bool nearHigh = high - reach <= value && value <= high + kGrabOutside;
    if (nearLow && nearHigh) {
        return value - low < high - value ? -1 : 1;
    }
    return nearLow ? -1 : nearHigh ? 1 : 0;
}

// value within [low, high]; low wins when the range is empty.
int clamp(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

// Python's // on ints: rounds toward negative infinity.
int floorDiv(int a, int b) {
    const int quotient = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? quotient - 1 : quotient;
}

} // namespace

QRect span(QPoint a, QPoint b) {
    return QRect(std::min(a.x(), b.x()), std::min(a.y(), b.y()), std::abs(b.x() - a.x()),
                 std::abs(b.y() - a.y()));
}

QPointF snapPoint(QPointF point, std::span<const QRect> targets) {
    QPointF best = point;
    std::pair<int, double> bestRank{0, 0.0};
    const double x = point.x();
    const double y = point.y();
    for (const QRect& rect : targets) {
        const int left = rect.x();
        const int top = rect.y();
        const int right = left + rect.width();
        const int bottom = top + rect.height();
        if (!(left - kSnapDistance <= x && x <= right + kSnapDistance && top - kSnapDistance <= y &&
              y <= bottom + kSnapDistance)) {
            continue;
        }
        const int edgeX = std::abs(left - x) <= std::abs(right - x) ? left : right;
        const int edgeY = std::abs(top - y) <= std::abs(bottom - y) ? top : bottom;
        const double dx = std::abs(edgeX - x);
        const double dy = std::abs(edgeY - y);
        const bool nearX = dx <= kSnapDistance;
        const bool nearY = dy <= kSnapDistance;
        const std::pair<int, double> rank{int(nearX) + int(nearY),
                                          -(nearX ? dx : 0.0) - (nearY ? dy : 0.0)};
        if (rank > bestRank) {
            best = QPointF(nearX ? edgeX : x, nearY ? edgeY : y);
            bestRank = rank;
        }
    }
    return best;
}

std::optional<Grab> grabAt(const QRect& selection, QPoint point) {
    const int left = selection.left();
    const int top = selection.top();
    const int right = selection.right() + 1;
    const int bottom = selection.bottom() + 1;
    const int x = point.x();
    const int y = point.y();
    if (!(left - kGrabOutside <= x && x <= right + kGrabOutside && top - kGrabOutside <= y &&
          y <= bottom + kGrabOutside)) {
        return std::nullopt;
    }
    int gx = side(x, left, right, kGrabInside);
    int gy = side(y, top, bottom, kGrabInside);
    if (gx != 0 && gy == 0) {
        gy = side(y, top, bottom, kGrabCorner);
    } else if (gy != 0 && gx == 0) {
        gx = side(x, left, right, kGrabCorner);
    }
    if (gx == 0 && gy == 0) {
        return std::nullopt;
    }
    return Grab{gx, gy};
}

QRect resized(const QRect& selection, Grab grab, QPoint point) {
    int left = selection.left();
    int top = selection.top();
    int right = selection.right() + 1;
    int bottom = selection.bottom() + 1;
    if (grab.x < 0) {
        left = point.x();
    } else if (grab.x > 0) {
        right = point.x();
    }
    if (grab.y < 0) {
        top = point.y();
    } else if (grab.y > 0) {
        bottom = point.y();
    }
    return span(QPoint(left, top), QPoint(right, bottom));
}

QRect deviceRect(const QRect& selection, double dpr, const QRect& imageRect) {
    const int left = static_cast<int>(std::floor(selection.x() * dpr + kEpsilon));
    const int top = static_cast<int>(std::floor(selection.y() * dpr + kEpsilon));
    const int right =
        static_cast<int>(std::ceil((selection.x() + selection.width()) * dpr - kEpsilon));
    const int bottom =
        static_cast<int>(std::ceil((selection.y() + selection.height()) * dpr - kEpsilon));
    return QRect(left, top, right - left, bottom - top).intersected(imageRect);
}

QPoint placeToolbar(QSize size, const QRect& selection, const QRect& bounds, int gap, int margin) {
    const int x = clamp(selection.x() + floorDiv(selection.width() - size.width(), 2),
                        bounds.left() + margin, bounds.right() + 1 - margin - size.width());
    const int floor = bounds.bottom() + 1 - margin - size.height();
    const int below = selection.bottom() + 1 + gap;
    if (below <= floor) {
        return QPoint(x, below);
    }
    const int above = selection.top() - gap - size.height();
    if (above >= bounds.top() + margin) {
        return QPoint(x, above);
    }
    const int inside = selection.bottom() + 1 - gap - size.height();
    return QPoint(x, clamp(inside, bounds.top() + margin, floor));
}

QRect badgeRect(QSize size, const QRect& selection, const QRect& bounds, int gap) {
    int y = selection.top() - gap - size.height();
    if (y < bounds.top() + 4) {
        y = selection.top() + gap;
    }
    const int x = clamp(selection.left(), bounds.left() + 4, bounds.right() + 1 - 4 - size.width());
    return QRect(QPoint(x, y), size);
}

} // namespace fv::ui_snap
