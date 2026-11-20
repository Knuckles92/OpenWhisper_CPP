#include "appcore/whats_new.h"

#include <format>

#include "base/identity.h"

namespace fv::appcore {

const WhatsNew& whats_new() {
    static const WhatsNew note{
        "0.1",
        "2026-11-14",
        std::format("{} 0.1 is the C++ rewrite of {}: the same dictation, uploads, history and "
                    "settings, with Parakeet running in-process for faster starts and lower "
                    "memory.",
                    identity().product, legacy_identity().product),
    };
    return note;
}

bool is_seen(const Json& seen) {
    return seen.is_string() && seen.get<std::string>() == whats_new().updated;
}

} // namespace fv::appcore
