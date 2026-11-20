#pragma once

#include "base/cancel.h"
#include "remote_records/host_store.h"

namespace fv {
class Database;

namespace remote_records {

// Finished meetings retained for paired devices. Routes and credentials of the
// original meeting are never activated on this computer.
std::shared_ptr<RecordKind> meeting_records(std::filesystem::path root, Database&, const Clock&);

std::vector<Json> local_meetings_for_sharing(Database&, const Clock&, int limit);
Files export_meeting_for_sharing(const std::filesystem::path& root, Database&, const Clock&,
                                 std::string_view id, const std::filesystem::path& destination,
                                 bool include_audio, const CancelToken&);

void import_meeting_for_viewing(const std::filesystem::path& root, Database&, const Clock&,
                                std::string_view id, const std::filesystem::path& files,
                                const Json& record);
} // namespace remote_records
} // namespace fv
