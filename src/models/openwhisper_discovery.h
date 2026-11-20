#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "base/paths.h"
#include "models/catalog.h"
#include "models/component_store.h"
#include "models/speech_model_cache.h"

// Reusing the speech runtimes and models an OpenWhisper install already downloaded (architecture
// 7.6). OpenWhisper's components\ and speech-models\ (and its models\speaker-id\ speaker model)
// are only read: a tree is offered when its sentinel, manifest and installed.json validate and
// its pins match this build's catalog, and adoption hard-links the files into FioriVoce's own
// folders (copying across volumes), so OpenWhisper keeps removing or replacing its own copy
// freely. The embedded Python of OpenWhisper's Windows runtime is not taken. Blocking: never on
// the GUI thread.
namespace fv::models {

// Adoption or an import cannot go ahead; the message is for the user.
class AdoptionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct AdoptableComponent {
    std::string component_id;
    std::filesystem::path source; // <OpenWhisper>/components/<id>
    Json version;                 // the OpenWhisper manifest's version
};

struct AdoptableModel {
    std::string key;
    std::string label;
    std::filesystem::path source; // <OpenWhisper>/speech-models/<key>
    std::int64_t size_bytes = 0;
};

struct Discovery {
    std::vector<AdoptableComponent> components;
    std::vector<AdoptableModel> models;
    // <OpenWhisper>/models/speaker-id/voxceleb_resnet34_LM.onnx at the pinned size, when
    // FioriVoce has no speaker model yet and that very file (path, size, modification time) has
    // not failed the integrity check before. Not counted by empty(), which is about the engine.
    std::optional<std::filesystem::path> speaker_model;
    bool empty() const { return components.empty() && models.empty(); }
};

// What adopt_missing did.
struct AdoptionReport {
    std::vector<std::string> components; // ids adopted
    std::vector<std::string> models;     // keys adopted
    bool speaker_model = false;          // the speaker model was adopted
};

struct AdopterOptions {
    // Makes `to` the same file as `from`; empty: std::filesystem::create_hard_link. When it
    // throws (another volume, a file system without hard links), the file is copied.
    std::function<void(const std::filesystem::path& from, const std::filesystem::path& to)>
        link_file;
    // Free bytes of a drive, for the check made before a runtime is copied instead of linked
    // (ComponentStoreOptions::free_bytes); empty: the real figure.
    std::function<std::uint64_t(const std::filesystem::path&)> free_bytes;
    // The pinned spec of a model; empty: artifacts().
    std::function<Json(std::string_view key)> artifacts;
    // FioriVoce's speaker-model folder (speaker_model_cache_dir); empty: the speaker model is
    // neither discovered nor adopted.
    std::filesystem::path speaker_model_dir;
    // The speaker model's pinned SHA-256 and size; empty and 0: kSpeakerModelSha256 and
    // kSpeakerModelSizeBytes.
    std::string speaker_model_sha256;
    std::int64_t speaker_model_size_bytes = 0;
};

// Affinity: one thread at a time.
class OpenWhisperAdopter {
public:
    // FioriVoce's components and speech-models roots, and the OpenWhisper data roots whose
    // components\ and speech-models\ are searched, first match winning.
    OpenWhisperAdopter(std::filesystem::path components_root,
                       std::filesystem::path speech_models_root,
                       std::vector<std::filesystem::path> openwhisper_roots, Host host,
                       AdopterOptions options = {});
    // From the app's paths (the speaker-model folder included) and this computer.
    explicit OpenWhisperAdopter(const Paths& paths);
    OpenWhisperAdopter(const OpenWhisperAdopter&) = delete;
    OpenWhisperAdopter& operator=(const OpenWhisperAdopter&) = delete;

    // What can be adopted that FioriVoce does not have yet: runtimes of this build's catalog for
    // this platform whose OpenWhisper tree is complete (sentinel), has a valid and compatible
    // manifest pinning the same archive SHA-256 as the catalog, and holds the runtime library;
    // models whose installed.json equals the pinned spec with every file at its pinned size.
    Discovery discover() const;

    // Adopts one discovered runtime, links its files (not the embedded Python, nor OpenWhisper's
    // manifest and sentinel) and writes this build's own manifest and sentinel. Throws
    // AdoptionError when the id is not adoptable, ComponentError when the install fails.
    void adopt_component(std::string_view component_id, const CancelToken& cancel = {});

    // Adopts one discovered model; each file's SHA-256 is checked against the pin before the
    // folder is published. Throws AdoptionError when the key is not adoptable or a file does not
    // match.
    void adopt_model(std::string_view key, const CancelToken& cancel = {});

    // Takes a model file the user picked: it must be byte for byte a pinned model file (size,
    // then SHA-256); it is linked or copied into speech-models/<key>/ with installed.json.
    // Returns the key. Throws AdoptionError "This file isn't a model FioriVoce knows. Pick
    // Parakeet TDT 0.6B v3 or Orukeet TDT 0.6B." otherwise.
    std::string import_model_file(const std::filesystem::path& file,
                                  const CancelToken& cancel = {});

    // Adopts the discovered speaker model: linked (or copied) beside its final name, checked
    // against the pinned SHA-256, then renamed into the speaker-model folder. Throws
    // AdoptionError when it is not adoptable or does not match; a file that does not match is
    // remembered (a record beside the speaker-model folder), so later discoveries do
    // not offer it, and hash its 26 MB, again until it changes.
    void adopt_speaker_model(const CancelToken& cancel = {});

    // Adopts what resolve_parakeet would need and FioriVoce lacks: the runtime it would pick if
    // the discovered ones were installed, and the selected model; and the speaker model.
    // Failures are logged and skipped.
    AdoptionReport adopt_missing(const Json& settings, const CancelToken& cancel = {});

private:
    // `before_copy` runs before a file is copied because it could not be linked; a copy stops
    // within a block once `cancel` is (ComponentCanceled), removing what it wrote.
    void link_or_copy(const std::filesystem::path& from, const std::filesystem::path& to,
                      const std::function<void()>& before_copy = {},
                      const CancelToken& cancel = {}) const;
    bool installed_here(std::string_view component_id) const;
    ComponentStoreOptions store_options() const;
    SpeechModelCacheOptions cache_options() const;
    Json spec(std::string_view key) const;
    std::string speaker_sha256() const;
    bool rejected_speaker_model(const std::filesystem::path& file) const;
    void remember_rejected_speaker_model(const std::filesystem::path& file) const;
    std::int64_t speaker_size_bytes() const;

    std::filesystem::path components_root_;
    std::filesystem::path speech_models_root_;
    std::vector<std::filesystem::path> openwhisper_roots_;
    Host host_;
    AdopterOptions options_;
};

// OpenWhisperAdopter(paths) shorthands.
Discovery discover_openwhisper(const Paths& paths);
void adopt_component(const Paths& paths, std::string_view component_id,
                     const CancelToken& cancel = {});
void adopt_model(const Paths& paths, std::string_view key, const CancelToken& cancel = {});
void adopt_speaker_model(const Paths& paths, const CancelToken& cancel = {});
std::string import_model_file(const Paths& paths, const std::filesystem::path& file,
                              const CancelToken& cancel = {});

} // namespace fv::models
