// Checklist line SE-12 of docs/port/map/a32-user-facing-features.md: the speech runtime, once
// loaded, stays loaded until exit (FreeLibrary on NeMo-Speech.cpp 0.1.0 crashes the process).
// Needs FIORIVOCE_TEST_RUNTIME_DIR (an unpacked NeMo-Speech.cpp release); skipped without it.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "engine/nemo_api.h"
#include "engine/platform.h"
#include "engine/runtime.h"

TEST_CASE("a loaded runtime is loaded once and never released", "[decode]") {
    const auto runtime = fv::env_utf8("FIORIVOCE_TEST_RUNTIME_DIR");
    if (!runtime) {
        SKIP("FIORIVOCE_TEST_RUNTIME_DIR is not set");
    }
    const std::filesystem::path library = fv::find_runtime_library(fv::path_from_utf8(*runtime));
    const fv::NemoApi* first = &fv::NemoApi::get(library);
    REQUIRE(first->version != nullptr);
    // Another spelling of the same file reaches the same, still bound, table: nothing was
    // unloaded in between and no second copy was loaded.
    const std::filesystem::path respelled = library.parent_path() / "." / library.filename();
    const fv::NemoApi* second = &fv::NemoApi::get(respelled);
    CHECK(second == first);
    CHECK(second->version == first->version);
    CHECK(second->library.path() == first->library.path());
}
