#include "models/speech_model_cache.h"

#include <stdexcept>

#include "base/utf8.h"
#include "download/verified_files.h"
#include "models/detail/files.h"
#include "models/detail/fs_native.h"
#include "models/hf_access_policy.h"
#include "models/speech_catalog.h"
#include "pyport/json.h"

namespace fv::models {

namespace fs = std::filesystem;

namespace {

fs::path sibling(const fs::path& target, std::string_view suffix) {
    return fs::path(target).concat(fv::base_utf8::path_from_utf8(suffix).native());
}

bool path_exists(const fs::path& path) {
    std::error_code error;
    return fs::exists(path, error);
}

} // namespace

SpeechModelCache::SpeechModelCache(fs::path speech_models_root, http::Client& client,
                                   SpeechModelCacheOptions options)
    : root_(std::move(speech_models_root)), client_(client), options_(std::move(options)) {}

Json SpeechModelCache::spec(std::string_view key) const {
    return options_.artifacts ? options_.artifacts(key) : artifacts(key);
}

void SpeechModelCache::replace(const fs::path& from, const fs::path& to) const {
    if (options_.replace) {
        options_.replace(from, to);
    } else {
        fs::rename(from, to);
    }
}

std::mutex& SpeechModelCache::key_lock(std::string_view key) {
    std::lock_guard lock(locks_mutex_);
    auto it = locks_.find(key);
    if (it == locks_.end()) {
        it = locks_.emplace(std::string(key), std::make_unique<std::mutex>()).first;
    }
    return *it->second;
}

fs::path SpeechModelCache::model_dir(std::string_view key) const {
    if (model(key) == nullptr) {
        throw std::invalid_argument("Unknown speech model");
    }
    return root_ / fv::base_utf8::path_from_utf8(key);
}

bool SpeechModelCache::is_cached(std::string_view key) const {
    return is_model_folder_complete(model_dir(key), spec(key));
}

bool is_model_folder_complete(const fs::path& directory, const Json& pinned) {
    const auto text = detail::read_bytes(directory / "installed.json");
    if (!text) {
        return false;
    }
    try {
        if (!py::py_equal(py::loads(*text), pinned)) {
            return false;
        }
        for (const Json& file : pinned.at("files")) {
            std::error_code error;
            const auto size = fs::file_size(
                directory / fv::base_utf8::path_from_utf8(file.at("name").get<std::string>()),
                error);
            if (error ||
                static_cast<std::int64_t>(size) != file.at("size_bytes").get<std::int64_t>()) {
                return false;
            }
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

fs::path SpeechModelCache::load_path(std::string_view key) const {
    if (!is_cached(key)) {
        throw ModelCacheError(std::string(model(key)->label) +
                              " is not downloaded. Open Downloads.");
    }
    const Json pinned = spec(key);
    for (const Json& file : pinned.at("files")) {
        const auto& name = file.at("name").get_ref<const std::string&>();
        if (name.ends_with(".gguf")) {
            return model_dir(key) / fv::base_utf8::path_from_utf8(name);
        }
    }
    return model_dir(key);
}

fs::path SpeechModelCache::download(std::string_view key, const ModelProgress& progress,
                                    const CancelToken& cancel) {
    // The hard offline override covers every model host, not only Hugging Face.
    if (is_hf_hub_offline_env_set(options_.env != nullptr ? *options_.env : system_env())) {
        throw ModelCacheError("Model downloads are disabled by HF_HUB_OFFLINE.");
    }
    const Json pinned = spec(key);
    const fs::path target = model_dir(key);
    fs::create_directories(target.parent_path());
    std::int64_t total = 0;
    for (const Json& file : pinned.at("files")) {
        total += file.at("size_bytes").get<std::int64_t>();
    }

    std::lock_guard in_process(key_lock(key));
    const detail::FileLock across_processes(sibling(target, ".lock"));
    const fs::path backup = sibling(target, ".previous");
    if (!path_exists(target) && path_exists(backup)) {
        replace(backup, target);
    }
    if (is_cached(key)) {
        return target;
    }
    const fs::path staging = sibling(target, ".partial");
    fs::create_directories(staging);

    const InstallProgress overall = [&progress](std::string_view, std::int64_t done,
                                                std::int64_t count) {
        if (progress) {
            progress(done, count);
        }
    };
    std::int64_t done = 0;
    for (const Json& file : pinned.at("files")) {
        if (cancel.cancelled()) {
            throw ComponentCanceled();
        }
        const auto& name = file.at("name").get_ref<const std::string&>();
        const auto& url = file.at("url").get_ref<const std::string&>();
        const auto& sha = file.at("sha256").get_ref<const std::string&>();
        const auto size = file.at("size_bytes").get<std::int64_t>();
        const fs::path destination = staging / fv::base_utf8::path_from_utf8(name);
        if (options_.download_file) {
            options_.download_file(url, sha, size, destination, overall, cancel, done, total);
        } else {
            download_component_file(client_, url, sha, size, destination, overall, cancel, done,
                                    total);
        }
        done += size;
    }
    // Path.write_text(json.dumps(spec)): one line, so text mode changes nothing.
    detail::write_text(staging / "installed.json", py::dumps(pinned));
    swap_into_place(staging, target);
    return target;
}

fs::path SpeechModelCache::install_local(std::string_view key,
                                         const std::function<void(const fs::path&)>& fill,
                                         const std::string& mismatch, const CancelToken& cancel) {
    const Json pinned = spec(key);
    const fs::path target = model_dir(key);
    fs::create_directories(target.parent_path());

    std::lock_guard in_process(key_lock(key));
    const detail::FileLock across_processes(sibling(target, ".lock"));
    const fs::path backup = sibling(target, ".previous");
    if (!path_exists(target) && path_exists(backup)) {
        replace(backup, target);
    }
    if (is_cached(key)) {
        return target;
    }
    const fs::path staging = sibling(target, ".partial");
    // A download's partial files may be links to another install's: start clean.
    if (path_exists(staging)) {
        fs::remove_all(staging);
    }
    fs::create_directories(staging);
    try {
        fill(staging);
        for (const Json& file : pinned.at("files")) {
            if (cancel.cancelled()) {
                throw ComponentCanceled();
            }
            const fs::path path =
                staging / fv::base_utf8::path_from_utf8(file.at("name").get<std::string>());
            std::error_code error;
            const auto size = fs::file_size(path, error);
            if (error ||
                static_cast<std::int64_t>(size) != file.at("size_bytes").get<std::int64_t>() ||
                detail::sha256_cancellable(path, cancel) != file.at("sha256").get<std::string>()) {
                throw ModelCacheError(mismatch);
            }
        }
        if (cancel.cancelled()) {
            throw ComponentCanceled();
        }
        detail::write_text(staging / "installed.json", py::dumps(pinned));
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        throw;
    }
    swap_into_place(staging, target);
    return target;
}

void SpeechModelCache::swap_into_place(const fs::path& staging, const fs::path& target) const {
    const fs::path backup = sibling(target, ".previous");
    if (path_exists(backup)) {
        fs::remove_all(backup);
    }
    if (path_exists(target)) {
        replace(target, backup);
    }
    try {
        replace(staging, target);
    } catch (const std::system_error&) {
        if (path_exists(backup)) {
            replace(backup, target);
        }
        throw;
    }
    if (path_exists(backup)) {
        fs::remove_all(backup);
    }
}

void SpeechModelCache::remove(std::string_view key) {
    const fs::path target = model_dir(key);
    fs::create_directories(target.parent_path());
    std::lock_guard in_process(key_lock(key));
    const detail::FileLock across_processes(sibling(target, ".lock"));
    // An interrupted swap leaves .previous; download() would move it back.
    for (const fs::path& directory :
         {target, sibling(target, ".partial"), sibling(target, ".previous")}) {
        if (path_exists(directory)) {
            fs::remove_all(directory);
        }
    }
}

std::vector<CachedModelInfo> SpeechModelCache::inventory() const {
    std::vector<CachedModelInfo> cached;
    for (const SpeechModel& entry : models()) {
        if (!is_cached(entry.key)) {
            continue;
        }
        const Json pinned = spec(entry.key);
        CachedModelInfo info;
        info.repo_id = pinned.at("repo").get<std::string>();
        for (const Json& file : pinned.at("files")) {
            info.size_on_disk += file.at("size_bytes").get<std::int64_t>();
        }
        info.path = model_dir(entry.key);
        info.revisions = {pinned.at("revision").get<std::string>()};
        cached.push_back(std::move(info));
    }
    return cached;
}

} // namespace fv::models
