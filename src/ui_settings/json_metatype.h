#pragma once

#include <type_traits>

#include <QDataStream>
#include <QDebug>
#include <QtCore/qttypetraits.h>

#include "base/json.h"

// fv::Json as a signal argument. moc registers every argument type with QMetaType, which probes
// for ==, <, QDebug << and QDataStream << and >>. nlohmann::json converts implicitly to almost
// any type, so those probes reach Windows types that are only declared (tagMSG) and fail to
// compile. Delivering or queueing a signal needs none of them. Every header that declares such a
// signal includes this one, so the answers are known before moc's code asks.
namespace QTypeTraits {
template <> struct has_operator_equal<fv::Json> : std::false_type {};
template <> struct has_operator_less_than<fv::Json> : std::false_type {};
template <> struct has_ostream_operator<QDebug, fv::Json, void> : std::false_type {};
template <> struct has_ostream_operator<QDataStream, fv::Json, void> : std::false_type {};
template <> struct has_istream_operator<QDataStream, fv::Json, void> : std::false_type {};
} // namespace QTypeTraits
