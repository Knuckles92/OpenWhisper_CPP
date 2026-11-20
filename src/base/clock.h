#pragma once

#include <chrono>

namespace fv {

// Time, injected so tests can pin it (fv_testkit's FakeClock).
// Affinity: thread-safe.
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::steady_clock::time_point steady() const = 0;
    virtual std::chrono::system_clock::time_point utc() const = 0;
    virtual const std::chrono::time_zone* zone() const = 0; // tests pin it
};

Clock& system_clock();

} // namespace fv
