#pragma once
#include "base/json.h"
#include <filesystem>
#include <string_view>

namespace fv::update_native {
// Setup owns its mutex across these calls. The manifest comes from the temporary installer
// payload. It independently determines which new files a failed installation can remove.
void prepare_setup(const std::filesystem::path& install, const Json& incoming, bool registered);
void finish_setup(const std::filesystem::path& install, const Json& incoming);
void rollback_setup(const std::filesystem::path& install, const Json& incoming, bool registered);
} // namespace fv::update_native
