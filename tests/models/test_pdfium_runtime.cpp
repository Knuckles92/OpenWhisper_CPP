// Where PDFium is found: the variable naming a file, then the pdfium component, and that
// component's unpublished catalog entry.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "http/testing/fake_http_client.h"
#include "models/catalog.h"
#include "models/component_store.h"
#include "models/pdfium_runtime.h"
#include "models_test_support.h"

using namespace fv::models;
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

    fs::path component() const { return store.component_dir(kPdfiumComponent); }
    // A complete component tree (its sentinel last) holding `library`.
    void install(const fs::path& library) {
        write_file(library, "dll");
        write_file(component() / "manifest.json", "{}");
        write_file(component() / ".installed", "0");
    }
};

} // namespace

TEST_CASE("the pdfium component is in its catalog but not published", "[pdfium_runtime]") {
    const auto entry = catalog_entry(kPdfiumComponent, platform::kWinAmd64, pdfium_catalog());
    REQUIRE(entry);
    CHECK(entry->at("platform") == "win_amd64");
    CHECK(entry->at("published") == false);
    CHECK_FALSE(component_is_published(*entry));
    CHECK_FALSE(component_is_published(kPdfiumComponent, platform::kWinAmd64, pdfium_catalog()));
    CHECK_FALSE(catalog_entry(kPdfiumComponent, platform::kLinuxX86_64, pdfium_catalog()));
    // The speech runtimes' catalog does not offer it.
    CHECK_FALSE(catalog_entry(kPdfiumComponent, platform::kWinAmd64));
}

TEST_CASE("PDFium is not found without the variable or the component", "[pdfium_runtime]") {
    Rig rig;
    CHECK_FALSE(locate_pdfium(rig.env, rig.store));
}

TEST_CASE("OPENWHISPER_CPP_PDFIUM names the library", "[pdfium_runtime]") {
    Rig rig;
    const fs::path library = rig.temp.path() / "pdf" / "pdfium.dll";
    write_file(library, "dll");
    const fs::path installed =
        rig.component() / "bin" / fv::base_utf8::path_from_utf8(kPdfiumLibrary);
    rig.install(installed);
    rig.env.set("OPENWHISPER_CPP_PDFIUM", " " + utf8(library) + " ");
    CHECK(locate_pdfium(rig.env, rig.store) == library);

    // A variable naming nothing is skipped for the component.
    rig.env.set("OPENWHISPER_CPP_PDFIUM", utf8(rig.temp.path() / "missing.dll"));
    CHECK(locate_pdfium(rig.env, rig.store) == installed);
    // Neither is a folder or an empty value.
    rig.env.set("OPENWHISPER_CPP_PDFIUM", utf8(rig.temp.path() / "pdf"));
    CHECK(locate_pdfium(rig.env, rig.store) == installed);
    rig.env.set("OPENWHISPER_CPP_PDFIUM", "   ");
    CHECK(locate_pdfium(rig.env, rig.store) == installed);
}

TEST_CASE("the installed component's library is found in bin, lib or at its top",
          "[pdfium_runtime]") {
    Rig rig;
    const fs::path library = fv::base_utf8::path_from_utf8(kPdfiumLibrary);
    SECTION("bin") {
        rig.install(rig.component() / "bin" / library);
        CHECK(locate_pdfium(rig.env, rig.store) == rig.component() / "bin" / library);
    }
    SECTION("lib") {
        rig.install(rig.component() / "lib" / library);
        CHECK(locate_pdfium(rig.env, rig.store) == rig.component() / "lib" / library);
    }
    SECTION("top") {
        rig.install(rig.component() / library);
        CHECK(locate_pdfium(rig.env, rig.store) == rig.component() / library);
    }
    SECTION("an install without the library") {
        rig.install(rig.component() / "include" / "fpdfview.h");
        CHECK_FALSE(locate_pdfium(rig.env, rig.store));
    }
}

TEST_CASE("an interrupted PDFium install (no sentinel) is not used", "[pdfium_runtime]") {
    Rig rig;
    write_file(rig.component() / "bin" / std::string(kPdfiumLibrary), "dll");
    CHECK_FALSE(locate_pdfium(rig.env, rig.store));
}
