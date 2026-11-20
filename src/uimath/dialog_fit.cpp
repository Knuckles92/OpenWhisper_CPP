#include "uimath/dialog_fit.h"

#include <algorithm>

namespace fv {
namespace {

// QSize::boundedTo.
Size bounded_to(Size size, Size limit) {
    return Size{std::min(size.width, limit.width), std::min(size.height, limit.height)};
}

} // namespace

FitResult fit_to_screen(Size window, Size minimum, const Rect& available) {
    const Size room{std::max(1, available.width - 2 * kDialogScreenMargin),
                    std::max(1, available.height - 2 * kDialogScreenMargin)};
    FitResult result;
    result.minimum = bounded_to(minimum, room);
    result.size = bounded_to(window, room);
    result.resized = !(result.size == window);
    return result;
}

} // namespace fv
