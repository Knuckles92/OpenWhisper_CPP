#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "base/identity.h"

// Data roots for backup tests, as OpenWhisper's tests/test_backup.py builds them.
namespace fv::backup::testing {

// Writes `bytes` to `path`, creating its folders.
void write_bytes(const std::filesystem::path& path, std::string_view bytes);
std::string read_bytes(const std::filesystem::path& path);

// _make_data: recordings (own.wav, devices/peer/remote.wav), a meeting folder m1 with
// mic-000.wav, remote_records with a cache and a retained note, recorded_audio.wav, a .env and a
// host key (neither may be exported), the settings file of `names` (compact JSON with pairing,
// MCP, a host-kept records location, the knowledge-folder path and a secret-looking key), and a
// minimal database: schema_version at the current version, history rows h1 ("own.wav") and h2
// ("devices/peer/remote.wav"), meeting m1 ("active" when `live`, else "ended") with tokens,
// remote JSON, pid and heartbeat, its chunk 1 and segment s1, and a record_sync row.
void make_sample_data(const std::filesystem::path& root, bool live = false,
                      const AppIdentity& names = identity());

} // namespace fv::backup::testing
