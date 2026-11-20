#pragma once

#include <filesystem>
#include <string_view>

namespace fv::update_native {

// The settings and SQLite database as they were before a health launch. A transaction holds
// the app's exclusive data lease throughout restore; the application's DB must be closed.
// The transaction ID binds all snapshot files to <data_root>/updates/tx/<id>.
void snapshot_data(const std::filesystem::path& data_root, std::string_view transaction_id);
void restore_data(const std::filesystem::path& data_root, std::string_view transaction_id);
} // namespace fv::update_native
