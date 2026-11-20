#pragma once

#include "remote_records/host_store.h"

namespace fv {
class Database;
}

namespace fv::remote_records {

std::shared_ptr<RecordKind> dictation_records(std::filesystem::path recordings, Database& db);

} // namespace fv::remote_records
