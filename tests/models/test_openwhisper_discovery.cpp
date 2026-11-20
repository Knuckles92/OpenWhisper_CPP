// Discovery and adoption of an OpenWhisper install's runtimes and models (architecture 7.6),
// against OpenWhisper-style trees built in a temporary folder. The real install is only looked at
// by the [platform] test, read-only, and only when FIORIVOCE_TEST_OW_DATA names it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <map>
#include <string>

#include "base/hash.h"
#include "base/utf8.h"
#include "http/testing/fake_http_client.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/openwhisper_discovery.h"
#include "models/speech_catalog.h"
#include "models/testing/fake_openwhisper.h"
#include "models_test_support.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fv::models;
using namespace fv::models::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

const std::string kWeights = "parakeet weights";

struct Setup {
    TempDir temp;
    fs::path ow = temp.path() / "OpenWhisper";
    fs::path components = temp.path() / "FioriVoce" / "components";
    fs::path speech_models = temp.path() / "FioriVoce" / "speech-models";
    Json spec = testing::small_model_spec("parakeet.gguf", kWeights);
    AdopterOptions options;
    std::string platform = "win_amd64";
    std::optional<NvidiaGpu> card;

    Setup() {
        options.artifacts = [this](std::string_view) {
            return spec;
        };
    }

    OpenWhisperAdopter adopter() {
        return OpenWhisperAdopter(components, speech_models, {temp.path() / "missing", ow},
                                  host_for(platform, card), options);
    }

    void openwhisper_cpu_runtime() {
        testing::write_openwhisper_runtime(ow, "asr-nvidia-cpu", platform);
    }
    fs::path openwhisper_model(std::string_view key = "parakeet-v3") {
        return testing::write_openwhisper_model(ow, key, spec, {{"parakeet.gguf", kWeights}});
    }
};

bool has_component(const Discovery& found, std::string_view id) {
    return std::ranges::find(found.components, id, &AdoptableComponent::component_id) !=
           found.components.end();
}

bool has_model(const Discovery& found, std::string_view key) {
    return std::ranges::find(found.models, key, &AdoptableModel::key) != found.models.end();
}

// Every file under `root` with its bytes and modification time: a tree that adoption must leave
// exactly as it was.
std::map<std::string, std::pair<std::string, fs::file_time_type>> snapshot(const fs::path& root) {
    std::map<std::string, std::pair<std::string, fs::file_time_type>> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files[fv::base_utf8::path_to_utf8(entry.path().lexically_relative(root))] = {
                read_file(entry.path()), entry.last_write_time()};
        }
    }
    return files;
}

} // namespace

TEST_CASE("an OpenWhisper runtime and model are discovered and adopted by hard link", "[models]") {
    Setup setup;
    setup.openwhisper_cpu_runtime();
    const auto model_source = setup.openwhisper_model();
    const auto before = snapshot(setup.ow);

    auto adopter = setup.adopter();
    const auto found = adopter.discover();
    REQUIRE(found.components.size() == 1);
    CHECK(found.components[0].component_id == "asr-nvidia-cpu");
    CHECK(found.components[0].source == setup.ow / "components" / "asr-nvidia-cpu");
    CHECK(found.components[0].version == "0.1.0-py312");
    REQUIRE(found.models.size() == 1);
    CHECK(found.models[0].key == "parakeet-v3");
    CHECK(found.models[0].label == "Parakeet TDT 0.6B v3");
    CHECK(found.models[0].size_bytes == static_cast<std::int64_t>(kWeights.size()));

    adopter.adopt_component("asr-nvidia-cpu");
    adopter.adopt_model("parakeet-v3");

    const auto runtime = setup.components / "asr-nvidia-cpu";
    CHECK(fs::hard_link_count(runtime / "bin" / "nemo_speech_asr_c.dll") == 2);
    CHECK(fs::hard_link_count(runtime / "share" / "licenses" / "nemo-speech" / "LICENSE") == 2);
    // The embedded Python stays behind.
    for (const char* name : {"python.exe", "python312.dll", "python312.zip", "python312._pth",
                             "_ctypes.pyd", "vcruntime140.dll", "LICENSE.txt"}) {
        CHECK_FALSE(fs::exists(runtime / name));
    }
    // FioriVoce's own manifest (its catalog entry) and sentinel, not links to OpenWhisper's.
    const Json manifest = fv::py::loads(read_file(runtime / "manifest.json"));
    CHECK(manifest == *catalog_entry("asr-nvidia-cpu", "win_amd64"));
    CHECK(fs::hard_link_count(runtime / "manifest.json") == 1);
    CHECK(read_file(runtime / ".installed") == "0.1.0-py312");

    const auto model = setup.speech_models / "parakeet-v3";
    CHECK(fs::hard_link_count(model / "parakeet.gguf") == 2);
    CHECK(fv::py::loads(read_file(model / "installed.json")) == setup.spec);

    // Adopted, it reads as installed and current, and is no longer offered.
    fv::http::FakeHttpClient client;
    ComponentStoreOptions store_options;
    store_options.host = host_for("win_amd64");
    ComponentStore store(setup.components, client, store_options);
    const auto info = ComponentCoordinator(store).describe("asr-nvidia-cpu");
    CHECK(info.state == ComponentState::Installed);
    CHECK(adopter.discover().empty());

    // OpenWhisper's folders are exactly as they were.
    CHECK(snapshot(setup.ow) == before);
}

TEST_CASE("adoption copies when the files cannot be linked", "[models]") {
    Setup setup;
    setup.options.link_file = [](const fs::path& from, const fs::path& to) {
        throw fs::filesystem_error("not the same device", from, to,
                                   std::make_error_code(std::errc::cross_device_link));
    };
    setup.openwhisper_cpu_runtime();
    setup.openwhisper_model();
    auto adopter = setup.adopter();
    adopter.adopt_component("asr-nvidia-cpu");
    adopter.adopt_model("parakeet-v3");
    const auto library = setup.components / "asr-nvidia-cpu" / "bin" / "nemo_speech_asr_c.dll";
    CHECK(fs::hard_link_count(library) == 1);
    CHECK(read_file(library) == "runtime");
    CHECK(fs::hard_link_count(setup.speech_models / "parakeet-v3" / "parakeet.gguf") == 1);
}

TEST_CASE("an incomplete or foreign OpenWhisper runtime is not offered", "[models]") {
    const auto offered = [](const std::function<void(const fs::path& tree)>& damage) {
        Setup setup;
        setup.openwhisper_cpu_runtime();
        damage(setup.ow / "components" / "asr-nvidia-cpu");
        return has_component(setup.adopter().discover(), "asr-nvidia-cpu");
    };
    CHECK(offered([](const fs::path&) {}));
    // No sentinel: an interrupted install.
    CHECK_FALSE(offered([](const fs::path& tree) { fs::remove(tree / ".installed"); }));
    // A manifest that does not parse, or has the wrong shape.
    CHECK_FALSE(offered([](const fs::path& tree) { write_file(tree / "manifest.json", "{oops"); }));
    CHECK_FALSE(offered([](const fs::path& tree) {
        write_file(tree / "manifest.json", R"({"component_api": true, "version": "1"})");
    }));
    // Another platform's build, or another component API.
    CHECK_FALSE(offered([](const fs::path& tree) {
        Json manifest = fv::py::loads(read_file(tree / "manifest.json"));
        manifest["platform"] = "linux_x86_64";
        write_file(tree / "manifest.json", manifest.dump());
    }));
    CHECK_FALSE(offered([](const fs::path& tree) {
        Json manifest = fv::py::loads(read_file(tree / "manifest.json"));
        manifest["component_api"] = 2;
        write_file(tree / "manifest.json", manifest.dump());
    }));
    // A different release: the NeMo archive's SHA-256 is not the catalog's.
    CHECK_FALSE(offered([](const fs::path& tree) {
        Json manifest = fv::py::loads(read_file(tree / "manifest.json"));
        manifest["archives"][1]["sha256"] = std::string(64, 'b');
        write_file(tree / "manifest.json", manifest.dump());
    }));
    // The runtime library is gone.
    CHECK_FALSE(
        offered([](const fs::path& tree) { fs::remove(tree / "bin" / "nemo_speech_asr_c.dll"); }));
}

TEST_CASE("an incomplete or different OpenWhisper model is not offered", "[models]") {
    const auto offered = [](const std::function<void(Setup&, const fs::path& folder)>& damage) {
        Setup setup;
        const auto folder = setup.openwhisper_model();
        damage(setup, folder);
        return has_model(setup.adopter().discover(), "parakeet-v3");
    };
    CHECK(offered([](Setup&, const fs::path&) {}));
    CHECK_FALSE(
        offered([](Setup&, const fs::path& folder) { fs::remove(folder / "installed.json"); }));
    // installed.json pins another digest.
    CHECK_FALSE(offered([](Setup& setup, const fs::path& folder) {
        Json other = setup.spec;
        other["files"][0]["sha256"] = std::string(64, 'c');
        write_file(folder / "installed.json", fv::py::dumps(other));
    }));
    // A truncated file.
    CHECK_FALSE(
        offered([](Setup&, const fs::path& folder) { write_file(folder / "parakeet.gguf", "x"); }));
}

TEST_CASE("a model whose bytes do not match the pin is not adopted", "[models]") {
    Setup setup;
    const auto folder = setup.openwhisper_model();
    // Same size, different bytes: offered (sizes match), refused at adoption.
    std::string tampered = kWeights;
    tampered[0] = 'P';
    write_file(folder / "parakeet.gguf", tampered);
    auto adopter = setup.adopter();
    REQUIRE(has_model(adopter.discover(), "parakeet-v3"));
    CHECK_THROWS_MATCHES(adopter.adopt_model("parakeet-v3"), AdoptionError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("integrity check")));
    CHECK_FALSE(fs::exists(setup.speech_models / "parakeet-v3"));
    CHECK(read_file(folder / "parakeet.gguf") == tampered);
}

TEST_CASE("only discovered ids and keys are adopted", "[models]") {
    Setup setup;
    auto adopter = setup.adopter();
    CHECK_THROWS_AS(adopter.adopt_component("asr-nvidia-cuda"), AdoptionError);
    CHECK_THROWS_AS(adopter.adopt_model("orukeet-v0.1"), AdoptionError);
}

TEST_CASE("import_model_file takes a pinned model and refuses anything else", "[models]") {
    Setup setup;
    auto adopter = setup.adopter();
    const auto wrong = setup.temp.path() / "picked" / "other.gguf";
    write_file(wrong, "not the model");
    CHECK_THROWS_WITH(adopter.import_model_file(wrong),
                      "This file isn't a model FioriVoce knows. Pick Parakeet TDT 0.6B v3 or "
                      "Orukeet TDT 0.6B.");
    std::string same_size(kWeights.size(), 'z');
    write_file(wrong, same_size);
    CHECK_THROWS_AS(adopter.import_model_file(wrong), AdoptionError);
    CHECK_THROWS_AS(adopter.import_model_file(setup.temp.path() / "missing.gguf"), AdoptionError);
    CHECK_FALSE(fs::exists(setup.speech_models / "parakeet-v3"));

    const auto right = setup.temp.path() / "picked" / "downloaded.gguf";
    write_file(right, kWeights);
    CHECK(adopter.import_model_file(right) == "parakeet-v3");
    const auto installed = setup.speech_models / "parakeet-v3";
    CHECK(read_file(installed / "parakeet.gguf") == kWeights);
    CHECK(fs::hard_link_count(installed / "parakeet.gguf") == 2);
    CHECK(fv::py::loads(read_file(installed / "installed.json")) == setup.spec);
}

TEST_CASE("import_model_file stops hashing once cancelled", "[models]") {
    // The runtime setup window cancels when it closes; the hash of a 714 MB model stops then.
    Setup setup;
    auto adopter = setup.adopter();
    const auto picked = setup.temp.path() / "picked" / "same-size.gguf";
    write_file(picked, std::string(kWeights.size(), 'z')); // a size the catalog pins
    fv::CancelSource cancel;
    cancel.cancel();
    CHECK_THROWS_AS(adopter.import_model_file(picked, cancel.token()), ComponentCanceled);
    CHECK_FALSE(fs::exists(setup.speech_models / "parakeet-v3"));
}

TEST_CASE("adopt_missing takes the runtime the engine would use and the selected model",
          "[models]") {
    Setup setup;
    setup.card = gpu("NVIDIA GeForce RTX 2060", 6144, {{7, 5}});
    testing::write_openwhisper_runtime(setup.ow, "asr-nvidia-cpu", setup.platform);
    testing::write_openwhisper_runtime(setup.ow, "asr-nvidia-cuda", setup.platform);
    setup.openwhisper_model();
    auto adopter = setup.adopter();
    // Auto on a GPU: the CUDA runtime, once it is here; the CPU one is not needed.
    const auto report = adopter.adopt_missing(Json::object());
    CHECK(report.components == std::vector<std::string>{"asr-nvidia-cuda"});
    CHECK(report.models == std::vector<std::string>{"parakeet-v3"});
    CHECK(fs::exists(setup.components / "asr-nvidia-cuda" / ".installed"));
    CHECK_FALSE(fs::exists(setup.components / "asr-nvidia-cpu"));
    // Nothing left that the engine needs.
    const auto again = adopter.adopt_missing(Json::object());
    CHECK(again.components.empty());
    CHECK(again.models.empty());
}

#ifdef _WIN32
TEST_CASE("OpenWhisper can delete its link of a runtime file FioriVoce has mapped",
          "[models][platform]") {
    // Section 13 item 31: FioriVoce never unloads the runtime it loaded from its hard link, and
    // OpenWhisper's swap and rollback must still remove their own link. The file mapped here is
    // a copy of this test executable in a temporary folder, mapped as an image (as the loader
    // maps a DLL) without running any of it.
    TempDir temp;
    std::wstring self(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), MAX_PATH);
    REQUIRE((length > 0 && length < MAX_PATH));
    self.resize(length);
    const auto ow_tree = temp.path() / "OpenWhisper" / "components" / "asr-nvidia-cpu";
    const auto fv_tree = temp.path() / "FioriVoce" / "components" / "asr-nvidia-cpu";
    fs::create_directories(ow_tree / "bin");
    fs::create_directories(fv_tree / "bin");
    const auto ow_link = ow_tree / "bin" / "fv_link_probe.dll";
    const auto fv_link = fv_tree / "bin" / "fv_link_probe.dll";
    fs::copy_file(fs::path(self), ow_link);
    fs::create_hard_link(ow_link, fv_link);
    REQUIRE(fs::hard_link_count(fv_link) == 2);

    HMODULE mapped = LoadLibraryExW(fv_link.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    REQUIRE(mapped != nullptr);
    // OpenWhisper moves its tree aside, then deletes the rollback.
    const auto rollback = fs::path(ow_tree).concat(".old");
    std::error_code error;
    fs::rename(ow_tree, rollback, error);
    CHECK_FALSE(error);
    fs::remove_all(rollback, error);
    CHECK_FALSE(error);
    CHECK_FALSE(fs::exists(rollback));
    CHECK(fs::exists(fv_link));
    CHECK(fs::hard_link_count(fv_link) == 1);
    FreeLibrary(mapped);
}
#endif

TEST_CASE("the installed OpenWhisper runtime and model are found where they exist",
          "[models][platform]") {
    // Read-only: discovery reads manifests and installed.json and checks sizes; nothing under
    // the real folder is opened for writing, linked or copied.
    const auto real = fv::testkit::get_env("FIORIVOCE_TEST_OW_DATA");
    if (!real || real->empty()) {
        SKIP("FIORIVOCE_TEST_OW_DATA is not set");
    }
    TempDir temp;
    OpenWhisperAdopter adopter(temp.path() / "components", temp.path() / "speech-models",
                               {fv::base_utf8::path_from_utf8(*real)}, Host::current({}));
    const auto found = adopter.discover();
    for (const auto& component : found.components) {
        CHECK(fs::exists(component.source / ".installed"));
    }
    for (const auto& model : found.models) {
        CHECK(fs::exists(model.source / "installed.json"));
    }
    if (fs::exists(fv::base_utf8::path_from_utf8(*real) / "components" / "asr-nvidia-cuda" /
                   ".installed")) {
        CHECK((has_component(found, "asr-nvidia-cuda") || has_component(found, "asr-nvidia-cpu")));
    }
    if (fs::exists(fv::base_utf8::path_from_utf8(*real) / "speech-models" / "parakeet-v3" /
                   "installed.json")) {
        CHECK(has_model(found, "parakeet-v3"));
    }
    CHECK_FALSE(fs::exists(temp.path() / "components"));
}

TEST_CASE("a runtime copied instead of linked is refused when the drive lacks room", "[models]") {
    Setup setup;
    setup.options.link_file = [](const fs::path& from, const fs::path& to) {
        throw fs::filesystem_error("not the same device", from, to,
                                   std::make_error_code(std::errc::cross_device_link));
    };
    std::vector<fs::path> asked;
    setup.options.free_bytes = [&asked](const fs::path& drive) {
        asked.push_back(drive);
        return std::uint64_t{1};
    };
    setup.openwhisper_cpu_runtime();
    auto adopter = setup.adopter();
    CHECK_THROWS_WITH(adopter.adopt_component("asr-nvidia-cpu"),
                      ContainsSubstring("Not enough disk space:"));
    CHECK(asked.size() == 1);
    CHECK_FALSE(fs::exists(setup.components / "asr-nvidia-cpu" / ".installed"));

    // Linked files take no room: nothing is asked.
    Setup linked;
    linked.options.free_bytes = [&asked](const fs::path& drive) {
        asked.push_back(drive);
        return std::uint64_t{1};
    };
    linked.openwhisper_cpu_runtime();
    linked.adopter().adopt_component("asr-nvidia-cpu");
    CHECK(asked.size() == 1);
    CHECK(fs::exists(linked.components / "asr-nvidia-cpu" / ".installed"));
}
