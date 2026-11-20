#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "meeting_state/ids.h"

namespace fv::meeting_state::testing {

// Deterministic ids: "000000000001", "000000000002", ... (one counter for every prefix), the
// sequence tools/parity/cases/meeting.state.patches.py gives OpenWhisper's new_id.
// Affinity: single thread.
class FixedIds final : public IdSource {
public:
    explicit FixedIds(std::uint64_t first = 1) : next_(first) {}

    std::string hex12() override {
        char buffer[16];
        std::snprintf(buffer, sizeof buffer, "%012llx", static_cast<unsigned long long>(next_++));
        return buffer;
    }

    std::uint64_t next() const { return next_; }

private:
    std::uint64_t next_;
};

} // namespace fv::meeting_state::testing
