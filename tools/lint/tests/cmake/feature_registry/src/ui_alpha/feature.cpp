#include "ui_alpha/feature.h"

#include "ui_alpha/data/greeting.json.h"

namespace fv {

std::string_view alpha_greeting() {
    return embedded::ui_alpha_greeting;
}

} // namespace fv
