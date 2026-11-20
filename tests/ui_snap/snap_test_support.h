#pragma once

#include <vector>

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>

#include "base/json.h"
#include "golden/golden.h"
#include "pyport/number.h"

namespace fvtest {

// Python's round() of a float, as the ported tests compute pixel positions.
inline int pyRound(double value) {
    return static_cast<int>(fv::py::round_half_even(value));
}

inline QRect rectOf(const fv::Json& values) {
    const auto v = fv::golden::decode<std::vector<int>>(values);
    return QRect(v.at(0), v.at(1), v.at(2), v.at(3));
}

inline QPoint pointOf(const fv::Json& values) {
    const auto v = fv::golden::decode<std::vector<int>>(values);
    return QPoint(v.at(0), v.at(1));
}

inline QPointF pointFOf(const fv::Json& values) {
    const auto v = fv::golden::decode<std::vector<double>>(values);
    return QPointF(v.at(0), v.at(1));
}

inline QSize sizeOf(const fv::Json& values) {
    const auto v = fv::golden::decode<std::vector<int>>(values);
    return QSize(v.at(0), v.at(1));
}

inline fv::Json listOf(const QRect& rect) {
    return fv::Json::array({rect.x(), rect.y(), rect.width(), rect.height()});
}

inline fv::Json listOf(const QRectF& rect) {
    return fv::Json::array({rect.x(), rect.y(), rect.width(), rect.height()});
}

inline fv::Json listOf(QPointF point) {
    return fv::Json::array({point.x(), point.y()});
}

inline const QColor& coral() {
    static const QColor color(QStringLiteral("#FF4F3A"));
    return color;
}

} // namespace fvtest
