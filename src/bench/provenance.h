#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "base/json.h"

// benchmarks/provenance.py: content identities for reproducible benchmark reuse, and the
// provenance block every fiorivoce-bench report carries. Nothing here touches the network.
namespace fv::bench {

// file_identity: the file's SHA-256, read in 1 MiB blocks. Throws std::runtime_error naming the
// file when it cannot be read.
std::string file_identity(const std::filesystem::path& path);

// The source a result depends on, relative to a FioriVoce checkout: every .h, .cpp and .inc
// under these directories, and these files (OpenWhisper's benchmarks, meeting, transcriber,
// services and config.py). Dirty bytes count, as a commit hash alone misses local edits.
const std::vector<std::string>& provenance_source_dirs();
const std::vector<std::string>& provenance_source_files();

struct IdentityRequest {
    // The checkout whose source is hashed (provenance.ROOT); none hashes nothing.
    std::optional<std::filesystem::path> source_root;
    std::vector<std::filesystem::path> inputs;
    Json settings = Json::object();
    Json model = nullptr;
};

// identity: {schema: 1, source: {relative path: sha256}, cpu_count, inputs: {absolute path:
// sha256}, settings, model, platform, machine}. OpenWhisper's GPU, Python and package versions
// have no counterpart: the runtime and model are named in `model`.
Json identity(const IdentityRequest& request);

// reusable: a stored result is reused only when its provenance equals `expected` and it holds no
// error, so an old or failed report never gains new provenance by rescoring.
bool reusable(const Json& result, const Json& expected);

// The contract block of a report (M10.4): {commit, runtime, model, machine}. commit is the
// checkout's HEAD read from .git (null outside one); runtime and model are the paths given (null
// when absent) with their size; machine is {platform, machine, cpu_count}.
Json report_provenance(const std::optional<std::filesystem::path>& source_root,
                       const std::optional<std::filesystem::path>& runtime_dir,
                       const std::optional<std::filesystem::path>& model_path);

// {schema: 1, kind, metrics, provenance}: what fiorivoce-bench writes and tools/perf reads.
Json make_report(std::string kind, Json metrics, Json provenance);

// Writes `value` as JSON indented by 2, creating the folder; non-ASCII stays UTF-8. Throws
// std::runtime_error naming the file when it cannot be written.
void write_json(const std::filesystem::path& path, const Json& value);
// A text file's contents, a UTF-8 byte order mark skipped; std::runtime_error naming the file
// when it cannot be read.
std::string read_text_file(const std::filesystem::path& path);
// Reads a JSON file, a UTF-8 byte order mark skipped (utf-8-sig); throws what Json::parse
// throws, or std::runtime_error naming an unreadable file.
Json read_json(const std::filesystem::path& path);

// The checkout fiorivoce-bench runs from: FIORIVOCE_SOURCE_ROOT, else the first parent of
// `start` holding CMakePresets.json and src/bench; nullopt when neither exists.
std::optional<std::filesystem::path> find_source_root(const std::filesystem::path& start);

} // namespace fv::bench
