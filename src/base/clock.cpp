#include "base/clock.h"

#include <exception>

namespace fv {

namespace {

class SystemClock final : public Clock {
public:
    std::chrono::steady_clock::time_point steady() const override {
        return std::chrono::steady_clock::now();
    }

    std::chrono::system_clock::time_point utc() const override {
        return std::chrono::system_clock::now();
    }

    // The zone is looked up on every call, so a change of the system's zone shows at once.
    // nullptr only when the time zone database is unavailable (no ICU on Windows).
    const std::chrono::time_zone* zone() const override {
        try {
            return std::chrono::current_zone();
        } catch (const std::exception&) {
        }
        try {
            return std::chrono::locate_zone("UTC");
        } catch (const std::exception&) {
            return nullptr;
        }
    }
};

} // namespace

Clock& system_clock() {
    static SystemClock clock;
    return clock;
}

} // namespace fv
