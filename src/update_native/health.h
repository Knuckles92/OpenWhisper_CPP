#pragma once
#include "update_native/journal.h"
#include <optional>
#include <span>
#include <string>

namespace fv::update_native {
struct HealthRequest {
    std::string transaction_id;
    std::string token;
};
// An explicit transaction ID avoids scanning other updates. Invalid health switches throw.
std::optional<HealthRequest> health_request(std::span<const std::string> arguments);
// Called before startup and again once the event loop and database are ready. Both the executable
// and registered install directory are supplied by the running application, never the journal.
bool check_health(const JournalContext&, const HealthRequest&,
                  const std::filesystem::path& executable, std::string_view running_version,
                  bool acknowledge);
} // namespace fv::update_native
