#include "models/openwhisper_discovery.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "download/verified_download.h"
#include "download/verified_files.h"
#include "models/component_store.h"
#include "models/detail/files.h"
#include "models/detail/no_download_client.h"
#include "models/runtime_resolver.h"
#include "models/speaker_model.h"
#include "models/speech_catalog.h"
#include "models/speech_model_cache.h"
#include "pyport/json.h"
#include "settings/keys.h"

namespace fv::models {

namespace fs = std::filesystem;

namespace {

constexpr log::Logger kLog("models.adoption");

// The files python-3.12.10-embed-amd64.zip puts at the top of OpenWhisper's Windows runtime
// (plus the .pyd modules), and OpenWhisper's own markers: OpenWhisper_CPP writes its own.
constexpr std::array<std::string_view, 16> kSkippedTopLevel{
    "python.exe",       "pythonw.exe",        "python3.dll",   "python312.dll",
    "python312.zip",    "python312._pth",     "python.cat",    "LICENSE.txt",
    "libcrypto-3.dll",  "libssl-3.dll",       "libffi-8.dll",  "sqlite3.dll",
    "vcruntime140.dll", "vcruntime140_1.dll", "manifest.json", ".installed"};

fs::path from_utf8(std::string_view text) {
    return fv::base_utf8::path_from_utf8(text);
}

// The OpenWhisper speaker model that failed its integrity check, beside OpenWhisper_CPP's
// speaker-model folder (never in it: that folder holds only the model): {"path", "size",
// "mtime"} (mtime in the file clock's ticks). One file at a time.
constexpr std::string_view kRejectedSpeakerModel = ".adopted-speaker-model-rejected.json";

// What identifies a file for kRejectedSpeakerModel; nullopt when it cannot be read.
std::optional<Json> file_identity_of(const fs::path& file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error) {
        return std::nullopt;
    }
    const auto mtime = fs::last_write_time(file, error);
    if (error) {
        return std::nullopt;
    }
    return Json{{"path", fv::base_utf8::path_to_utf8(file)},
                {"size", static_cast<std::int64_t>(size)},
                {"mtime", static_cast<std::int64_t>(mtime.time_since_epoch().count())}};
}

// Bytes of the regular files under `root` (links not followed); what a copy of it takes.
std::int64_t tree_size_bytes(const fs::path& root) {
    std::int64_t total = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator walk(root, error), end; !error && walk != end;
         walk.increment(error)) {
        std::error_code ignored;
        if (walk->symlink_status(ignored).type() == fs::file_type::regular) {
            const auto size = walk->file_size(ignored);
            if (!ignored) {
                total += static_cast<std::int64_t>(size);
            }
        }
    }
    return total;
}

std::string utf8(const fs::path& path) {
    return fv::base_utf8::path_to_utf8(path);
}

bool skipped_top_level(const fs::path& name) {
    const std::string text = utf8(name);
    return text.ends_with(".pyd") ||
           std::ranges::find(kSkippedTopLevel, std::string_view(text)) != kSkippedTopLevel.end();
}

bool is_file(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

// Every archive the catalog pins for this entry is one the OpenWhisper manifest also pinned.
bool same_pins(const Json& catalog_entry_json, const Json& manifest) {
    const Json* wanted = detail::member(catalog_entry_json, "archives");
    const Json* had = detail::member(manifest, "archives");
    if (wanted == nullptr || had == nullptr || !wanted->is_array() || !had->is_array() ||
        wanted->empty()) {
        return false;
    }
    for (const Json& archive : *wanted) {
        const Json* sha = detail::member(archive, "sha256");
        if (sha == nullptr || !sha->is_string()) {
            return false;
        }
        const bool found = std::ranges::any_of(*had, [&](const Json& other) {
            const Json* other_sha = detail::member(other, "sha256");
            return other_sha != nullptr && *other_sha == *sha;
        });
        if (!found) {
            return false;
        }
    }
    return true;
}

std::string import_refusal() {
    std::string labels;
    const auto all = models();
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i > 0) {
            labels += i + 1 == all.size() ? " or " : ", ";
        }
        labels += all[i].label;
    }
    return std::format("This file isn't a model {} knows. Pick {}.", identity().product, labels);
}

// The runtime component the engine would use, as resolve_parakeet chooses it.
std::string wanted_runtime(const Json& settings, const Host& host) {
    const std::string runtime = keys::fv_speech_runtime.read(settings);
    if (runtime == "cpu") {
        return runtime_id("parakeet", "cpu", host);
    }
    if (runtime == "cuda") {
        return runtime_id("parakeet", "cuda", host);
    }
    return resolve_runtime("parakeet", selected_device(settings, "parakeet"), host).first;
}

} // namespace

OpenWhisperAdopter::OpenWhisperAdopter(fs::path components_root, fs::path speech_models_root,
                                       std::vector<fs::path> openwhisper_roots, Host host,
                                       AdopterOptions options)
    : components_root_(std::move(components_root)),
      speech_models_root_(std::move(speech_models_root)),
      openwhisper_roots_(std::move(openwhisper_roots)), host_(std::move(host)),
      options_(std::move(options)) {
    host_.is_installed = [this](std::string_view component_id) {
        return installed_here(component_id);
    };
}

namespace {

AdopterOptions app_options(const Paths& paths) {
    AdopterOptions options;
    options.speaker_model_dir = speaker_model_cache_dir(paths);
    return options;
}

} // namespace

OpenWhisperAdopter::OpenWhisperAdopter(const Paths& paths)
    : OpenWhisperAdopter(paths.components_root(), paths.speech_models_root(),
                         paths.openwhisper_roots(), Host::current({}), app_options(paths)) {}

bool OpenWhisperAdopter::rejected_speaker_model(const fs::path& file) const {
    const auto saved = detail::read_bytes(options_.speaker_model_dir.parent_path() /
                                          from_utf8(kRejectedSpeakerModel));
    if (!saved) {
        return false;
    }
    try {
        const auto identity = file_identity_of(file);
        return identity && py::loads(*saved) == *identity;
    } catch (const std::exception&) {
        return false; // unreadable: checked again, and rewritten if it fails again
    }
}

void OpenWhisperAdopter::remember_rejected_speaker_model(const fs::path& file) const {
    const auto identity = file_identity_of(file);
    if (!identity) {
        return;
    }
    try {
        detail::write_text(options_.speaker_model_dir.parent_path() /
                               from_utf8(kRejectedSpeakerModel),
                           identity->dump());
    } catch (const std::exception& failure) {
        kLog.debug("Could not remember the rejected speaker model: {}", failure.what());
    }
}

std::string OpenWhisperAdopter::speaker_sha256() const {
    return options_.speaker_model_sha256.empty() ? std::string(kSpeakerModelSha256)
                                                 : options_.speaker_model_sha256;
}

std::int64_t OpenWhisperAdopter::speaker_size_bytes() const {
    return options_.speaker_model_size_bytes > 0 ? options_.speaker_model_size_bytes
                                                 : kSpeakerModelSizeBytes;
}

Json OpenWhisperAdopter::spec(std::string_view key) const {
    return options_.artifacts ? options_.artifacts(key) : artifacts(key);
}

SpeechModelCacheOptions OpenWhisperAdopter::cache_options() const {
    SpeechModelCacheOptions options;
    options.artifacts = options_.artifacts;
    return options;
}

ComponentStoreOptions OpenWhisperAdopter::store_options() const {
    ComponentStoreOptions options;
    options.host = host_;
    options.free_bytes = options_.free_bytes;
    return options;
}

bool OpenWhisperAdopter::installed_here(std::string_view component_id) const {
    return is_file(components_root_ / from_utf8(component_id) / ".installed");
}

void OpenWhisperAdopter::link_or_copy(const fs::path& from, const fs::path& to,
                                      const std::function<void()>& before_copy,
                                      const CancelToken& cancel) const {
    try {
        if (options_.link_file) {
            options_.link_file(from, to);
        } else {
            fs::create_hard_link(from, to);
        }
    } catch (const std::exception& failure) {
        // Another volume, or a file system without hard links.
        kLog.info("Copying {} instead of linking it: {}", utf8(from.filename()), failure.what());
        if (before_copy) {
            before_copy();
        }
        detail::copy_file_cancellable(from, to, cancel);
    }
}

Discovery OpenWhisperAdopter::discover() const {
    Discovery found;
    ComponentStore probe(components_root_, detail::no_download_client(), store_options());
    for (const std::string_view id : runtime_ids()) {
        const auto entry = catalog_entry(id, host_);
        if (!entry || installed_here(id)) {
            continue;
        }
        for (const auto& root : openwhisper_roots_) {
            const fs::path tree = root / "components" / from_utf8(id);
            if (!is_file(tree / ".installed")) {
                continue;
            }
            const auto manifest = read_manifest_file(tree / "manifest.json");
            if (!manifest || check_compatibility(*manifest, host_) ||
                !same_pins(*entry, *manifest)) {
                continue;
            }
            try {
                probe.validate_payload(id, tree);
            } catch (const ComponentError&) {
                continue;
            }
            AdoptableComponent component;
            component.component_id = std::string(id);
            component.source = tree;
            if (const Json* version = detail::member(*manifest, "version")) {
                component.version = *version;
            }
            found.components.push_back(std::move(component));
            break;
        }
    }
    for (const SpeechModel& entry : models()) {
        const Json pinned = spec(entry.key);
        if (is_model_folder_complete(speech_models_root_ / from_utf8(entry.key), pinned)) {
            continue;
        }
        for (const auto& root : openwhisper_roots_) {
            const fs::path folder = root / "speech-models" / from_utf8(entry.key);
            if (!is_model_folder_complete(folder, pinned)) {
                continue;
            }
            AdoptableModel model_found;
            model_found.key = std::string(entry.key);
            model_found.label = std::string(entry.label);
            model_found.source = folder;
            for (const Json& file : pinned.at("files")) {
                model_found.size_bytes += file.at("size_bytes").get<std::int64_t>();
            }
            found.models.push_back(std::move(model_found));
            break;
        }
    }
    if (!options_.speaker_model_dir.empty() && !first_onnx_file(options_.speaker_model_dir)) {
        for (const auto& root : openwhisper_roots_) {
            const fs::path file = root / "models" / "speaker-id" / from_utf8(kSpeakerModelFilename);
            std::error_code error;
            const auto size = is_file(file) ? fs::file_size(file, error) : 0;
            if (!error && static_cast<std::int64_t>(size) == speaker_size_bytes()) {
                if (rejected_speaker_model(file)) {
                    continue;
                }
                found.speaker_model = file;
                break;
            }
        }
    }
    return found;
}

void OpenWhisperAdopter::adopt_component(std::string_view component_id, const CancelToken& cancel) {
    const Discovery found = discover();
    const auto it =
        std::ranges::find(found.components, component_id, &AdoptableComponent::component_id);
    if (it == found.components.end()) {
        throw AdoptionError(std::format("{}'s copy of {} can't be used: it is missing, incomplete "
                                        "or a different release.",
                                        legacy_identity().product, component_id));
    }
    const fs::path source = it->source;
    const auto entry = catalog_entry(component_id, host_);
    ComponentStore store(components_root_, detail::no_download_client(), store_options());
    // A link takes no room; the first file that must be copied checks that the whole tree fits
    // (ComponentError "Not enough disk space: ..." when it does not), as a download does.
    bool space_checked = false;
    const auto before_copy = [&] {
        if (!std::exchange(space_checked, true)) {
            store.check_free_space(tree_size_bytes(source));
        }
    };
    const auto fill = [&](const fs::path& staging) {
        std::error_code error;
        for (fs::recursive_directory_iterator walk(source, error), end; !error && walk != end;
             walk.increment(error)) {
            if (cancel.cancelled()) {
                throw ComponentCanceled();
            }
            const fs::path relative = walk->path().lexically_relative(source);
            const auto status = walk->symlink_status();
            if (walk.depth() == 0 && status.type() != fs::file_type::directory &&
                skipped_top_level(relative)) {
                continue;
            }
            const fs::path target = staging / relative;
            if (status.type() == fs::file_type::directory) {
                fs::create_directories(target);
            } else if (status.type() == fs::file_type::symlink) {
                fs::create_directories(target.parent_path());
                fs::copy_symlink(walk->path(), target);
            } else if (status.type() == fs::file_type::regular) {
                fs::create_directories(target.parent_path());
                link_or_copy(walk->path(), target, before_copy, cancel);
            }
        }
        if (error) {
            throw fs::filesystem_error("could not list the runtime", source, error);
        }
    };
    store.install_tree(component_id, *entry, fill, cancel);
    kLog.info("Adopted component '{}' from {}", component_id, legacy_identity().product);
}

void OpenWhisperAdopter::adopt_model(std::string_view key, const CancelToken& cancel) {
    const Discovery found = discover();
    const auto it = std::ranges::find(found.models, key, &AdoptableModel::key);
    if (it == found.models.end()) {
        throw AdoptionError(std::format("{}'s copy of {} can't be used: it is missing or "
                                        "incomplete.",
                                        legacy_identity().product, key));
    }
    const fs::path source = it->source;
    SpeechModelCache cache(speech_models_root_, detail::no_download_client(), cache_options());
    const Json pinned = spec(key);
    const auto fill = [&](const fs::path& staging) {
        for (const Json& file : pinned.at("files")) {
            const fs::path name = from_utf8(file.at("name").get<std::string>());
            link_or_copy(source / name, staging / name, {}, cancel);
        }
    };
    try {
        cache.install_local(key, fill,
                            std::format("{}'s copy of {} failed its integrity check.",
                                        legacy_identity().product, it->label),
                            cancel);
    } catch (const ModelCacheError& error) {
        throw AdoptionError(error.what());
    }
    kLog.info("Adopted speech model '{}' from {}", key, legacy_identity().product);
}

void OpenWhisperAdopter::adopt_speaker_model(const CancelToken& cancel) {
    const Discovery found = discover();
    if (!found.speaker_model) {
        throw AdoptionError(std::format("{}'s copy of the speaker model can't be used: it is "
                                        "missing or incomplete.",
                                        legacy_identity().product));
    }
    const fs::path folder = options_.speaker_model_dir;
    const fs::path target = folder / from_utf8(kSpeakerModelFilename);
    // Not ".onnx": speaker_model_path never takes a file that is still being checked.
    fs::path staging = target;
    staging += ".adopting";
    if (cancel.cancelled()) {
        throw ComponentCanceled();
    }
    fs::create_directories(folder);
    download::discard_regular_file(staging);
    // Whatever stops the adoption (a cancel, a mismatch, a disk error while copying, hashing or
    // renaming) leaves no staging file behind.
    try {
        link_or_copy(*found.speaker_model, staging, {}, cancel);
        if (cancel.cancelled()) {
            throw ComponentCanceled();
        }
        // The bytes checked are the ones adopted: a link is OpenWhisper's file, a copy is ours.
        if (detail::sha256_cancellable(staging, cancel) != speaker_sha256()) {
            remember_rejected_speaker_model(*found.speaker_model);
            throw AdoptionError(std::format("{}'s copy of the speaker model failed its integrity "
                                            "check.",
                                            legacy_identity().product));
        }
        fs::rename(staging, target);
    } catch (...) {
        download::discard_regular_file(staging);
        throw;
    }
    kLog.info("Adopted the speaker model from {}", legacy_identity().product);
}

std::string OpenWhisperAdopter::import_model_file(const fs::path& file, const CancelToken& cancel) {
    std::error_code error;
    const auto size = fs::is_regular_file(file, error) ? fs::file_size(file, error) : 0;
    if (error || size == 0) {
        throw AdoptionError(import_refusal());
    }
    std::optional<std::string> digest;
    for (const SpeechModel& entry : models()) {
        const Json pinned = spec(entry.key);
        const Json& files = pinned.at("files");
        if (files.size() != 1 ||
            files[0].at("size_bytes").get<std::int64_t>() != static_cast<std::int64_t>(size)) {
            continue;
        }
        if (!digest) {
            digest = detail::sha256_cancellable(file, cancel);
        }
        if (*digest != files[0].at("sha256").get<std::string>()) {
            continue;
        }
        const fs::path name = from_utf8(files[0].at("name").get<std::string>());
        SpeechModelCache cache(speech_models_root_, detail::no_download_client(), cache_options());
        try {
            cache.install_local(
                entry.key,
                [&](const fs::path& staging) { link_or_copy(file, staging / name, {}, cancel); },
                import_refusal(), cancel);
        } catch (const ModelCacheError& failure) {
            throw AdoptionError(failure.what());
        }
        kLog.info("Imported speech model '{}' from a file", entry.key);
        return std::string(entry.key);
    }
    throw AdoptionError(import_refusal());
}

AdoptionReport OpenWhisperAdopter::adopt_missing(const Json& settings, const CancelToken& cancel) {
    AdoptionReport report;
    const Discovery found = discover();
    if (found.empty() && !found.speaker_model) {
        return report;
    }
    if (keys::fv_runtime_dir.read(settings).empty()) {
        Host would_be = host_;
        would_be.is_installed = [this, &found](std::string_view component_id) {
            return installed_here(component_id) ||
                   std::ranges::find(found.components, component_id,
                                     &AdoptableComponent::component_id) != found.components.end();
        };
        const std::string component = wanted_runtime(settings, would_be);
        if (!installed_here(component) &&
            std::ranges::find(found.components, component, &AdoptableComponent::component_id) !=
                found.components.end()) {
            try {
                adopt_component(component, cancel);
                report.components.push_back(component);
            } catch (const ComponentCanceled&) {
                kLog.info("Stopped adopting component '{}'", component);
                return report;
            } catch (const std::exception& failure) {
                kLog.error_with(failure, std::format("Could not adopt component '{}'", component));
            }
        }
    }
    // Quit cancels: what was not adopted yet is left for the next start, which finds it again.
    if (cancel.cancelled()) {
        return report;
    }
    const std::string key = selected_model(settings, "parakeet");
    if (std::ranges::find(found.models, key, &AdoptableModel::key) != found.models.end()) {
        try {
            adopt_model(key, cancel);
            report.models.push_back(key);
        } catch (const ComponentCanceled&) {
            kLog.info("Stopped adopting speech model '{}'", key);
            return report;
        } catch (const std::exception& failure) {
            kLog.error_with(failure, std::format("Could not adopt speech model '{}'", key));
        }
    }
    if (found.speaker_model && !cancel.cancelled()) {
        try {
            adopt_speaker_model(cancel);
            report.speaker_model = true;
        } catch (const ComponentCanceled&) {
            kLog.info("Stopped adopting the speaker model");
        } catch (const std::exception& failure) {
            kLog.error_with(failure, "Could not adopt the speaker model");
        }
    }
    return report;
}

Discovery discover_openwhisper(const Paths& paths) {
    return OpenWhisperAdopter(paths).discover();
}

void adopt_component(const Paths& paths, std::string_view component_id, const CancelToken& cancel) {
    OpenWhisperAdopter(paths).adopt_component(component_id, cancel);
}

void adopt_model(const Paths& paths, std::string_view key, const CancelToken& cancel) {
    OpenWhisperAdopter(paths).adopt_model(key, cancel);
}

void adopt_speaker_model(const Paths& paths, const CancelToken& cancel) {
    OpenWhisperAdopter(paths).adopt_speaker_model(cancel);
}

std::string import_model_file(const Paths& paths, const fs::path& file, const CancelToken& cancel) {
    return OpenWhisperAdopter(paths).import_model_file(file, cancel);
}

} // namespace fv::models
