// Quit during the adoption of an OpenWhisper model: the copy stops within a block, nothing half
// adopted is left (no staging folder, no model), and the next start adopts the model whole.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "base/cancel.h"
#include "models/catalog.h"
#include "models/detail/files.h"
#include "models/openwhisper_discovery.h"
#include "models/testing/fake_openwhisper.h"
#include "models_test_support.h"

using namespace fv::models;
using namespace fv::models::test;
using fv::Json;

TEST_CASE("sweep: Quit during a model's adoption leaves nothing half adopted", "[models]") {
    TempDir temp;
    const fs::path ow = temp.path() / "OpenWhisper";
    const fs::path models = temp.path() / "OpenWhisper_CPP" / "speech-models";
    const std::string weights(3 * 1024 * 1024, 'w');
    const Json spec = testing::small_model_spec("parakeet.gguf", weights);
    testing::write_openwhisper_model(ow, "parakeet-v3", spec, {{"parakeet.gguf", weights}});

    fv::CancelSource quit;
    AdopterOptions options;
    options.artifacts = [&spec](std::string_view) {
        return spec;
    };
    // Another volume: the file is copied, and Quit comes as the copy starts.
    options.link_file = [&quit](const fs::path&, const fs::path&) {
        quit.cancel();
        throw std::runtime_error("another volume");
    };
    Host host;
    host.platform = "win_amd64";
    {
        OpenWhisperAdopter adopter(temp.path() / "OpenWhisper_CPP" / "components", models, {ow},
                                   host, options);
        const AdoptionReport report = adopter.adopt_missing(Json::object(), quit.token());
        CHECK(report.models.empty());
    }
    CHECK_FALSE(fs::exists(models / "parakeet-v3"));
    CHECK_FALSE(fs::exists(models / "parakeet-v3.partial"));

    // The next start (nothing cancels it) adopts the model whole.
    options.link_file = [](const fs::path&, const fs::path&) {
        throw std::runtime_error("another volume");
    };
    OpenWhisperAdopter adopter(temp.path() / "OpenWhisper_CPP" / "components", models, {ow}, host,
                               options);
    const AdoptionReport report = adopter.adopt_missing(Json::object(), fv::CancelToken());
    CHECK(report.models == std::vector<std::string>{"parakeet-v3"});
    CHECK(read_file(models / "parakeet-v3" / "parakeet.gguf") == weights);
}

TEST_CASE("sweep: a cancelled copy stops and leaves no file behind", "[models]") {
    TempDir temp;
    const fs::path from = temp.path() / "big.bin";
    {
        std::ofstream out(from, std::ios::binary);
        out << std::string(9 * 1024 * 1024, 'x');
    }
    const fs::path to = temp.path() / "copy.bin";
    fv::CancelSource cancel;
    cancel.cancel();
    CHECK_THROWS_AS(detail::copy_file_cancellable(from, to, cancel.token()), ComponentCanceled);
    CHECK_FALSE(fs::exists(to));
    detail::copy_file_cancellable(from, to, fv::CancelToken());
    CHECK(fs::file_size(to) == 9 * 1024 * 1024);
}
