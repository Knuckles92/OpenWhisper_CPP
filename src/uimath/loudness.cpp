#include "uimath/loudness.h"

#include <cmath>

namespace fv {

double level_to_height(double level) {
    if (level <= 0.0) {
        return 0.0;
    }
    const double db = 20.0 * std::log10(level);
    const double t = (db - kLoudnessFloorDb) / (kLoudnessCeilingDb - kLoudnessFloorDb);
    // Python's max(0.0, min(1.0, t)) keeps the first argument unless the other compares past it,
    // so a NaN level comes out as 1.0, not NaN.
    const double upper = (t < 1.0) ? t : 1.0;
    return (upper > 0.0) ? upper : 0.0;
}

} // namespace fv
