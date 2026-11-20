#pragma once

#include <string>
#include <string_view>

// Short unique ids for meeting-state entities (schema.py new_id).
namespace fv::meeting_state {

// Twelve lowercase hex digits per call: uuid4().hex[:12]. Tests pin it with
// testing/fixed_ids.h.
// Affinity: an implementation documents its own; RandomIds is thread-safe.
class IdSource {
public:
    virtual ~IdSource() = default;
    virtual std::string hex12() = 0;
};

// From the operating system's CSPRNG, as uuid.uuid4().
class RandomIds final : public IdSource {
public:
    std::string hex12() override;
};

// The process-wide random source.
IdSource& random_ids();

// "<prefix>_<hex12>": "it" card items, "q" questions, "p" participants, "rep" reports.
std::string new_id(std::string_view prefix, IdSource& ids = random_ids());

} // namespace fv::meeting_state
