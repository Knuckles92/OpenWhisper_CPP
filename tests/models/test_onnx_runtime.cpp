// Where ONNX Runtime is found: the variable naming a file, then the onnxruntime-cpu component, and
// that component's unpublished catalog entry.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/component_store.h"
#include "models/onnx_runtime.h"
#include "models_test_support.h"

using namespace fv::models;
using fv::Json;
using fv::models::test::TempDir;
using fv::models::test::write_file;
namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& path) {
    return fv::base_utf8::path_to_utf8(path);
}

struct Rig {
    TempDir temp;
    fv::MapEnvReader env;
    fv::http::FakeHttpClient client;
    ComponentStore store{temp.path() / "components", client, [] {
                             ComponentStoreOptions options;
                             options.host = test::host_for(std::string(platform::kWinAmd64));
                             return options;
                         }()};

    fs::path component() const { return store.component_dir(kOnnxRuntimeComponent); }
    // A complete component tree (its sentinel last) holding `library`.
    void install(const fs::path& library) {
        write_file(library, "dll");
        write_file(component() / "manifest.json", "{}");
        write_file(component() / ".installed", "1.29.0");
    }
};

} // namespace

TEST_CASE("the onnxruntime-cpu component is in the catalog but not published", "[onnx_runtime]") {
    const auto entry =
        catalog_entry(kOnnxRuntimeComponent, platform::kWinAmd64, onnxruntime_catalog());
    REQUIRE(entry);
    CHECK(entry->at("platform") == "win_amd64");
    CHECK(entry->at("published") == false);
    CHECK_FALSE(component_is_published(*entry));
    CHECK_FALSE(
        component_is_published(kOnnxRuntimeComponent, platform::kWinAmd64, onnxruntime_catalog()));
    CHECK_FALSE(
        catalog_entry(kOnnxRuntimeComponent, platform::kLinuxX86_64, onnxruntime_catalog()));
    // The speech runtimes' catalog does not offer it.
    CHECK_FALSE(catalog_entry(kOnnxRuntimeComponent, platform::kWinAmd64));
}

TEST_CASE("nothing is found without the variable or the component", "[onnx_runtime]") {
    Rig rig;
    CHECK_FALSE(locate_onnxruntime(rig.env, rig.store));
}

TEST_CASE("OPENWHISPER_CPP_ONNXRUNTIME names the library", "[onnx_runtime]") {
    Rig rig;
    const fs::path library = rig.temp.path() / "ort" / "onnxruntime.dll";
    write_file(library, "dll");
    // The component's library has this platform's name (libonnxruntime.so on Linux).
    const fs::path installed =
        rig.component() / "lib" / fv::base_utf8::path_from_utf8(kOnnxRuntimeLibrary);
    rig.install(installed);
    rig.env.set("OPENWHISPER_CPP_ONNXRUNTIME", " " + utf8(library) + " ");
    CHECK(locate_onnxruntime(rig.env, rig.store) == library);

    // A variable naming nothing is skipped for the component.
    rig.env.set("OPENWHISPER_CPP_ONNXRUNTIME", utf8(rig.temp.path() / "missing.dll"));
    CHECK(locate_onnxruntime(rig.env, rig.store) == installed);
    // A folder is not a library.
    rig.env.set("OPENWHISPER_CPP_ONNXRUNTIME", utf8(rig.temp.path() / "ort"));
    CHECK(locate_onnxruntime(rig.env, rig.store) == installed);
    // Neither is an empty value.
    rig.env.set("OPENWHISPER_CPP_ONNXRUNTIME", "   ");
    CHECK(locate_onnxruntime(rig.env, rig.store) == installed);
}

TEST_CASE("the installed component's library is found in lib or at its top", "[onnx_runtime]") {
    Rig rig;
    const fs::path library = fv::base_utf8::path_from_utf8(kOnnxRuntimeLibrary);
    SECTION("lib") {
        rig.install(rig.component() / "lib" / library);
        CHECK(locate_onnxruntime(rig.env, rig.store) == rig.component() / "lib" / library);
    }
    SECTION("top") {
        rig.install(rig.component() / library);
        CHECK(locate_onnxruntime(rig.env, rig.store) == rig.component() / library);
    }
    SECTION("an install without the library") {
        rig.install(rig.component() / "bin" / "other.dll");
        CHECK_FALSE(locate_onnxruntime(rig.env, rig.store));
    }
}

TEST_CASE("an interrupted install (no sentinel) is not used", "[onnx_runtime]") {
    Rig rig;
    write_file(rig.component() / "lib" / std::string(kOnnxRuntimeLibrary), "dll");
    CHECK_FALSE(locate_onnxruntime(rig.env, rig.store));
}
