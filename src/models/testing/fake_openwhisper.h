#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "base/json.h"

// Builders of OpenWhisper-style installs in a test's temporary folder, for discovery and
// adoption tests. Nothing here touches a real OpenWhisper or FioriVoce folder.
namespace fv::models::testing {

// A pinned spec of the models.json shape for one small file, standing in for a model's real pin
// (tests cannot hold a 714 MB file): {"repo", "revision", "files": [{name, url, sha256,
// size_bytes}]}.
Json small_model_spec(std::string_view file_name, std::string_view content);

// <ow_root>/components/<component_id> as OpenWhisper installs it for `tag`: the runtime library
// where the release puts it, and on Windows the embedded Python's files beside bin/; its
// manifest.json is this build's catalog entry as OpenWhisper pins it (on Windows the Python
// embeddable zip listed first and counted in install_bytes, as OpenWhisper's catalog has it),
// written with "\r\n" line ends on Windows; the sentinel last. Returns the folder.
std::filesystem::path write_openwhisper_runtime(const std::filesystem::path& ow_root,
                                                std::string_view component_id,
                                                std::string_view tag);

// <ow_root>/speech-models/<key> holding `contents` (file name to bytes) and installed.json with
// `spec` as json.dumps writes it. Returns the folder.
std::filesystem::path write_openwhisper_model(const std::filesystem::path& ow_root,
                                              std::string_view key, const Json& spec,
                                              const std::map<std::string, std::string>& contents);

} // namespace fv::models::testing
