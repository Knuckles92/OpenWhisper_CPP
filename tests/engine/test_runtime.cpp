#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "engine/error.h"
#include "engine/platform.h"
#include "engine/runtime.h"
#include "engine/shared_library.h"

using Catch::Matchers::ContainsSubstring;

namespace {

// A fresh folder under the system temp folder, removed with the object.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ =
            std::filesystem::temp_directory_path() /
            ("fv-test-" +
             std::to_string(std::hash<std::string>{}(std::filesystem::current_path().string())) +
             "-" + std::to_string(counter++));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void touch(const std::filesystem::path& file) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "x";
}

} // namespace

TEST_CASE("the runtime library is found in each release layout") {
    for (const char* folder : {"bin", "lib", "nemo-speech/lib", ""}) {
        const TempDir runtime;
        const auto library = runtime.path() / folder / fv::runtime_library_name();
        touch(library);
        CHECK(std::filesystem::equivalent(fv::find_runtime_library(runtime.path()), library));
    }
}

TEST_CASE("a folder without the runtime is reported by name") {
    const TempDir empty;
    CHECK_THROWS_WITH(fv::find_runtime_library(empty.path()),
                      ContainsSubstring(fv::path_to_utf8(empty.path())) &&
                          ContainsSubstring(fv::runtime_library_name()));
}

TEST_CASE("loading a library that is not one fails with its path") {
    const TempDir dir;
    const auto fake = dir.path() / fv::runtime_library_name();
    touch(fake);
    CHECK_THROWS_WITH(fv::SharedLibrary(fake), ContainsSubstring("Could not load") &&
                                                   ContainsSubstring(fv::path_to_utf8(fake)));
}

TEST_CASE("paths round-trip through UTF-8") {
    const std::string name = "mod\xC3\xA8le \xE2\x9C\x93.gguf"; // "modèle ✓.gguf"
    CHECK(fv::path_to_utf8(fv::path_from_utf8(name)) == name);
}
