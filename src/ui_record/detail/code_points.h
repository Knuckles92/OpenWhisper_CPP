#pragma once

#include <QString>

// Python's len() and slicing count code points; a QString counts UTF-16 units, so a limit that
// OpenWhisper applies to text[:n] must not cut an emoji in two.
namespace fv::detail {

// The UTF-16 length of the first maxPoints code points of text, and how many code points it holds
// when that is fewer.
inline qsizetype prefixLength(const QString& text, qsizetype maxPoints, bool* complete) {
    qsizetype index = 0;
    qsizetype points = 0;
    while (index < text.size() && points < maxPoints) {
        const bool pair = text[index].isHighSurrogate() && index + 1 < text.size() &&
                          text[index + 1].isLowSurrogate();
        index += pair ? 2 : 1;
        ++points;
    }
    if (complete != nullptr) {
        *complete = index >= text.size();
    }
    return index;
}

} // namespace fv::detail
