// Adoption of OpenWhisper's speaker model (models/speaker-id/voxceleb_resnet34_LM.onnx): found by
// name and size, hard-linked, checked against the pinned digest, then put in place. Built in a
// temporary folder with a 64-byte stand-in whose digest and size the test pins.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "base/cancel.h"
#include "base/hash.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "models/catalog.h"
#include "models/openwhisper_discovery.h"
#include "models/speaker_model.h"
#include "models_test_support.h"

using namespace fv::models;
using fv::Json;
using fv::models::test::host_for;
using fv::models::test::read_file;
using fv::models::test::TempDir;
using fv::models::test::write_file;
namespace fs = std::filesystem;

namespace {

const std::string kModelBytes(64, 's');

struct Setup {
    TempDir temp;
    fs::path ow = temp.path() / "OpenWhisper";
    fs::path speaker_dir = temp.path() / "OpenWhisper_CPP" / "models" / "speaker-id";
    AdopterOptions options;
    std::vector<std::pair<fs::path, fs::path>> links;

    Setup() {
        options.speaker_model_dir = speaker_dir;
        options.speaker_model_sha256 = fv::sha256_hex(kModelBytes);
        options.speaker_model_size_bytes = static_cast<std::int64_t>(kModelBytes.size());
        options.link_file = [this](const fs::path& from, const fs::path& to) {
            links.emplace_back(from, to);
            fs::create_hard_link(from, to);
        };
    }

    OpenWhisperAdopter adopter() {
        return OpenWhisperAdopter(temp.path() / "OpenWhisper_CPP" / "components",
                                  temp.path() / "OpenWhisper_CPP" / "speech-models",
                                  {temp.path() / "missing", ow}, host_for(std::string("win_amd64")),
                                  options);
    }

    fs::path openwhisper_model(const std::string& bytes = kModelBytes) {
        const fs::path file = ow / "models" / "speaker-id" / "voxceleb_resnet34_LM.onnx";
        write_file(file, bytes);
        return file;
    }
    fs::path adopted() const { return speaker_dir / "voxceleb_resnet34_LM.onnx"; }
};

} // namespace

TEST_CASE("OpenWhisper's speaker model is discovered and adopted by hard link", "[models]") {
    Setup setup;
    const fs::path source = setup.openwhisper_model();
    auto adopter = setup.adopter();
    const auto found = adopter.discover();
    REQUIRE(found.speaker_model);
    CHECK(*found.speaker_model == source);
    // The engine's adoption (the runtime setup dialog) is not about the speaker model.
    CHECK(found.empty());

    adopter.adopt_speaker_model();
    CHECK(read_file(setup.adopted()) == kModelBytes);
    REQUIRE(setup.links.size() == 1);
    CHECK(setup.links[0].first == source);
    CHECK(fs::equivalent(setup.adopted(), source));
    CHECK(fs::hard_link_count(source) == 2);
    // OpenWhisper's copy is untouched, and nothing but the model is left in the folder.
    CHECK(read_file(source) == kModelBytes);
    std::vector<fs::path> left;
    for (const auto& entry : fs::directory_iterator(setup.speaker_dir)) {
        left.push_back(entry.path().filename());
    }
    CHECK(left == std::vector<fs::path>{"voxceleb_resnet34_LM.onnx"});
    // Once OpenWhisper_CPP has one, there is nothing more to discover.
    CHECK_FALSE(adopter.discover().speaker_model);
}

TEST_CASE("a speaker model of another size, or none, is not offered", "[models]") {
    Setup setup;
    CHECK_FALSE(setup.adopter().discover().speaker_model);
    setup.openwhisper_model(std::string(65, 's'));
    CHECK_FALSE(setup.adopter().discover().speaker_model);
    CHECK_THROWS_AS(setup.adopter().adopt_speaker_model(), AdoptionError);
}

TEST_CASE("OpenWhisper_CPP's own speaker model is never replaced", "[models]") {
    Setup setup;
    setup.openwhisper_model();
    write_file(setup.speaker_dir / "mine.onnx", "mine");
    CHECK_FALSE(setup.adopter().discover().speaker_model);
}

TEST_CASE("without a speaker-model folder the speaker model is left alone", "[models]") {
    Setup setup;
    setup.openwhisper_model();
    setup.options.speaker_model_dir.clear();
    auto adopter = setup.adopter();
    CHECK_FALSE(adopter.discover().speaker_model);
    const auto report = adopter.adopt_missing(Json::object());
    CHECK_FALSE(report.speaker_model);
}

TEST_CASE("a speaker model that fails its digest is not adopted", "[models]") {
    Setup setup;
    setup.openwhisper_model(std::string(64, 'x'));
    auto adopter = setup.adopter();
    REQUIRE(adopter.discover().speaker_model);
    try {
        adopter.adopt_speaker_model();
        FAIL_CHECK("adopted a file that does not match the pin");
    } catch (const AdoptionError& error) {
        CHECK(std::string(error.what()) ==
              "OpenWhisper's copy of the speaker model failed its integrity check.");
    }
    CHECK_FALSE(fs::exists(setup.adopted()));
    CHECK(fs::is_empty(setup.speaker_dir));
    // Pinned for every platform: LOCALAPPDATA alone is the real ~/.local/share on Linux.
    const std::string per_user = fv::base_utf8::path_to_utf8(setup.temp.path());
    CHECK_FALSE(speaker_model_path(
        fv::Paths::resolve(fv::identity(), fv::MapEnvReader{{"LOCALAPPDATA", per_user},
                                                            {"XDG_DATA_HOME", per_user},
                                                            {"HOME", per_user},
                                                            {"USERPROFILE", per_user}}),
        fv::MapEnvReader{}));
}

TEST_CASE("a cancelled or failed speaker-model adoption leaves no staging file", "[models]") {
    Setup setup;
    const fs::path source = setup.openwhisper_model();

    SECTION("cancelled once the file is linked") {
        fv::CancelSource cancel;
        setup.options.link_file = [&cancel](const fs::path& from, const fs::path& to) {
            fs::create_hard_link(from, to);
            cancel.cancel();
        };
        CHECK_THROWS_AS(setup.adopter().adopt_speaker_model(cancel.token()), ComponentCanceled);
    }
    SECTION("cancelled before it starts") {
        fv::CancelSource cancel;
        cancel.cancel();
        CHECK_THROWS_AS(setup.adopter().adopt_speaker_model(cancel.token()), ComponentCanceled);
        CHECK(setup.links.empty());
    }
    SECTION("the final name is taken by a folder") {
        fs::create_directories(setup.adopted() / "in-the-way");
        CHECK_THROWS(setup.adopter().adopt_speaker_model());
        fs::remove_all(setup.adopted());
    }
    std::vector<fs::path> left;
    if (fs::exists(setup.speaker_dir)) {
        for (const auto& entry : fs::directory_iterator(setup.speaker_dir)) {
            left.push_back(entry.path().filename());
        }
    }
    CHECK(left.empty());
    CHECK(read_file(source) == kModelBytes);
}

TEST_CASE("adopt_missing adopts the speaker model with the engine's files", "[models]") {
    Setup setup;
    setup.openwhisper_model();
    auto adopter = setup.adopter();
    const auto report = adopter.adopt_missing(Json::object());
    CHECK(report.speaker_model);
    CHECK(report.components.empty());
    CHECK(report.models.empty());
    CHECK(read_file(setup.adopted()) == kModelBytes);

    const auto again = adopter.adopt_missing(Json::object());
    CHECK_FALSE(again.speaker_model);
}

TEST_CASE("the app's adopter puts the speaker model where speaker_model_path looks", "[models]") {
    TempDir temp;
    const std::string root = fv::base_utf8::path_to_utf8(temp.path());
    const fv::MapEnvReader env{{"LOCALAPPDATA", root},
                               {"USERPROFILE", root},
                               {"HOME", root},
                               {"XDG_DATA_HOME", root},
                               {"OPENWHISPER_DATA_DIR", root + "/OpenWhisper"}};
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
    // The real pin: OpenWhisper's file must be the pinned size to be offered at all.
    write_file(temp.path() / "OpenWhisper" / "models" / "speaker-id" / "voxceleb_resnet34_LM.onnx",
               "too small");
    CHECK_FALSE(discover_openwhisper(paths).speaker_model);
    CHECK_THROWS_AS(adopt_speaker_model(paths), AdoptionError);
    CHECK(speaker_model_cache_dir(paths) ==
          temp.path() / "OpenWhisper_CPP" / "models" / "speaker-id");
}

TEST_CASE("a speaker model that failed its digest is not offered again until it changes",
          "[models]") {
    Setup setup;
    const fs::path source = setup.openwhisper_model(std::string(64, 'x'));
    REQUIRE(setup.adopter().discover().speaker_model);
    CHECK_THROWS_AS(setup.adopter().adopt_speaker_model(), AdoptionError);
    REQUIRE(setup.links.size() == 1);

    // The next start's discovery skips that file, so it is neither hashed nor logged again.
    CHECK_FALSE(setup.adopter().discover().speaker_model);
    const auto report = setup.adopter().adopt_missing(Json::object());
    CHECK_FALSE(report.speaker_model);
    CHECK(setup.links.size() == 1);
    CHECK(fs::is_empty(setup.speaker_dir));

    // OpenWhisper replaces it with the right file: offered and adopted.
    write_file(source, kModelBytes);
    fs::last_write_time(source, fs::last_write_time(source) + std::chrono::seconds(5));
    REQUIRE(setup.adopter().discover().speaker_model);
    setup.adopter().adopt_speaker_model();
    CHECK(read_file(setup.adopted()) == kModelBytes);
}
