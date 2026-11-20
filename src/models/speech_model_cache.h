#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/env.h"
#include "base/json.h"
#include "http/client.h"
#include "models/component_store.h"

// Downloaded speech models (services/local_asr/cache.py): speech-models/<key>/ holds the pinned
// files and installed.json, the model's pinned spec. A model counts as cached only when
// installed.json equals the spec and every file has its pinned size; downloads assemble the
// folder in <key>.partial and swap it in, keeping the previous copy as <key>.previous until the
// new one is in place. Blocking: never on the GUI thread.
namespace fv::models {

// Python's RuntimeError from the cache: the message is for the user.
class ModelCacheError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// cache.is_cached for a model folder anywhere: installed.json parses equal to `spec` and every
// pinned file has its pinned size.
bool is_model_folder_complete(const std::filesystem::path& directory, const Json& spec);

// One cached model, as hf_access.CachedModelInfo lists it.
struct CachedModelInfo {
    std::string repo_id;
    std::int64_t size_on_disk = 0; // the pinned sizes summed
    std::filesystem::path path;
    std::vector<std::string> revisions;
};

// (bytes done, bytes total) across every file of the model.
using ModelProgress = std::function<void(std::int64_t done, std::int64_t total)>;

struct SpeechModelCacheOptions {
    const EnvReader* env = nullptr; // HF_HUB_OFFLINE; nullptr: system_env()
    // The pinned spec of a model; empty: artifacts().
    std::function<Json(std::string_view key)> artifacts;
    // One file's verified download (components._download_verified); empty:
    // download_component_file through the cache's client.
    std::function<void(std::string_view url, std::string_view sha256, std::int64_t size,
                       const std::filesystem::path& destination, const InstallProgress& progress,
                       const CancelToken& cancel, std::int64_t offset_base,
                       std::int64_t grand_total)>
        download_file;
    // os.replace; empty: std::filesystem::rename.
    std::function<void(const std::filesystem::path&, const std::filesystem::path&)> replace;
};

// Affinity: thread-safe. Each model's download, swap and removal hold that model's lock, in this
// process and (through <key>.lock) across processes.
class SpeechModelCache {
public:
    // `speech_models_root` is Paths::speech_models_root() in the app.
    SpeechModelCache(std::filesystem::path speech_models_root, http::Client& client,
                     SpeechModelCacheOptions options = {});

    // <root>/<key>. Throws std::invalid_argument "Unknown speech model" for a key not in
    // models().
    std::filesystem::path model_dir(std::string_view key) const;
    bool is_cached(std::string_view key) const;
    // The model's .gguf file, or its folder when it has none. Throws ModelCacheError
    // "<label> is not downloaded. Open Downloads." when it is not cached.
    std::filesystem::path load_path(std::string_view key) const;
    // Downloads the model unless it is cached and returns its folder. Throws ModelCacheError
    // "Model downloads are disabled by HF_HUB_OFFLINE." under that override (consent is the
    // caller's: it must have passed the access policy), ComponentCanceled when cancelled,
    // ComponentError when a file fails, std::filesystem::filesystem_error when the swap does
    // (the previous copy is restored first).
    std::filesystem::path download(std::string_view key, const ModelProgress& progress = {},
                                   const CancelToken& cancel = {});
    // Installs the model from files already on this computer instead of downloading them:
    // `fill` puts each pinned file into the staging folder it is given (linked or copied); each
    // must then have its pinned size and SHA-256, or ModelCacheError `mismatch` is thrown and
    // nothing is published. Then installed.json is written and the folder swapped in, under the
    // same locks as download(). A model already cached is left as it is.
    std::filesystem::path
    install_local(std::string_view key,
                  const std::function<void(const std::filesystem::path&)>& fill,
                  const std::string& mismatch, const CancelToken& cancel = {});
    // Removes the model, its .partial and its .previous folders (cache.delete).
    void remove(std::string_view key);
    // The cached models, in catalog order.
    std::vector<CachedModelInfo> inventory() const;

    const std::filesystem::path& root() const { return root_; }

private:
    Json spec(std::string_view key) const;
    void replace(const std::filesystem::path& from, const std::filesystem::path& to) const;
    void swap_into_place(const std::filesystem::path& staging,
                         const std::filesystem::path& target) const;
    std::mutex& key_lock(std::string_view key);

    std::filesystem::path root_;
    http::Client& client_;
    SpeechModelCacheOptions options_;
    std::mutex locks_mutex_;
    std::map<std::string, std::unique_ptr<std::mutex>, std::less<>> locks_;
};

} // namespace fv::models
