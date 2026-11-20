#pragma once

#include <QString>

#include <filesystem>
#include <string>
#include <string_view>

#include "base/log.h"
#include "base/utf8.h"

namespace fv::documents::detail {

// document.py's logger.
inline constexpr log::Logger logger("services.walkthrough.document");

inline QString qtText(std::string_view utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

inline std::string utf8Text(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

inline QString qtPath(const std::filesystem::path& path) {
    return qtText(path_to_utf8(path));
}

} // namespace fv::documents::detail
